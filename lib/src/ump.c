#include "airmidi/ump.h"

#include <string.h>

int ump_words_for_mt(uint8_t mt) {
    // M2-104 table 4: 0x0-0x2, 0x6, 0x7 are 32 bit; 0x3, 0x4, 0x8-0xA 64 bit; 0xB, 0xC 96 bit; the rest 128 bit.
    static const uint8_t words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return words[mt & 0xF];
}

bool ump_equal(const ump_t *a, const ump_t *b) {
    int n = ump_words(a);
    return ump_mt(a) == ump_mt(b) && memcmp(a->w, b->w, n * sizeof a->w[0]) == 0;
}

ump_t ump_system(uint8_t group, uint8_t status, uint8_t d1, uint8_t d2) {
    ump_t u = {{(uint32_t)UMP_MT_SYSTEM << 28 | (uint32_t)(group & 0xF) << 24 | (uint32_t)status << 16 |
                (uint32_t)(d1 & 0x7F) << 8 | (d2 & 0x7F)}};
    return u;
}

ump_t ump_m1_cv(uint8_t group, uint8_t status, uint8_t d1, uint8_t d2) {
    ump_t u = {{(uint32_t)UMP_MT_M1_CV << 28 | (uint32_t)(group & 0xF) << 24 | (uint32_t)status << 16 |
                (uint32_t)(d1 & 0x7F) << 8 | (d2 & 0x7F)}};
    return u;
}

ump_t ump_sysex7(uint8_t group, uint8_t status, const uint8_t *data, int n) {
    uint8_t b[6] = {0};
    if (n > 6) n = 6;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) b[i] = data[i] & 0x7F;
    ump_t u = {{(uint32_t)UMP_MT_SYSEX7 << 28 | (uint32_t)(group & 0xF) << 24 | (uint32_t)(status & 0xF) << 20 |
                    (uint32_t)n << 16 | (uint32_t)b[0] << 8 | b[1],
                (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]}};
    return u;
}

int ump_sysex7_data(const ump_t *u, uint8_t out[6]) {
    int n = (u->w[0] >> 16) & 0xF;
    if (n > 6) n = 6;  // a malformed count must not overrun the caller
    const uint8_t all[6] = {(uint8_t)(u->w[0] >> 8),  (uint8_t)u->w[0],        (uint8_t)(u->w[1] >> 24),
                            (uint8_t)(u->w[1] >> 16), (uint8_t)(u->w[1] >> 8), (uint8_t)u->w[1]};
    for (int i = 0; i < n; i++) out[i] = all[i] & 0x7F;
    return n;
}

ump_t ump_m2_cv(uint8_t group, uint8_t opcode, uint8_t channel, uint8_t index1, uint8_t index2, uint32_t data) {
    ump_t u = {{(uint32_t)UMP_MT_M2_CV << 28 | (uint32_t)(group & 0xF) << 24 | (uint32_t)(opcode & 0xF) << 20 |
                    (uint32_t)(channel & 0xF) << 16 | (uint32_t)index1 << 8 | index2,
                data}};
    return u;
}

int midi1_data_len(uint8_t status) {
    if (status < 0x80) return -1;
    if (status < 0xF0) {
        uint8_t kind = status & 0xF0;
        return kind == 0xC0 || kind == 0xD0 ? 1 : 2;
    }
    switch (status) {
        case 0xF1:  // MTC quarter frame
        case 0xF3:  // song select
            return 1;
        case 0xF2:  // song position pointer
            return 2;
        case 0xF6:  // tune request
        case 0xF7:  // end of exclusive
        case 0xF8:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFE:
        case 0xFF:  // real-time
            return 0;
        default:  // F0 (variable), F4 F5 F9 FD (undefined)
            return -1;
    }
}
