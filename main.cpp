#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <cstring>
#include <cstdio>

// Flash storage for progression persistence
// Use last sector of flash (4KB before end of 2MB)
#define FLASH_PROG_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_PROG_MAGIC 0xAB

/**
Resonator Workshop System Computer Card - by Johan Eklund
version 1.2 - 2026-05-06

Four resonating strings using Karplus-Strong synthesis
*/

// Cross-core shared state for YIN pitch detection (Core 0 writes audio, Core 1 computes)
#define YIN_RING_BITS 6
#define YIN_RING_SIZE (1 << YIN_RING_BITS)
static volatile int16_t yinRing[YIN_RING_SIZE];  // audio sample ring buffer
static volatile uint32_t yinRingHead;             // write index (Core 0)
static volatile int32_t yinSharedPitchMV;         // pitch result (Core 1 writes, Core 0 reads)

// YIN circular buffer in SRAM4 scratch RAM (after core1_stack at 0x20040000)
static int16_t* const yinBuf = (int16_t*)0x20040800;

// Delay lookup table for 1V/oct pitch control
// 341 entries per octave, inverse exponential curve
// Base: C1 = 32.7Hz at 48kHz = 1468 samples, scaled by 64
// Higher input = shorter delay = higher pitch
// Formula: delay_vals[i] = 93952 / 2^(i/341)
// Ratio across table = 2.0 (one octave)
static const uint32_t delay_vals[341] = {
    93952, 93761, 93571, 93381, 93191, 93002, 92813, 92625, 92437, 92249,
    92062, 91875, 91688, 91502, 91316, 91131, 90946, 90761, 90577, 90393,
    90209, 90026, 89843, 89661, 89479, 89297, 89116, 88935, 88754, 88574,
    88394, 88214, 88035, 87857, 87678, 87500, 87322, 87145, 86968, 86792,
    86615, 86439, 86264, 86089, 85914, 85739, 85565, 85392, 85218, 85045,
    84872, 84700, 84528, 84356, 84185, 84014, 83844, 83673, 83503, 83334,
    83165, 82996, 82827, 82659, 82491, 82324, 82157, 81990, 81823, 81657,
    81491, 81326, 81161, 80996, 80831, 80667, 80503, 80340, 80177, 80014,
    79852, 79689, 79528, 79366, 79205, 79044, 78884, 78723, 78564, 78404,
    78245, 78086, 77927, 77769, 77611, 77454, 77296, 77139, 76983, 76826,
    76670, 76515, 76359, 76204, 76049, 75895, 75741, 75587, 75434, 75280,
    75128, 74975, 74823, 74671, 74519, 74368, 74217, 74066, 73916, 73766,
    73616, 73466, 73317, 73168, 73020, 72872, 72724, 72576, 72428, 72281,
    72135, 71988, 71842, 71696, 71551, 71405, 71260, 71116, 70971, 70827,
    70683, 70540, 70396, 70253, 70111, 69968, 69826, 69685, 69543, 69402,
    69261, 69120, 68980, 68840, 68700, 68561, 68421, 68282, 68144, 68005,
    67867, 67729, 67592, 67455, 67318, 67181, 67045, 66908, 66773, 66637,
    66502, 66367, 66232, 66097, 65963, 65829, 65696, 65562, 65429, 65296,
    65164, 65031, 64899, 64767, 64636, 64505, 64374, 64243, 64112, 63982,
    63852, 63723, 63593, 63464, 63335, 63207, 63078, 62950, 62822, 62695,
    62568, 62440, 62314, 62187, 62061, 61935, 61809, 61684, 61558, 61433,
    61309, 61184, 61060, 60936, 60812, 60689, 60565, 60442, 60320, 60197,
    60075, 59953, 59831, 59710, 59588, 59467, 59347, 59226, 59106, 58986,
    58866, 58747, 58627, 58508, 58389, 58271, 58153, 58034, 57917, 57799,
    57682, 57564, 57448, 57331, 57215, 57098, 56982, 56867, 56751, 56636,
    56521, 56406, 56292, 56177, 56063, 55949, 55836, 55722, 55609, 55496,
    55384, 55271, 55159, 55047, 54935, 54824, 54712, 54601, 54490, 54380,
    54269, 54159, 54049, 53939, 53830, 53720, 53611, 53503, 53394, 53285,
    53177, 53069, 52962, 52854, 52747, 52640, 52533, 52426, 52320, 52213,
    52107, 52001, 51896, 51790, 51685, 51580, 51476, 51371, 51267, 51163,
    51059, 50955, 50852, 50748, 50645, 50542, 50440, 50337, 50235, 50133,
    50031, 49930, 49828, 49727, 49626, 49525, 49425, 49325, 49224, 49124,
    49025, 48925, 48826, 48727, 48628, 48529, 48430, 48332, 48234, 48136,
    48038, 47941, 47843, 47746, 47649, 47552, 47456, 47360, 47263, 47167,
    47072
};

// Exponential delay lookup for 1V/oct pitch control
// in: 0-4095 (knob + CV combined)
// Returns delay in samples (right-shifted by octave)
int32_t ExpDelay(int32_t in) {
    if (in < 0) in = 0;
    if (in > 4091) in = 4091;
    int32_t oct = in / 341;
    int32_t suboct = in % 341;
    return delay_vals[suboct] >> oct;
}

// Convert a just intonation ratio (num:den) to millivolt offset for 1V/oct CV
// round(1000 * log2(num/den)) precomputed for all ratios in the chord table
// Uses num*256+den as a collision-free key for switch lookup
int32_t ratioToMillivolts(int num, int den) {
    switch (num * 256 + den) {
        case 1*256+1:   return 0;     // 1:1 unison
        case 9*256+8:   return 170;   // 9:8 major 2nd
        case 6*256+5:   return 263;   // 6:5 minor 3rd
        case 5*256+4:   return 322;   // 5:4 major 3rd
        case 4*256+3:   return 415;   // 4:3 perfect 4th
        case 36*256+25: return 526;   // 36:25 dim 5th
        case 3*256+2:   return 585;   // 3:2 perfect 5th
        case 5*256+3:   return 737;   // 5:3 major 6th
        case 9*256+5:   return 848;   // 9:5 minor 7th
        case 15*256+8:  return 907;   // 15:8 major 7th
        case 2*256+1:   return 1000;  // 2:1 octave
        case 9*256+4:   return 1170;  // 9:4 major 9th
        case 5*256+2:   return 1322;  // 5:2 major 10th
        case 3*256+1:   return 1585;  // 3:1 octave + 5th
        case 4*256+1:   return 2000;  // 4:1 two octaves
        default:        return 0;
    }
}

// Convert measured period (in samples) to 1V/oct millivolts
// Reverse lookup into delay_vals table
int32_t periodToMillivolts(int32_t period) {
    if (period < 24) period = 24;      // clamp ~2kHz max
    if (period > 1500) period = 1500;  // clamp ~32Hz min
    // Find octave: largest oct where delay_vals[0] >> oct >= period
    // delay_vals are <<6 scaled, ExpDelay returns delay_vals[sub] >> oct = samples
    int oct = 0;
    while (oct < 11 && (int32_t)(delay_vals[0] >> (oct + 1)) >= period) oct++;
    // Binary search within delay_vals (monotonically decreasing)
    // Compare delay_vals[mid] against period << oct for full precision
    int32_t scaledPeriod = (int32_t)period << oct;
    int lo = 0, hi = 340;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if ((int32_t)delay_vals[mid] > scaledPeriod)
            lo = mid + 1;
        else
            hi = mid;
    }
    int32_t pitchCV = oct * 341 + lo;
    // Reference 2048 (CV In 1 midpoint) so self-patching CV Out → CV In round-trips correctly.
    // X knob becomes transpose (±1 octave) when CV is connected.
    return (pitchCV - 2048) * 12014 >> 12;
}

// Output mode enums
enum CV1Mode  { CV1_ARP=0, CV1_ROOT, CV1_ENVELOPE, CV1_RANDOM_SH, CV1_PITCH_SH, CV1_PITCH_TRACK };
enum CV2Mode  { CV2_RES_ENV=0, CV2_IN_ENV, CV2_ARP, CV2_ROOT, CV2_PITCH_TRACK };
enum P1Mode   { P1_AUDIO_TRIG=0, P1_TAP_CLOCK, P1_ARP_CLOCK, P1_ONSET };
enum P2Mode   { P2_CHORD_TRIG=0, P2_TAP_CLOCK, P2_AUDIO_TRIG, P2_ARP_CLOCK, P2_CLK_DIV, P2_ONSET };
enum PI1Mode  { PI1_PLUCK=0, PI1_ADVANCE, PI1_RESET };
enum PI2Mode  { PI2_ADVANCE=0, PI2_ARP_STEP, PI2_PLUCK, PI2_RESET };

class ResonatingStrings : public ComputerCard
{
private:
    static const int MAX_DELAY_SIZE = 2048;  // power of 2 for & mask (no division)

    int16_t delayLine1[MAX_DELAY_SIZE];
    int16_t delayLine2[MAX_DELAY_SIZE];
    int16_t delayLine3[MAX_DELAY_SIZE];
    int16_t delayLine4[MAX_DELAY_SIZE];

    int writeIndex1;
    int writeIndex2;
    int writeIndex3;
    int writeIndex4;

    int delayLength1;
    int delayLength2;
    int delayLength3;
    int delayLength4;

    int32_t filterState1;
    int32_t filterState2;
    int32_t filterState3;
    int32_t filterState4;

    // Chord modes
    enum ChordMode {
        HARMONIC = 0,    // 1:1, 2:1, 3:1, 4:1 (harmonic series)
        FIFTH = 1,       // 1:1, 3:2, 2:1, 3:1 (stacked fifths)
        MAJOR7 = 2,      // 1:1, 5:4, 3:2, 15:8 (major 7th chord)
        MINOR7 = 3,      // 1:1, 6:5, 3:2, 9:5 (minor 7th chord)
        DIM = 4,         // 1:1, 6:5, 36:25, 3:2 (diminished)
        SUS4 = 5,        // 1:1, 4:3, 3:2, 2:1 (suspended 4th)
        ADD9 = 6,        // 1:1, 5:4, 3:2, 9:4 (major add 9)
        MAJOR10 = 7,     // 1:1, 5:4, 3:2, 5:2 (major chord with 10th)
        SUS2 = 8,        // 1:1, 9:8, 3:2, 2:1 (suspended 2nd)
        MAJOR = 9,       // 1:1, 5:4, 3:2, 2:1 (major triad)
        MINOR = 10,      // 1:1, 6:5, 3:2, 2:1 (minor triad)
        MAJOR6 = 11,     // 1:1, 5:4, 3:2, 5:3 (major 6th)
        DOM7 = 12,       // 1:1, 5:4, 3:2, 9:5 (dominant 7th)
        MIN9 = 13,       // 1:1, 6:5, 3:2, 9:4 (minor add 9)
        TANPURA_PA = 14, // 1:1, 3:2, 2:1, 4:1 (Sa, Pa, Sa', Sa'')
        TANPURA_MA = 15, // 1:1, 4:3, 2:1, 4:1 (Sa, Ma, Sa', Sa'')
        TANPURA_NI = 16, // 1:1, 15:8, 2:1, 4:1 (Sa, Ni, Sa', Sa'')
        TANPURA_NI_KOMAL = 17  // 1:1, 9:5, 2:1, 4:1 (Sa, ni, Sa', Sa'')
    };
    static const int NUM_MODES = 18;
    ChordMode currentMode;

    // Double-buffered progression for lock-free Core0/Core1 sharing
    static const int MAX_PROGRESSION_LENGTH = 18;
    struct ProgressionBuffer {
        ChordMode chords[MAX_PROGRESSION_LENGTH];
        int length;
    };
    volatile int activeBuffer;
    ProgressionBuffer progressionBuffers[2];
    volatile bool progressionChanged;
    volatile bool pendingFlashSave;  // Flag for Core 1 to save flash (Core 0 can't lock out Core 1)
    int progressionIndex;

    bool lastSwitchDown;

    int32_t pulseExciteEnvelope;
    uint32_t noiseState;

    int32_t dcState1, dcState2, dcState3, dcState4;

    // Smoothed delay values for glide between chord modes (fixed-point, 8 bits fraction)
    int32_t smoothDelay1, smoothDelay2, smoothDelay3, smoothDelay4;

    // Long-press reset detection
    uint32_t switchDownCounter;
    bool resetTriggered;
    static const uint32_t RESET_HOLD_SAMPLES = 144000;  // 3 seconds at 48kHz

    // CV and pulse output state
    int arpRotation;           // 0-3, current string for arpeggio output
    int32_t envFollower;       // peak envelope tracker
    bool triggerArmed;         // Schmitt trigger state
    int32_t trigPulseCounter;  // audio trigger pulse countdown
    int prevProgressionIndex;  // for chord-change detection
    int32_t chordPulseCounter; // chord change pulse countdown
    uint32_t chordPeriod;      // samples between chord changes
    uint32_t chordTimer;       // sample counter since last chord change
    uint32_t arpStepCounter;   // counts down to next arpeggio step
    volatile int arpDivision;  // arpeggio subdivision (1, 2, 4, or 8)
    volatile int arpPattern;   // arpeggio pattern (0=up, 1=down, 2=up-down, 3=random)
    volatile bool arpSettingsChanged; // flag for Core 0 to reset arp state
    int arpRandomString;       // cached random string index, updated on arp step

    // Configurable I/O modes
    volatile int cv1Mode;      // CV1Mode enum
    volatile int cv2Mode;      // CV2Mode enum
    volatile int p1Mode;       // P1Mode enum
    volatile int p2Mode;       // P2Mode enum
    volatile int pi1Mode;      // PI1Mode enum
    volatile int pi2Mode;      // PI2Mode enum
    volatile bool outputModesChanged;

    // Tap tempo clock state
    uint32_t clockCounter;     // counts up, wraps at chordPeriod
    int32_t tapClockPulseCounter; // 5ms pulse countdown

    // Random S&H state
    int32_t randomSHValue;     // millivolts, updated on chord change

    // Arp clock state
    int32_t arpClockPulseCounter; // 5ms pulse countdown

    // Pitch S&H state
    int32_t pitchSHValue;      // millivolts, updated on PulseIn1 rising edge

    // Clock divider state
    uint32_t clockDivCount;    // counts PulseIn2 rising edges
    volatile int clockDivRatio; // 2, 3, 4, or 8
    int32_t clockDivPulseCounter; // output pulse countdown

    // Input envelope follower (for CV2 input envelope mode)
    int32_t inputEnvFollower;

    // Onset detector state
    int32_t onsetPeakEnv;        // peak-hold envelope for onset detection (fixed-point <<16)
    int32_t onsetEnvelope;       // slow-tracking baseline envelope (fixed-point <<16)
    int32_t onsetPulseCounter;   // pulse + lockout countdown


    // Precomputed flags: which blocks ProcessSample actually needs to run
    bool needsAudioAbs;
    bool needsAudioTrig;
    bool needsOnset;
    bool needsResEnv;
    bool needsInEnv;
    bool needsPitchTrack;
    bool needsRootMV;
    bool needsArpMV;
    bool needsTapClock;
    bool needsArpClock;
    bool needsClkDiv;
    bool needsChordDetect;

    void updateNeedsFlags() {
        needsArpMV = (cv1Mode == CV1_ARP || cv2Mode == CV2_ARP);
        needsRootMV = needsArpMV || cv1Mode == CV1_ROOT || cv2Mode == CV2_ROOT;
        needsResEnv = (cv1Mode == CV1_ENVELOPE || cv2Mode == CV2_RES_ENV);
        needsInEnv = (cv2Mode == CV2_IN_ENV);
        needsPitchTrack = (cv1Mode == CV1_PITCH_TRACK || cv2Mode == CV2_PITCH_TRACK);
        needsAudioTrig = (p1Mode == P1_AUDIO_TRIG || p2Mode == P2_AUDIO_TRIG);
        needsOnset = (p1Mode == P1_ONSET || p2Mode == P2_ONSET);
        needsTapClock = (p1Mode == P1_TAP_CLOCK || p2Mode == P2_TAP_CLOCK);
        needsArpClock = (p1Mode == P1_ARP_CLOCK || p2Mode == P2_ARP_CLOCK);
        needsClkDiv = (p2Mode == P2_CLK_DIV);
        needsChordDetect = needsArpMV || needsArpClock || needsTapClock ||
                           p2Mode == P2_CHORD_TRIG || cv1Mode == CV1_RANDOM_SH;
        needsAudioAbs = needsAudioTrig || needsOnset || needsInEnv;
    }

    // One-pole lowpass filter for damping
    int32_t dampingFilter(int32_t input, int32_t& state, int32_t coefficient) {
        state += (((input - state) * coefficient + 32768) >> 16);
        return state;
    }

    // Map arpRotation to a 0-3 string index based on arpPattern
    int arpStringIndex() {
        switch (arpPattern) {
            default:
            case 0: // up: 0,1,2,3
                return arpRotation & 3;
            case 1: // down: 3,2,1,0
                return 3 - (arpRotation & 3);
            case 2: { // up-down: 0,1,2,3,2,1 (period 6, extended to 8 for & mask)
                static const int updown[] = {0, 1, 2, 3, 2, 1, 0, 1};
                return updown[arpRotation & 7];
            }
            case 3: // random: cached, updated on arp step
                return arpRandomString;
        }
    }

    // Process one string with linear interpolation for fractional delay
    int32_t processString(int16_t* delayLine, int& writeIndex, int delayLength,
                         int32_t& filterState, int32_t& dcState, int32_t excitation,
                         int32_t dampingCoeff, int32_t frac) {
        // Read two adjacent samples from delay line
        int readIndex1 = (writeIndex - delayLength) & (MAX_DELAY_SIZE - 1);
        int readIndex2 = (readIndex1 - 1) & (MAX_DELAY_SIZE - 1);

        int32_t sample1 = delayLine[readIndex1];
        int32_t sample2 = delayLine[readIndex2];

        // Linear interpolation: blend based on fractional part (frac is 0-255)
        int32_t delayedSample = ((sample1 * (256 - frac)) + (sample2 * frac)) >> 8;

        int32_t dampedSample = dampingFilter(delayedSample, filterState, dampingCoeff);

        // DC blocker: remove DC offset to prevent accumulation
        dcState += (dampedSample - dcState) >> 8;
        dampedSample -= dcState;

        // Add excitation (input signal)
        int32_t newSample = dampedSample + excitation;

        // Soft clipping to prevent overflow
        if (newSample > 2047) newSample = 2047;
        if (newSample < -2047) newSample = -2047;

        // Write back to delay line
        delayLine[writeIndex] = (int16_t)newSample;

        // Advance write index
        writeIndex = (writeIndex + 1) & (MAX_DELAY_SIZE - 1);

        return delayedSample;
    }

    // Calculate frequency ratio based on chord mode and string number
    // Using fixed-point math to avoid floating-point on Cortex-M0+
    // Returns numerator and denominator for each ratio
    void getFrequencyRatios(int& num1, int& den1, int& num2, int& den2,
                            int& num3, int& den3, int& num4, int& den4) {
        // String 1: Fundamental
        num1 = 1;
        den1 = 1;

        switch (currentMode) {
            case HARMONIC:
                // Harmonic series: 1:1, 2:1, 3:1, 4:1
                num2 = 2; den2 = 1;
                num3 = 3; den3 = 1;
                num4 = 4; den4 = 1;
                break;
            case FIFTH:
                // Stacked fifths: 1:1, 3:2, 2:1, 3:1
                num2 = 3; den2 = 2;
                num3 = 2; den3 = 1;
                num4 = 3; den4 = 1;
                break;
            case MAJOR7:
                // Major 7th: 1:1, 5:4, 3:2, 15:8
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 15; den4 = 8;
                break;
            case MINOR7:
                // Minor 7th: 1:1, 6:5, 3:2, 9:5
                num2 = 6; den2 = 5;
                num3 = 3; den3 = 2;
                num4 = 9; den4 = 5;
                break;
            case DIM:
                // Diminished: 1:1, 6:5, 36:25, 3:2
                num2 = 6; den2 = 5;
                num3 = 36; den3 = 25;
                num4 = 3; den4 = 2;
                break;
            case SUS4:
                // Suspended 4th: 1:1, 4:3, 3:2, 2:1
                num2 = 4; den2 = 3;
                num3 = 3; den3 = 2;
                num4 = 2; den4 = 1;
                break;
            case ADD9:
                // Major add 9: 1:1, 5:4, 3:2, 9:4
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 9; den4 = 4;
                break;
            case MAJOR10:
                // Major 10th: 1:1, 5:4, 3:2, 5:2 (root, M3, P5, M10)
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 5; den4 = 2;
                break;
            case SUS2:
                // Suspended 2nd: 1:1, 9:8, 3:2, 2:1 (root, M2, P5, octave)
                num2 = 9; den2 = 8;
                num3 = 3; den3 = 2;
                num4 = 2; den4 = 1;
                break;
            case MAJOR:
                // Major triad: 1:1, 5:4, 3:2, 2:1 (root, M3, P5, octave)
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 2; den4 = 1;
                break;
            case MINOR:
                // Minor triad: 1:1, 6:5, 3:2, 2:1 (root, m3, P5, octave)
                num2 = 6; den2 = 5;
                num3 = 3; den3 = 2;
                num4 = 2; den4 = 1;
                break;
            case MAJOR6:
                // Major 6th: 1:1, 5:4, 3:2, 5:3 (root, M3, P5, M6)
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 5; den4 = 3;
                break;
            case DOM7:
                // Dominant 7th: 1:1, 5:4, 3:2, 9:5 (root, M3, P5, m7)
                num2 = 5; den2 = 4;
                num3 = 3; den3 = 2;
                num4 = 9; den4 = 5;
                break;
            case MIN9:
                // Minor add 9: 1:1, 6:5, 3:2, 9:4 (root, m3, P5, M9)
                num2 = 6; den2 = 5;
                num3 = 3; den3 = 2;
                num4 = 9; den4 = 4;
                break;
            case TANPURA_PA:
                // Tanpura Pa: 1:1, 3:2, 2:1, 4:1 (Sa, Pa, Sa', Sa'')
                num2 = 3; den2 = 2;
                num3 = 2; den3 = 1;
                num4 = 4; den4 = 1;
                break;
            case TANPURA_MA:
                // Tanpura Ma: 1:1, 4:3, 2:1, 4:1 (Sa, Ma, Sa', Sa'')
                num2 = 4; den2 = 3;
                num3 = 2; den3 = 1;
                num4 = 4; den4 = 1;
                break;
            case TANPURA_NI:
                // Tanpura Ni: 1:1, 15:8, 2:1, 4:1 (Sa, Ni, Sa', Sa'')
                num2 = 15; den2 = 8;
                num3 = 2; den3 = 1;
                num4 = 4; den4 = 1;
                break;
            case TANPURA_NI_KOMAL:
                // Tanpura ni: 1:1, 9:5, 2:1, 4:1 (Sa, ni, Sa', Sa'')
                num2 = 9; den2 = 5;
                num3 = 2; den3 = 1;
                num4 = 4; den4 = 1;
                break;
        }
    }

public:
    ResonatingStrings() : writeIndex1(0), writeIndex2(0), writeIndex3(0), writeIndex4(0),
                          delayLength1(100), delayLength2(150), delayLength3(200), delayLength4(400),
                          filterState1(0), filterState2(0), filterState3(0), filterState4(0),
                          currentMode(HARMONIC), activeBuffer(0), progressionChanged(false), pendingFlashSave(false),
                          progressionIndex(0), lastSwitchDown(true),
                          pulseExciteEnvelope(0), noiseState(12345),
                          dcState1(0), dcState2(0), dcState3(0), dcState4(0),
                          smoothDelay1(0), smoothDelay2(0), smoothDelay3(0), smoothDelay4(0),
                          switchDownCounter(0), resetTriggered(false),
                          arpRotation(0), envFollower(0), triggerArmed(true),
                          trigPulseCounter(0), prevProgressionIndex(0), chordPulseCounter(0),
                          chordPeriod(0), chordTimer(0), arpStepCounter(0), arpDivision(4), arpPattern(0), arpSettingsChanged(false), arpRandomString(0),
                          cv1Mode(CV1_ARP), cv2Mode(CV2_RES_ENV), p1Mode(P1_AUDIO_TRIG), p2Mode(P2_CHORD_TRIG),
                          pi1Mode(PI1_PLUCK), pi2Mode(PI2_ADVANCE), outputModesChanged(false),
                          clockCounter(0), tapClockPulseCounter(0),
                          randomSHValue(0), arpClockPulseCounter(0),
                          pitchSHValue(0), clockDivCount(0), clockDivRatio(2), clockDivPulseCounter(0),
                          inputEnvFollower(0),
                          onsetPeakEnv(0), onsetEnvelope(0), onsetPulseCounter(0) {
        // Try to load progression from flash, fall back to defaults
        if (!loadProgressionFromFlash()) {
            // Default progression: all chords (card works standalone without browser UI)
            const ChordMode allChords[] = {
                HARMONIC, FIFTH, MAJOR7, MINOR7, DIM, SUS4, ADD9, MAJOR10,
                SUS2, MAJOR, MINOR, MAJOR6, DOM7, MIN9,
                TANPURA_PA, TANPURA_MA, TANPURA_NI, TANPURA_NI_KOMAL
            };
            for (int i = 0; i < NUM_MODES; i++) {
                progressionBuffers[0].chords[i] = allChords[i];
                progressionBuffers[1].chords[i] = allChords[i];
            }
            progressionBuffers[0].length = NUM_MODES;
            progressionBuffers[1].length = NUM_MODES;
        }

        // Initialize delay lines with silence
        for (int i = 0; i < MAX_DELAY_SIZE; i++) {
            delayLine1[i] = 0;
            delayLine2[i] = 0;
            delayLine3[i] = 0;
            delayLine4[i] = 0;
        }

        updateNeedsFlags();
    }

    // Check and perform deferred flash save (called from Core 1)
    void checkPendingFlashSave() {
        if (pendingFlashSave) {
            pendingFlashSave = false;
            saveProgressionToFlash();
        }
    }

    // Serial command handler (called from Core 1)
    void handleSerialCommand(const char* cmd) {
        if (strncmp(cmd, "SET ", 4) == 0) {
            handleSet(cmd + 4);
        } else if (strcmp(cmd, "GET") == 0) {
            handleGet();
        } else if (strncmp(cmd, "SETARP ", 7) == 0) {
            handleSetArp(cmd + 7);
        } else if (strcmp(cmd, "GETARP") == 0) {
            handleGetArp();
        } else if (strncmp(cmd, "SETPAT ", 7) == 0) {
            handleSetPat(cmd + 7);
        } else if (strcmp(cmd, "GETPAT") == 0) {
            handleGetPat();
        } else if (strncmp(cmd, "SETOUT ", 7) == 0) {
            handleSetOut(cmd + 7);
        } else if (strcmp(cmd, "GETOUT") == 0) {
            handleGetOut();
        } else if (strncmp(cmd, "SETDIV ", 7) == 0) {
            handleSetDiv(cmd + 7);
        } else if (strcmp(cmd, "GETDIV") == 0) {
            handleGetDiv();
        } else {
            printf("ERR unknown_command\n");
        }
    }

private:
    void handleSet(const char* args) {
        ChordMode newChords[MAX_PROGRESSION_LENGTH];
        int count = 0;
        const char* p = args;

        while (*p && count < MAX_PROGRESSION_LENGTH) {
            int val = 0;
            bool hasDigit = false;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
                hasDigit = true;
            }
            if (!hasDigit) break;
            if (val < 0 || val >= NUM_MODES) {
                printf("ERR invalid_id\n");
                return;
            }
            newChords[count++] = (ChordMode)val;
            if (*p == ',') p++;
        }

        if (count == 0) {
            printf("ERR empty_progression\n");
            return;
        }

        // Write to inactive buffer, then swap
        int writeIdx = 1 - activeBuffer;
        for (int i = 0; i < count; i++) {
            progressionBuffers[writeIdx].chords[i] = newChords[i];
        }
        progressionBuffers[writeIdx].length = count;
        __dmb();  // ARM data memory barrier
        activeBuffer = writeIdx;
        progressionChanged = true;

        handleGet();

        // Persist to flash
        saveProgressionToFlash();
    }

    void handleGet() {
        int bufIdx = activeBuffer;
        printf("PROG ");
        for (int i = 0; i < progressionBuffers[bufIdx].length; i++) {
            if (i > 0) printf(",");
            printf("%d", (int)progressionBuffers[bufIdx].chords[i]);
        }
        printf("\n");
    }

    void handleGetArp() {
        printf("ARP %d\n", arpDivision);
    }

    void handleSetArp(const char* args) {
        int val = 0;
        const char* p = args;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val != 1 && val != 2 && val != 4 && val != 8) {
            printf("ERR invalid_arp_division\n");
            return;
        }
        arpDivision = val;
        arpSettingsChanged = true;
        printf("ARP %d\n", arpDivision);
        saveProgressionToFlash();
    }

    void handleGetPat() {
        printf("PAT %d\n", arpPattern);
    }

    void handleSetPat(const char* args) {
        int val = 0;
        const char* p = args;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val < 0 || val > 3) {
            printf("ERR invalid_arp_pattern\n");
            return;
        }
        arpPattern = val;
        arpSettingsChanged = true;
        printf("PAT %d\n", arpPattern);
        saveProgressionToFlash();
    }

    void handleGetOut() {
        printf("OUT %d,%d,%d,%d,%d,%d\n", (int)cv1Mode, (int)cv2Mode, (int)p1Mode, (int)p2Mode, (int)pi1Mode, (int)pi2Mode);
    }

    void handleSetOut(const char* args) {
        int vals[6];
        int count = 0;
        const char* p = args;
        while (*p && count < 6) {
            int val = 0;
            bool hasDigit = false;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
                hasDigit = true;
            }
            if (!hasDigit) break;
            vals[count++] = val;
            if (*p == ',') p++;
        }
        if (count != 6) {
            printf("ERR invalid_out_args\n");
            return;
        }
        // Validate ranges
        if (vals[0] < 0 || vals[0] > 5 ||
            vals[1] < 0 || vals[1] > 4 ||
            vals[2] < 0 || vals[2] > 3 ||
            vals[3] < 0 || vals[3] > 5 ||
            vals[4] < 0 || vals[4] > 2 ||
            vals[5] < 0 || vals[5] > 3) {
            printf("ERR invalid_out_mode\n");
            return;
        }
        cv1Mode = vals[0];
        cv2Mode = vals[1];
        p1Mode = vals[2];
        p2Mode = vals[3];
        pi1Mode = vals[4];
        pi2Mode = vals[5];
        outputModesChanged = true;
        handleGetOut();
        saveProgressionToFlash();
    }

    void handleGetDiv() {
        printf("DIV %d\n", (int)clockDivRatio);
    }

    void handleSetDiv(const char* args) {
        int val = 0;
        const char* p = args;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val != 2 && val != 3 && val != 4 && val != 8) {
            printf("ERR invalid_div_ratio\n");
            return;
        }
        clockDivRatio = val;
        printf("DIV %d\n", (int)clockDivRatio);
        saveProgressionToFlash();
    }

    // Save current progression to flash (must be called from Core 1)
    void saveProgressionToFlash() {
        int bufIdx = activeBuffer;
        int len = progressionBuffers[bufIdx].length;

        // Prepare data buffer (must be 256-byte aligned for flash write)
        uint8_t data[FLASH_PAGE_SIZE] = {0};
        data[0] = FLASH_PROG_MAGIC;
        data[1] = (uint8_t)len;
        for (int i = 0; i < len && i < MAX_PROGRESSION_LENGTH; i++) {
            data[2 + i] = (uint8_t)progressionBuffers[bufIdx].chords[i];
        }
        data[20] = (uint8_t)arpDivision;
        data[21] = (uint8_t)arpPattern;
        data[22] = (uint8_t)cv1Mode;
        data[23] = (uint8_t)cv2Mode;
        data[24] = (uint8_t)p1Mode;
        data[25] = (uint8_t)p2Mode;
        data[26] = (uint8_t)pi1Mode;
        data[27] = (uint8_t)pi2Mode;
        data[28] = (uint8_t)clockDivRatio;

        // Pause Core 0 during flash operation (XIP is blocked during erase/program)
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(FLASH_PROG_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_PROG_OFFSET, data, FLASH_PAGE_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
    }

private:
    // Load progression from flash, returns true if valid data found
    bool loadProgressionFromFlash() {
        // Read from flash (XIP address space)
        const uint8_t* flash_data = (const uint8_t*)(XIP_BASE + FLASH_PROG_OFFSET);

        // Check magic byte
        if (flash_data[0] != FLASH_PROG_MAGIC) {
            return false;
        }

        int len = flash_data[1];
        if (len < 1 || len > MAX_PROGRESSION_LENGTH) {
            return false;
        }

        // Load into both buffers
        for (int i = 0; i < len; i++) {
            uint8_t modeVal = flash_data[2 + i];
            if (modeVal >= NUM_MODES) {
                return false;  // Invalid chord ID
            }
            ChordMode mode = (ChordMode)modeVal;
            progressionBuffers[0].chords[i] = mode;
            progressionBuffers[1].chords[i] = mode;
        }
        progressionBuffers[0].length = len;
        progressionBuffers[1].length = len;

        // Load arp division (byte 20), default to 4 if invalid (old flash has 0x00)
        uint8_t arpVal = flash_data[20];
        if (arpVal == 1 || arpVal == 2 || arpVal == 4 || arpVal == 8) {
            arpDivision = arpVal;
        } else {
            arpDivision = 4;
        }

        // Load arp pattern (byte 21), default to 0 if invalid (old flash has 0x00)
        uint8_t patVal = flash_data[21];
        if (patVal <= 3) {
            arpPattern = patVal;
        } else {
            arpPattern = 0;
        }

        // Load output modes (bytes 22-27), default to 0 if invalid (old flash)
        uint8_t cv1Val = flash_data[22];
        cv1Mode = (cv1Val <= 5) ? cv1Val : 0;
        uint8_t cv2Val = flash_data[23];
        cv2Mode = (cv2Val <= 4) ? cv2Val : 0;
        uint8_t p1Val = flash_data[24];
        p1Mode = (p1Val <= 3) ? p1Val : 0;
        uint8_t p2Val = flash_data[25];
        p2Mode = (p2Val <= 5) ? p2Val : 0;
        uint8_t pi1Val = flash_data[26];
        pi1Mode = (pi1Val <= 2) ? pi1Val : 0;
        uint8_t pi2Val = flash_data[27];
        pi2Mode = (pi2Val <= 3) ? pi2Val : 0;

        // Load clock divider ratio (byte 28), default to 2 if invalid
        uint8_t divVal = flash_data[28];
        if (divVal == 2 || divVal == 3 || divVal == 4 || divVal == 8) {
            clockDivRatio = divVal;
        } else {
            clockDivRatio = 2;
        }

        return true;
    }

    // Reset progression to factory defaults and save to flash
    void resetToDefaults() {
        const ChordMode allChords[] = {
            HARMONIC, FIFTH, MAJOR7, MINOR7, DIM, SUS4, ADD9, MAJOR10,
            SUS2, MAJOR, MINOR, MAJOR6, DOM7, MIN9,
            TANPURA_PA, TANPURA_MA, TANPURA_NI, TANPURA_NI_KOMAL
        };

        // Write to inactive buffer, then swap
        int writeIdx = 1 - activeBuffer;
        for (int i = 0; i < NUM_MODES; i++) {
            progressionBuffers[writeIdx].chords[i] = allChords[i];
        }
        progressionBuffers[writeIdx].length = NUM_MODES;
        __dmb();
        activeBuffer = writeIdx;

        // Reset to first chord
        progressionIndex = 0;
        currentMode = progressionBuffers[activeBuffer].chords[0];
        arpDivision = 4;
        arpPattern = 0;
        cv1Mode = CV1_ARP;
        cv2Mode = CV2_RES_ENV;
        p1Mode = P1_AUDIO_TRIG;
        p2Mode = P2_CHORD_TRIG;
        pi1Mode = PI1_PLUCK;
        pi2Mode = PI2_ADVANCE;
        clockDivRatio = 2;
        outputModesChanged = true;

        // Defer flash save to Core 1 (Core 0 can't lock out Core 1)
        pendingFlashSave = true;
    }

protected:
    void ProcessSample() override {
        int16_t audioIn1 = AudioIn1();
        int16_t audioIn2 = AudioIn2();
        int32_t audioIn = ((int32_t)audioIn1 + (int32_t)audioIn2 + 1) >> 1;

        // Check for serial progression update from Core 1
        if (progressionChanged) {
            __dmb();  // Ensure we see updated buffer contents after flag
            progressionIndex = 0;
            currentMode = progressionBuffers[activeBuffer].chords[0];
            progressionChanged = false;
        }

        // Recompute which output blocks to run when I/O modes change
        if (outputModesChanged) {
            __dmb();
            outputModesChanged = false;
            updateNeedsFlags();
        }

        // Mode switching (switch down or pulse in 2)
        // Long press (3 sec) resets to factory defaults
        Switch switchPos = SwitchVal();
        bool switchDown = (switchPos == Down);

        if (switchDown) {
            switchDownCounter++;

            // Long press detected - reset to defaults
            if (switchDownCounter >= RESET_HOLD_SAMPLES && !resetTriggered) {
                resetToDefaults();
                resetTriggered = true;
            }
        } else {
            // Switch released - advance chord only if it was a short press
            if (lastSwitchDown && !resetTriggered && switchDownCounter > 0) {
                int bufIdx = activeBuffer;
                progressionIndex = (progressionIndex + 1) % progressionBuffers[bufIdx].length;
                currentMode = progressionBuffers[bufIdx].chords[progressionIndex];
            }
            switchDownCounter = 0;
            resetTriggered = false;
        }

        // Pulse input handling (mode-based)
        bool pulseIn1Rising = PulseIn1RisingEdge();
        bool pulseIn2Rising = PulseIn2RisingEdge();

        // Pulse In 1 dispatch
        if (pulseIn1Rising) {
            switch (pi1Mode) {
                case PI1_PLUCK:
                    pulseExciteEnvelope = 2048;
                    break;
                case PI1_ADVANCE: {
                    int bufIdx = activeBuffer;
                    progressionIndex = (progressionIndex + 1) % progressionBuffers[bufIdx].length;
                    currentMode = progressionBuffers[bufIdx].chords[progressionIndex];
                    break;
                }
                case PI1_RESET:
                    progressionIndex = 0;
                    currentMode = progressionBuffers[activeBuffer].chords[0];
                    break;
            }
        }

        // Pulse In 2 dispatch
        if (pulseIn2Rising) {
            switch (pi2Mode) {
                case PI2_ADVANCE: {
                    int bufIdx = activeBuffer;
                    progressionIndex = (progressionIndex + 1) % progressionBuffers[bufIdx].length;
                    currentMode = progressionBuffers[bufIdx].chords[progressionIndex];
                    break;
                }
                case PI2_ARP_STEP:
                    arpRotation++;
                    noiseState = noiseState * 1103515245 + 12345;
                    arpRandomString = (noiseState >> 16) & 3;
                    break;
                case PI2_PLUCK:
                    pulseExciteEnvelope = 2048;
                    break;
                case PI2_RESET:
                    progressionIndex = 0;
                    currentMode = progressionBuffers[activeBuffer].chords[0];
                    break;
            }
        }

        lastSwitchDown = switchDown;

        // FREQUENCY CONTROL - 1V/oct
        // CV1: ±6V maps to -2048 to 2047
        int32_t pitchCV;

        if (Disconnected(Input::CV1)) {
            // No CV connected: X knob controls C1-C7 range
            // Map knob 0-4095 to pitchCV 2048-4095 (6 octaves)
            pitchCV = 2048 + (KnobVal(X) / 2);
        } else {
            // CV connected: X knob is fine tune (±1 octave)
            // 1 octave = 341 steps
            int32_t fineTune = ((KnobVal(X) - 2048) * 341) / 2048;

            // CV input with 1V/oct scaling
            // CVIn1 range: -2048 to +2047 for ±6V, so 1V = 341 counts
            int32_t scaledCV = CVIn1();
            
            pitchCV = 2048 + scaledCV + fineTune;
        }

        if (pitchCV > 4095) pitchCV = 4095;
        if (pitchCV < 0) pitchCV = 0;

        // Get delay from exponential lookup table (1V/oct)
        int32_t baseDelay = ExpDelay(pitchCV);

        // Clamp to usable range
        const int MIN_DELAY = 15;
        const int MAX_DELAY = 1468;  // C1 at 32.7Hz
        if (baseDelay < MIN_DELAY) baseDelay = MIN_DELAY;
        if (baseDelay > MAX_DELAY) baseDelay = MAX_DELAY;

        // Get frequency ratios based on current chord mode
        int num1 = 1, den1 = 1, num2 = 2, den2 = 1, num3 = 3, den3 = 1, num4 = 4, den4 = 1;
        getFrequencyRatios(num1, den1, num2, den2, num3, den3, num4, den4);

        // Calculate target delay lengths for each string using fixed-point math
        // delay = baseDelay * denominator / numerator
        // Use 8 extra bits of precision to extract fractional part for interpolation
        int32_t targetDelay1 = ((baseDelay * den1) << 8) / num1;
        int32_t targetDelay2 = ((baseDelay * den2) << 8) / num2;
        int32_t targetDelay3 = ((baseDelay * den3) << 8) / num3;
        int32_t targetDelay4 = ((baseDelay * den4) << 8) / num4;

        // Smooth delay transitions to avoid harsh plucks on chord changes
        // Initialize smoothDelay on first sample (when it's 0)
        if (smoothDelay1 == 0) smoothDelay1 = targetDelay1;
        if (smoothDelay2 == 0) smoothDelay2 = targetDelay2;
        if (smoothDelay3 == 0) smoothDelay3 = targetDelay3;
        if (smoothDelay4 == 0) smoothDelay4 = targetDelay4;

        // One-pole lowpass: smoothDelay approaches target
        const int32_t GLIDE_COEFF = 2;  // Lower = slower glide (~2 seconds)
        smoothDelay1 += ((targetDelay1 - smoothDelay1) * GLIDE_COEFF) >> 8;
        smoothDelay2 += ((targetDelay2 - smoothDelay2) * GLIDE_COEFF) >> 8;
        smoothDelay3 += ((targetDelay3 - smoothDelay3) * GLIDE_COEFF) >> 8;
        smoothDelay4 += ((targetDelay4 - smoothDelay4) * GLIDE_COEFF) >> 8;

        delayLength1 = smoothDelay1 >> 8;  // Integer part
        delayLength2 = smoothDelay2 >> 8;
        delayLength3 = smoothDelay3 >> 8;
        delayLength4 = smoothDelay4 >> 8;

        int32_t frac1 = smoothDelay1 & 0xFF;  // Fractional part (0-255)
        int32_t frac2 = smoothDelay2 & 0xFF;
        int32_t frac3 = smoothDelay3 & 0xFF;
        int32_t frac4 = smoothDelay4 & 0xFF;

        // Clamp to valid range
        if (delayLength1 < MIN_DELAY) delayLength1 = MIN_DELAY;
        if (delayLength2 < MIN_DELAY) delayLength2 = MIN_DELAY;
        if (delayLength3 < MIN_DELAY) delayLength3 = MIN_DELAY;
        if (delayLength4 < MIN_DELAY) delayLength4 = MIN_DELAY;
        if (delayLength1 > MAX_DELAY_SIZE - 1) delayLength1 = MAX_DELAY_SIZE - 1;
        if (delayLength2 > MAX_DELAY_SIZE - 1) delayLength2 = MAX_DELAY_SIZE - 1;
        if (delayLength3 > MAX_DELAY_SIZE - 1) delayLength3 = MAX_DELAY_SIZE - 1;
        if (delayLength4 > MAX_DELAY_SIZE - 1) delayLength4 = MAX_DELAY_SIZE - 1;

        // DAMPING CONTROL (Y Knob + CV2)
        int32_t dampingKnob = KnobVal(Y) + CVIn2();  // 0-4095 knob + CV
        if (dampingKnob > 4095) dampingKnob = 4095;
        if (dampingKnob < 0) dampingKnob = 0;

        // Map to filter coefficient (more damping = lower coefficient, longer decay = higher coefficient)
        int32_t dampingCoeff = 32000 + ((dampingKnob * 33300) >> 12);

        // Excitation amounts for each string
        // String 1 gets full input, others get scaled versions (sympathetic response)
        int32_t excitation1 = audioIn >> 2;  // Direct excitation
        int32_t excitation2 = audioIn >> 4;  // Sympathetic response
        int32_t excitation3 = audioIn >> 4;  // Sympathetic response
        int32_t excitation4 = audioIn >> 3;  // 4th string

        // Apply decaying noise burst while envelope is active
        if (pulseExciteEnvelope > 10) {
            noiseState = noiseState * 1103515245 + 12345;
            int32_t noise = (int32_t)((noiseState >> 16) & 0xFFF) - 2048;
            int32_t scaledNoise = (noise * pulseExciteEnvelope) >> 11;
            excitation1 += scaledNoise;
            excitation2 += scaledNoise >> 1;
            excitation3 += scaledNoise >> 1;
            excitation4 += scaledNoise >> 1;
            // Fast decay for short pluck burst
            pulseExciteEnvelope = (pulseExciteEnvelope * 250) >> 8;
        }

        // Process each string with fractional delay interpolation
        int32_t out1 = processString(delayLine1, writeIndex1, delayLength1,
                                     filterState1, dcState1, excitation1, dampingCoeff, frac1);
        int32_t out2 = processString(delayLine2, writeIndex2, delayLength2,
                                     filterState2, dcState2, excitation2, dampingCoeff, frac2);
        int32_t out3 = processString(delayLine3, writeIndex3, delayLength3,
                                     filterState3, dcState3, excitation3, dampingCoeff, frac3);
        int32_t out4 = processString(delayLine4, writeIndex4, delayLength4,
                                     filterState4, dcState4, excitation4, dampingCoeff, frac4);

        // Mix strings together - stereo mid/side
        // Out1 (mid): all strings summed - mono compatible
        // Out2 (side): strings 1&3 center, strings 2&4 wide/diffuse
        int32_t resonatorOut1, resonatorOut2;
        if (SwitchVal() == Switch::Up) {
            // TUNING MODE: first string only
            resonatorOut1 = out1 / 4;
            resonatorOut2 = out1 / 4;
        } else {
            resonatorOut1 = (out1 + out2 + out3 + out4) / 4;
            resonatorOut2 = (out1 - out2 + out3 - out4) / 4;
        }

        resonatorOut1 *= 2;
        resonatorOut2 *= 2;

        // WET/DRY MIX (Main Knob)
        int32_t mixKnob = KnobVal(Main);  // 0-4095

        int32_t dryGain = 4095 - mixKnob;
        int32_t wetGain = mixKnob;

        int32_t mixedOutput1 = ((audioIn * dryGain) + (resonatorOut1 * wetGain) + 2048) >> 12;
        int32_t mixedOutput2 = ((audioIn * dryGain) + (resonatorOut2 * wetGain) + 2048) >> 12;

        // Clipping
        if (mixedOutput1 > 2047) mixedOutput1 = 2047;
        if (mixedOutput1 < -2047) mixedOutput1 = -2047;
        if (mixedOutput2 > 2047) mixedOutput2 = 2047;
        if (mixedOutput2 < -2047) mixedOutput2 = -2047;

        // Stereo output
        AudioOut1((int16_t)mixedOutput1);
        AudioOut2((int16_t)mixedOutput2);

        // --- CV and Pulse outputs (only compute what the current I/O config needs) ---

        bool chordChanged = false;
        bool arpStepped = false;

        // Chord detection, arp stepping, and related state
        if (needsChordDetect) {
            chordTimer++;
            if (arpSettingsChanged) {
                arpSettingsChanged = false;
                arpRotation = 0;
                if (chordPeriod > 0) {
                    arpStepCounter = chordPeriod / arpDivision;
                }
            }

            chordChanged = (progressionIndex != prevProgressionIndex);

            if (chordChanged) {
                chordPulseCounter = 240;  // 5ms at 48kHz
                chordPeriod = chordTimer;
                chordTimer = 0;
                prevProgressionIndex = progressionIndex;
                arpRotation = 0;
                noiseState = noiseState * 1103515245 + 12345;
                arpRandomString = (noiseState >> 16) & 3;
                arpStepCounter = chordPeriod / arpDivision;
                noiseState = noiseState * 1103515245 + 12345;
                randomSHValue = ((int32_t)((noiseState >> 16) & 0xFFF) - 2048) * 12014 >> 12;
                clockCounter = 0;
            }

            // Subdivide chord period into arpDivision steps
            if (chordPeriod > 0 && arpStepCounter > 0) {
                arpStepCounter--;
                if (arpStepCounter == 0 && arpRotation < (arpDivision - 1)) {
                    arpRotation++;
                    arpStepped = true;
                    noiseState = noiseState * 1103515245 + 12345;
                    arpRandomString = (noiseState >> 16) & 3;
                    arpStepCounter = chordPeriod / arpDivision;
                }
            }
        }

        // Pitch S&H: capture CV In 1 on Pulse In 1 rising edge
        if (pulseIn1Rising && cv1Mode == CV1_PITCH_SH) {
            pitchSHValue = (pitchCV - 3069) * 12014 >> 12;
        }

        // Audio amplitude (shared by schmitt trigger, onset detector, input envelope)
        int32_t audioAbs = 0;
        if (needsAudioAbs) {
            int32_t abs1 = audioIn1 < 0 ? -audioIn1 : audioIn1;
            int32_t abs2 = audioIn2 < 0 ? -audioIn2 : audioIn2;
            audioAbs = abs1 > abs2 ? abs1 : abs2;
        }

        // Schmitt trigger
        bool audioTrigOut = false;
        if (needsAudioTrig) {
            if (trigPulseCounter > 0) {
                trigPulseCounter--;
            } else {
                if (triggerArmed && audioAbs > 200) {
                    triggerArmed = false;
                    trigPulseCounter = 2400;
                }
                if (!triggerArmed && audioAbs < 80) {
                    triggerArmed = true;
                }
            }
            audioTrigOut = (trigPulseCounter > 2400 - 240);
        }

        // Onset detector
        bool onsetTrigOut = false;
        if (needsOnset) {
            // Stage 1: Peak-hold envelope — instant attack, slow release
            // Smooths out waveform zero-crossing dips without adding onset latency
            int32_t target = audioAbs << 16;
            if (target > onsetPeakEnv) {
                onsetPeakEnv = target;                           // instant attack
            } else {
                onsetPeakEnv -= (onsetPeakEnv - target) >> 13;  // release τ ≈ 170ms
            }

            // Stage 2: Baseline tracker — adapts to sustained level
            if (onsetPeakEnv > onsetEnvelope) {
                onsetEnvelope += (onsetPeakEnv - onsetEnvelope) >> 10;  // attack τ ≈ 21ms
            } else {
                onsetEnvelope -= (onsetEnvelope - onsetPeakEnv) >> 12;  // release τ ≈ 85ms
            }

            // Hybrid threshold: 12.5% of baseline or absolute minimum
            int32_t threshold = onsetEnvelope >> 3;
            if (threshold < (80 << 16)) threshold = (80 << 16);

            if (onsetPulseCounter > 0) {
                onsetPulseCounter--;
            } else if (onsetPeakEnv > onsetEnvelope + threshold) {
                onsetPulseCounter = 2400;         // 50ms lockout
                onsetEnvelope = onsetPeakEnv;     // snap baseline to current peak
            }
            onsetTrigOut = (onsetPulseCounter > 2400 - 240);  // 5ms pulse
        }

        // Chord change pulse
        if (chordPulseCounter > 0) chordPulseCounter--;
        bool chordTrigOut = (chordPulseCounter > 0);

        // Resonator envelope follower
        if (needsResEnv) {
            int32_t resAbs = resonatorOut1 < 0 ? -resonatorOut1 : resonatorOut1;
            int32_t target = resAbs << 16;
            if (target > envFollower) {
                envFollower += (target - envFollower) >> 5;
            } else {
                envFollower -= (envFollower - target) >> 12;
            }
        }

        // Input envelope follower
        if (needsInEnv) {
            int32_t target = audioAbs << 16;
            if (target > inputEnvFollower) {
                inputEnvFollower += (target - inputEnvFollower) >> 5;
            } else {
                inputEnvFollower -= (inputEnvFollower - target) >> 12;
            }
        }

        // Feed audio to Core 1 for YIN pitch detection
        if (needsPitchTrack) {
            yinRing[yinRingHead & (YIN_RING_SIZE - 1)] = audioIn1;
            yinRingHead++;
        }

        // Arp / root pitch (millivolts)
        int32_t rootMV = 0, arpMV = 0;
        if (needsRootMV) {
            rootMV = (pitchCV - 3069) * 12014 >> 12;
        }
        if (needsArpMV) {
            int ratioMV;
            switch (arpStringIndex()) {
                case 0: ratioMV = ratioToMillivolts(num1, den1); break;
                case 1: ratioMV = ratioToMillivolts(num2, den2); break;
                case 2: ratioMV = ratioToMillivolts(num3, den3); break;
                case 3: ratioMV = ratioToMillivolts(num4, den4); break;
                default: ratioMV = 0; break;
            }
            arpMV = rootMV + ratioMV;
        }

        // Tap tempo clock
        bool tapClockOut = false;
        if (needsTapClock) {
            if (chordPeriod > 0) {
                clockCounter++;
                if (clockCounter >= chordPeriod) {
                    clockCounter = 0;
                    tapClockPulseCounter = 240;
                }
            }
            if (tapClockPulseCounter > 0) tapClockPulseCounter--;
            tapClockOut = (tapClockPulseCounter > 0);
        }

        // Arp clock
        bool arpClockOut = false;
        if (needsArpClock) {
            if (arpStepped || chordChanged) {
                arpClockPulseCounter = 240;
            }
            if (arpClockPulseCounter > 0) arpClockPulseCounter--;
            arpClockOut = (arpClockPulseCounter > 0);
        }

        // Clock divider
        bool clockDivOut = false;
        if (needsClkDiv) {
            if (pulseIn2Rising) {
                clockDivCount++;
                if ((int)clockDivCount >= clockDivRatio) {
                    clockDivCount = 0;
                    clockDivPulseCounter = 240;
                }
            }
            if (clockDivPulseCounter > 0) clockDivPulseCounter--;
            clockDivOut = (clockDivPulseCounter > 0);
        }

        // --- Dispatch outputs ---

        // CV Out 1
        switch (cv1Mode) {
            default:
            case CV1_ARP:       CVOut1Millivolts(arpMV); break;
            case CV1_ROOT:      CVOut1Millivolts(rootMV); break;
            case CV1_ENVELOPE:  CVOut1((int16_t)(envFollower >> 16)); break;
            case CV1_RANDOM_SH: CVOut1Millivolts(randomSHValue); break;
            case CV1_PITCH_SH:  CVOut1Millivolts(pitchSHValue); break;
            case CV1_PITCH_TRACK: CVOut1Millivolts(yinSharedPitchMV); break;
        }

        // CV Out 2
        switch (cv2Mode) {
            default:
            case CV2_RES_ENV:   CVOut2((int16_t)(envFollower >> 16)); break;
            case CV2_IN_ENV:    CVOut2((int16_t)(inputEnvFollower >> 16)); break;
            case CV2_ARP:       CVOut2Millivolts(arpMV); break;
            case CV2_ROOT:      CVOut2Millivolts(rootMV); break;
            case CV2_PITCH_TRACK: CVOut2Millivolts(yinSharedPitchMV); break;
        }

        // Pulse Out 1
        switch (p1Mode) {
            default:
            case P1_AUDIO_TRIG: PulseOut1(audioTrigOut); break;
            case P1_TAP_CLOCK:  PulseOut1(tapClockOut); break;
            case P1_ARP_CLOCK:  PulseOut1(arpClockOut); break;
            case P1_ONSET:      PulseOut1(onsetTrigOut); break;
        }

        // Pulse Out 2
        switch (p2Mode) {
            default:
            case P2_CHORD_TRIG: PulseOut2(chordTrigOut); break;
            case P2_TAP_CLOCK:  PulseOut2(tapClockOut); break;
            case P2_AUDIO_TRIG: PulseOut2(audioTrigOut); break;
            case P2_ARP_CLOCK:  PulseOut2(arpClockOut); break;
            case P2_CLK_DIV:    PulseOut2(clockDivOut); break;
            case P2_ONSET:      PulseOut2(onsetTrigOut); break;
        }

        // LED indicators - show position in progression (0-17)
        // Single LED: positions 0-5
        // Pairs for positions 6-17:
        // 6: 0+5, 7: 1+3, 8: 0+2, 9: 1+2
        // 10: 3+4, 11: 2+4, 12: 0+4, 13: 3+5
        // 14: 1+4, 15: 2+3, 16: 0+3, 17: 2+5
        // LED indicators
        // During long press: progressive fill (0->5) showing reset countdown
        // After reset: brief flash of all LEDs
        // Normal: show position in progression
        if (switchDown && switchDownCounter > 0 && !resetTriggered) {
            // Progressive LED fill during hold (6 LEDs over 3 seconds)
            int ledsLit = (switchDownCounter * 6) / RESET_HOLD_SAMPLES;
            LedOn(0, ledsLit >= 1);
            LedOn(1, ledsLit >= 2);
            LedOn(2, ledsLit >= 3);
            LedOn(3, ledsLit >= 4);
            LedOn(4, ledsLit >= 5);
            LedOn(5, ledsLit >= 6);
        } else if (resetTriggered && switchDownCounter < RESET_HOLD_SAMPLES + 24000) {
            // Flash all LEDs for 0.5 sec after reset
            bool flash = ((switchDownCounter / 4800) % 2) == 0;  // 10Hz blink
            LedOn(0, flash);
            LedOn(1, flash);
            LedOn(2, flash);
            LedOn(3, flash);
            LedOn(4, flash);
            LedOn(5, flash);
        } else {
            // Normal: show position in progression (0-17)
            LedOn(0, progressionIndex == 0 || progressionIndex == 6 || progressionIndex == 8 || progressionIndex == 12 || progressionIndex == 16);
            LedOn(1, progressionIndex == 1 || progressionIndex == 7 || progressionIndex == 9 || progressionIndex == 14);
            LedOn(2, progressionIndex == 2 || progressionIndex == 8 || progressionIndex == 9 || progressionIndex == 11 || progressionIndex == 15 || progressionIndex == 17);
            LedOn(3, progressionIndex == 3 || progressionIndex == 7 || progressionIndex == 10 || progressionIndex == 13 || progressionIndex == 15 || progressionIndex == 16);
            LedOn(4, progressionIndex == 4 || progressionIndex == 10 || progressionIndex == 11 || progressionIndex == 12 || progressionIndex == 14);
            LedOn(5, progressionIndex == 5 || progressionIndex == 6 || progressionIndex == 13 || progressionIndex == 17);
        }
    }
};

// Global pointer for Core 1 to access shared state
static ResonatingStrings* g_resonator = nullptr;

void core1_handler() {
    sleep_ms(500);  // Wait for USB to settle

    // Serial state
    char lineBuf[128];
    int linePos = 0;

    // YIN pitch detector state (all local to Core 1)
    const int YIN_W = 150;
    const int YIN_MIN_LAG = 8;
    const int YIN_MAX_LAG = 150;

    int32_t hp_state = 0, lp1_state = 0, lp2_state = 0;
    int bufIdx = 0, decCount = 0;
    int scanLag = 0;
    int64_t runningSum = 0, prevNorm = 0;
    bool foundDip = false;
    int32_t pitchMV = 0;
    int32_t yinAmplitude = 0;   // peak-hold envelope of decimated signal
    uint32_t ringTail = 0;

    for (int i = 0; i < 512; i++) yinBuf[i] = 0;

    while (true) {
        // Process all pending audio samples from Core 0
        uint32_t head = yinRingHead;
        while (ringTail != head) {
            int16_t sample = yinRing[ringTail & (YIN_RING_SIZE - 1)];
            ringTail++;

            // Bandpass + 4x decimation into circular buffer
            hp_state += (sample - hp_state) >> 9;
            int32_t hp = sample - hp_state;
            lp1_state += (hp - lp1_state) >> 3;
            lp2_state += (lp1_state - lp2_state) >> 3;
            if (++decCount >= 4) {
                decCount = 0;
                yinBuf[bufIdx] = (int16_t)lp2_state;
                bufIdx = (bufIdx + 1) & 511;
                // Peak-hold amplitude for pitch gate
                int32_t absVal = lp2_state < 0 ? -lp2_state : lp2_state;
                if (absVal > yinAmplitude) {
                    yinAmplitude = absVal;                    // instant attack
                } else {
                    yinAmplitude -= yinAmplitude >> 11;       // release τ ≈ 170ms at 12kHz
                }
            }
        }

        // Compute up to 4 YIN lags per iteration (full window each, no chunking)
        if (scanLag <= YIN_MAX_LAG && ringTail > 0) {
            int base = (bufIdx - 1) & 511;
            for (int n = 0; n < 4 && scanLag <= YIN_MAX_LAG; n++) {
                uint64_t sum = 0;
                int lag = scanLag;
                for (int i = 0; i < YIN_W; i++) {
                    int idx1 = (base - i) & 511;
                    int idx2 = (idx1 - lag) & 511;
                    int32_t diff = (int32_t)yinBuf[idx1] - yinBuf[idx2];
                    sum += (uint32_t)diff * (uint32_t)diff;
                }

                bool detected = false;
                int64_t d = (int64_t)sum;
                if (scanLag > 0) {
                    runningSum += d;
                    if (scanLag >= YIN_MIN_LAG) {
                        // CMND: d'(τ) = d(τ)*τ / runningSum
                        int64_t dTimesLag = d * (int64_t)scanLag;
                        int64_t lhs = (dTimesLag << 3) + (dTimesLag << 1); // ×10
                        bool belowThreshold = (lhs < runningSum);
                        // Detect first local minimum after crossing below threshold
                        if (foundDip && dTimesLag > prevNorm) {
                            int32_t period = (scanLag - 1) * 4;
                            int32_t newMV = periodToMillivolts(period);
                            if (yinAmplitude > 40) {            // gate: hold pitch when signal is too weak
                                pitchMV += (newMV - pitchMV) >> 3;
                                yinSharedPitchMV = pitchMV;
                            }
                            detected = true;
                        }
                        foundDip = foundDip || belowThreshold;
                        prevNorm = dTimesLag;
                    }
                }
                if (detected) {
                    scanLag = 0;
                    runningSum = 0;
                    prevNorm = 0;
                    foundDip = false;
                    break;
                }
                scanLag++;
            }
            if (scanLag > YIN_MAX_LAG) {
                scanLag = 0;
                runningSum = 0;
                prevNorm = 0;
                foundDip = false;
            }
        }

        // Non-blocking serial check
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            g_resonator->checkPendingFlashSave();
        } else if (c == '\n' || c == '\r') {
            if (linePos > 0) {
                lineBuf[linePos] = '\0';
                g_resonator->handleSerialCommand(lineBuf);
                linePos = 0;
            }
        } else if (linePos < 127) {
            lineBuf[linePos++] = (char)c;
        }
    }
}

int main() {
    stdio_init_all();  // Initialize USB CDC

    static ResonatingStrings resonator;
    g_resonator = &resonator;
    resonator.EnableNormalisationProbe();

    // Enable lockout handler so Core 0 can be safely paused during flash operations
    multicore_lockout_victim_init();

    multicore_launch_core1(core1_handler);
    resonator.Run();
    return 0;
}
