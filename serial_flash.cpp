#define COMPUTERCARD_NOIMPL
#include "resonator.h"

// Check and perform deferred flash save (called from Core 1)
void ResonatingStrings::checkPendingFlashSave() {
    if (pendingFlashSave) {
        pendingFlashSave = false;
        saveProgressionToFlash();
    }
}

// Serial command handler (called from Core 1)
void ResonatingStrings::handleSerialCommand(const char* cmd) {
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

void ResonatingStrings::handleSet(const char* args) {
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

void ResonatingStrings::handleGet() {
    int bufIdx = activeBuffer;
    printf("PROG ");
    for (int i = 0; i < progressionBuffers[bufIdx].length; i++) {
        if (i > 0) printf(",");
        printf("%d", (int)progressionBuffers[bufIdx].chords[i]);
    }
    printf("\n");
}

void ResonatingStrings::handleGetArp() {
    printf("ARP %d\n", arpDivision);
}

void ResonatingStrings::handleSetArp(const char* args) {
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

void ResonatingStrings::handleGetPat() {
    printf("PAT %d\n", arpPattern);
}

void ResonatingStrings::handleSetPat(const char* args) {
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

void ResonatingStrings::handleGetOut() {
    printf("OUT %d,%d,%d,%d,%d,%d,%d,%d\n", (int)cv1Mode, (int)cv2Mode, (int)p1Mode, (int)p2Mode, (int)pi1Mode, (int)pi2Mode, (int)ao2Mode, (int)ci1Mode);
}

void ResonatingStrings::handleSetOut(const char* args) {
    int vals[8];
    int count = 0;
    const char* p = args;
    while (*p && count < 8) {
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
    if (count < 6 || count > 8) {
        printf("ERR invalid_out_args\n");
        return;
    }
    // Validate ranges
    if (vals[0] < 0 || vals[0] > 6 ||
        vals[1] < 0 || vals[1] > 6 ||
        vals[2] < 0 || vals[2] > 3 ||
        vals[3] < 0 || vals[3] > 5 ||
        vals[4] < 0 || vals[4] > 2 ||
        vals[5] < 0 || vals[5] > 3 ||
        (count >= 7 && (vals[6] < 0 || vals[6] > 2)) ||
        (count >= 8 && (vals[7] < 0 || vals[7] > 1))) {
        printf("ERR invalid_out_mode\n");
        return;
    }
    cv1Mode = vals[0];
    cv2Mode = vals[1];
    p1Mode = vals[2];
    p2Mode = vals[3];
    pi1Mode = vals[4];
    pi2Mode = vals[5];
    if (count >= 7) ao2Mode = vals[6];
    if (count >= 8) ci1Mode = vals[7];
    outputModesChanged = true;
    handleGetOut();
    saveProgressionToFlash();
}

void ResonatingStrings::handleGetDiv() {
    printf("DIV %d\n", (int)clockDivRatio);
}

void ResonatingStrings::handleSetDiv(const char* args) {
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
void ResonatingStrings::saveProgressionToFlash() {
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
    data[29] = (uint8_t)ao2Mode;
    data[30] = (uint8_t)ci1Mode;

    // Pause Core 0 during flash operation (XIP is blocked during erase/program)
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_PROG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_PROG_OFFSET, data, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

// Load progression from flash, returns true if valid data found
bool ResonatingStrings::loadProgressionFromFlash() {
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
    cv1Mode = (cv1Val <= 6) ? cv1Val : 0;
    uint8_t cv2Val = flash_data[23];
    cv2Mode = (cv2Val <= 6) ? cv2Val : 0;
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

    // Load Audio Out 2 mode (byte 29), default to 0 (audio) if invalid
    uint8_t ao2Val = flash_data[29];
    ao2Mode = (ao2Val <= 2) ? ao2Val : 0;

    // Load CV Input 1 mode (byte 30), default to 0 (1V/oct) if invalid
    uint8_t ci1Val = flash_data[30];
    ci1Mode = (ci1Val <= 1) ? ci1Val : 0;

    return true;
}

// Reset progression to factory defaults and save to flash
void ResonatingStrings::resetToDefaults() {
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
    cv1Mode = CVOUT_ARP;
    cv2Mode = CVOUT_RES_ENV;
    p1Mode = P1_AUDIO_TRIG;
    p2Mode = P2_CHORD_TRIG;
    pi1Mode = PI1_PLUCK;
    pi2Mode = PI2_ADVANCE;
    clockDivRatio = 2;
    ao2Mode = AO2_AUDIO;
    ci1Mode = CI1_VOCT;
    outputModesChanged = true;

    // Defer flash save to Core 1 (Core 0 can't lock out Core 1)
    pendingFlashSave = true;
}
