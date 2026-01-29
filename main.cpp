#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <cstring>
#include <cstdio>

/**
Resonator Workshop System Computer Card - by Johan Eklund
version 0.3 - 2026-01-09

Four resonating strings using Karplus-Strong synthesis
*/

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

class ResonatingStrings : public ComputerCard
{
private:
    static const int MAX_DELAY_SIZE = 1920;

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
    int progressionIndex;

    bool lastSwitchDown;

    int32_t pulseExciteEnvelope;
    uint32_t noiseState;

    int32_t dcState1, dcState2, dcState3, dcState4;

    // Smoothed delay values for glide between chord modes (fixed-point, 8 bits fraction)
    int32_t smoothDelay1, smoothDelay2, smoothDelay3, smoothDelay4;

    // One-pole lowpass filter for damping
    int32_t dampingFilter(int32_t input, int32_t& state, int32_t coefficient) {
        state += (((input - state) * coefficient + 32768) >> 16);
        return state;
    }

    // Process one string with linear interpolation for fractional delay
    int32_t processString(int16_t* delayLine, int& writeIndex, int delayLength,
                         int32_t& filterState, int32_t& dcState, int32_t excitation,
                         int32_t dampingCoeff, int32_t frac) {
        // Read two adjacent samples from delay line
        int readIndex1 = writeIndex - delayLength;
        if (readIndex1 < 0) readIndex1 += MAX_DELAY_SIZE;
        int readIndex2 = readIndex1 - 1;
        if (readIndex2 < 0) readIndex2 += MAX_DELAY_SIZE;

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
        writeIndex = (writeIndex + 1) % MAX_DELAY_SIZE;

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
                          currentMode(HARMONIC), activeBuffer(0), progressionChanged(false),
                          progressionIndex(0), lastSwitchDown(true),
                          pulseExciteEnvelope(0), noiseState(12345),
                          dcState1(0), dcState2(0), dcState3(0), dcState4(0),
                          smoothDelay1(0), smoothDelay2(0), smoothDelay3(0), smoothDelay4(0) {
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

        // Initialize delay lines with silence
        for (int i = 0; i < MAX_DELAY_SIZE; i++) {
            delayLine1[i] = 0;
            delayLine2[i] = 0;
            delayLine3[i] = 0;
            delayLine4[i] = 0;
        }
    }

    // Serial command handler (called from Core 1)
    void handleSerialCommand(const char* cmd) {
        if (strncmp(cmd, "SET ", 4) == 0) {
            handleSet(cmd + 4);
        } else if (strcmp(cmd, "GET") == 0) {
            handleGet();
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

protected:
    void ProcessSample() override {
        int16_t audioIn1 = AudioIn1();
        int16_t audioIn2 = AudioIn2();
        int32_t audioIn = ((int32_t)audioIn1 + (int32_t)audioIn2 + 1) >> 1;

        // Check for serial progression update from Core 1
        if (progressionChanged) {
            progressionIndex = 0;
            currentMode = progressionBuffers[activeBuffer].chords[0];
            progressionChanged = false;
        }

        // Mode switching (switch down or pulse in 2)
        Switch switchPos = SwitchVal();
        bool switchDown = (switchPos == Down);
        if ((switchDown && !lastSwitchDown) || PulseIn2RisingEdge()) {
            int bufIdx = activeBuffer;
            progressionIndex = (progressionIndex + 1) % progressionBuffers[bufIdx].length;
            currentMode = progressionBuffers[bufIdx].chords[progressionIndex];
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
        if (delayLength1 < 10) delayLength1 = 10;
        if (delayLength2 < 10) delayLength2 = 10;
        if (delayLength3 < 10) delayLength3 = 10;
        if (delayLength4 < 10) delayLength4 = 10;
        if (delayLength1 > MAX_DELAY_SIZE - 1) delayLength1 = MAX_DELAY_SIZE - 1;
        if (delayLength2 > MAX_DELAY_SIZE - 1) delayLength2 = MAX_DELAY_SIZE - 1;
        if (delayLength3 > MAX_DELAY_SIZE - 1) delayLength3 = MAX_DELAY_SIZE - 1;
        if (delayLength4 > MAX_DELAY_SIZE - 1) delayLength4 = MAX_DELAY_SIZE - 1;

        // DAMPING CONTROL (Y Knob + CV2)
        int32_t dampingKnob = KnobVal(Y) + CVIn2();  // 0-4095 knob + CV
        if (dampingKnob > 4095) dampingKnob = 4095;
        if (dampingKnob < 0) dampingKnob = 0;

        // Map to filter coefficient (more damping = lower coefficient, longer decay = higher coefficient)
        int32_t dampingCoeff = 32000 + ((dampingKnob * 33300) / 4095);

        // Excitation amounts for each string
        // String 1 gets full input, others get scaled versions (sympathetic response)
        int32_t excitation1 = audioIn >> 2;  // Direct excitation
        int32_t excitation2 = audioIn >> 4;  // Sympathetic response
        int32_t excitation3 = audioIn >> 4;  // Sympathetic response
        int32_t excitation4 = audioIn >> 3;  // 4th string

        // Pulse1 triggers a noise burst to excite strings (like plucking)
        if (PulseIn1RisingEdge()) {
            pulseExciteEnvelope = 2048;  // Start excitation envelope
        }

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
            resonatorOut1 = out1 / 2;
            resonatorOut2 = out1 / 2;
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

        // LED indicators - all 6 LEDs show chord mode
        // Single LED: HARMONIC(0), FIFTH(1), MAJOR7(2), MINOR7(3), DIM(4), SUS4(5)
        // Pairs for extended chords:
        // ADD9(6): 0+5, MAJOR10(7): 1+3, SUS2(8): 0+2, MAJOR(9): 1+2
        // MINOR(10): 3+4, MAJOR6(11): 2+4, DOM7(12): 0+4, MIN9(13): 3+5
        // TANPURA_PA(14): 1+4, TANPURA_MA(15): 2+3, TANPURA_NI(16): 0+3, TANPURA_NI_KOMAL(17): 2+5
        LedOn(0, currentMode == HARMONIC || currentMode == ADD9 || currentMode == SUS2 || currentMode == DOM7 || currentMode == TANPURA_NI);
        LedOn(1, currentMode == FIFTH || currentMode == MAJOR10 || currentMode == MAJOR || currentMode == TANPURA_PA);
        LedOn(2, currentMode == MAJOR7 || currentMode == SUS2 || currentMode == MAJOR || currentMode == MAJOR6 || currentMode == TANPURA_MA || currentMode == TANPURA_NI_KOMAL);
        LedOn(3, currentMode == MINOR7 || currentMode == MAJOR10 || currentMode == MINOR || currentMode == MIN9 || currentMode == TANPURA_MA || currentMode == TANPURA_NI);
        LedOn(4, currentMode == DIM || currentMode == MINOR || currentMode == MAJOR6 || currentMode == DOM7 || currentMode == TANPURA_PA);
        LedOn(5, currentMode == SUS4 || currentMode == ADD9 || currentMode == MIN9 || currentMode == TANPURA_NI_KOMAL);
    }
};

// Global pointer for Core 1 to access shared state
static ResonatingStrings* g_resonator = nullptr;

void core1_serial_handler() {
    sleep_ms(500);  // Wait for USB to settle

    char lineBuf[128];
    int linePos = 0;

    while (true) {
        int c = getchar_timeout_us(10000);  // 10ms timeout
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\n' || c == '\r') {
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
    multicore_launch_core1(core1_serial_handler);
    resonator.Run();
    return 0;
}
