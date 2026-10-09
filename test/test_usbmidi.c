#include <stdlib.h>
#include <string.h>

#include "airmidi/usbmidi.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static usbmidi_tx_t tx;
static uint8_t pk[USBMIDI_MAX_PACKETS][4];

static void assert_packet(int i, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t want[4] = {b0, b1, b2, b3};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pk[i], 4);
}

// Every kind of message gets the CIN the class spec gives it (table 4-1), on the configured cable.
static void test_messages_and_their_code_index_numbers(void) {
    usbmidi_tx_init(&tx, 1);
    ump_t note = ump_m1_cv(0, 0x93, 60, 100);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &note, pk));
    assert_packet(0, 0x19, 0x93, 60, 100);
    ump_t pc = ump_m1_cv(0, 0xC0, 5, 0);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &pc, pk));
    assert_packet(0, 0x1C, 0xC0, 5, 0);
    ump_t clock = ump_system(0, 0xF8, 0, 0);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &clock, pk));
    assert_packet(0, 0x1F, 0xF8, 0, 0);
    ump_t spp = ump_system(0, 0xF2, 1, 2);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &spp, pk));
    assert_packet(0, 0x13, 0xF2, 1, 2);
    ump_t song = ump_system(0, 0xF3, 7, 0);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &song, pk));
    assert_packet(0, 0x12, 0xF3, 7, 0);
    ump_t tune = ump_system(0, 0xF6, 0, 0);
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &tune, pk));
    assert_packet(0, 0x15, 0xF6, 0, 0);
}

// SysEx: 3 bytes a packet with CIN 4, the last one 5, 6 or 7 by how many bytes end it.
static void test_sysex_lengths(void) {
    usbmidi_tx_init(&tx, 0);
    const uint8_t d[4] = {1, 2, 3, 4};
    ump_t one = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 1);  // F0 01 F7
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &one, pk));
    assert_packet(0, 0x07, 0xF0, 1, 0xF7);
    ump_t two = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 2);  // F0 01 02 | F7
    TEST_ASSERT_EQUAL(2, usbmidi_tx_encode(&tx, &two, pk));
    assert_packet(0, 0x04, 0xF0, 1, 2);
    assert_packet(1, 0x05, 0xF7, 0, 0);
    ump_t three = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 3);  // F0 01 02 | 03 F7
    TEST_ASSERT_EQUAL(2, usbmidi_tx_encode(&tx, &three, pk));
    assert_packet(1, 0x06, 3, 0xF7, 0);
    ump_t empty = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 0);  // F0 F7
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &empty, pk));
    assert_packet(0, 0x06, 0xF0, 0xF7, 0);
}

// The box's own reading of what it sent, packets back to bytes, for SysEx of every length up to 40 split into UMP
// packets, with a clock in the middle: the same messages come out, and the clock is not inside the SysEx bytes.
static void test_round_trip_through_a_parser(void) {
    for (int len = 0; len <= 40; len++) {
        usbmidi_tx_init(&tx, 0);
        midi1_in_t in;
        midi1_in_init(&in, 0);
        uint8_t data[40];
        for (int i = 0; i < len; i++) data[i] = (uint8_t)(i * 7 % 128);
        ump_t u[16];
        int n = 0;
        for (int off = 0; off < len || n == 0; off += 6) {
            int k = len - off < 6 ? len - off : 6;
            bool first = off == 0, last = off + 6 >= len;
            uint8_t st = first && last ? UMP_SYSEX_COMPLETE
                         : first       ? UMP_SYSEX_START
                         : last        ? UMP_SYSEX_END
                                       : UMP_SYSEX_CONTINUE;
            u[n++] = ump_sysex7(0, st, data + off, k);
            if (n == 2) u[n++] = ump_system(0, 0xF8, 0, 0);
        }
        uint8_t buf[64];
        sysex7_rx_t rx;
        sysex7_rx_init(&rx, buf, sizeof buf);
        int done = 0, clocks = 0;
        for (int i = 0; i < n; i++) {
            int np = usbmidi_tx_encode(&tx, &u[i], pk);
            for (int p = 0; p < np; p++) {
                uint8_t b[3];
                int nb = usbmidi_packet_bytes(pk[p], b);
                for (int j = 0; j < nb; j++) {
                    ump_t out[2];
                    int no = midi1_in_feed(&in, b[j], out);
                    for (int q = 0; q < no; q++) {
                        if (ump_mt(&out[q]) == UMP_MT_SYSTEM) clocks++;
                        if (sysex7_rx_push(&rx, &out[q])) done++;
                    }
                }
            }
        }
        TEST_ASSERT_EQUAL_MESSAGE(1, done, "SysEx not delivered whole");
        TEST_ASSERT_EQUAL(len, rx.len);
        if (len) TEST_ASSERT_EQUAL_HEX8_ARRAY(data, buf, len);
        TEST_ASSERT_EQUAL(n > 2 ? 1 : 0, clocks);
    }
}

// A message in the middle of an unfinished SysEx ends it first (a byte stream cannot nest them).
static void test_unfinished_sysex_is_ended(void) {
    usbmidi_tx_init(&tx, 0);
    const uint8_t d[6] = {1, 2, 3, 4, 5, 6};
    ump_t start = ump_sysex7(0, UMP_SYSEX_START, d, 4);  // F0 01 02 | (03 04 waiting)
    TEST_ASSERT_EQUAL(1, usbmidi_tx_encode(&tx, &start, pk));
    ump_t note = ump_m1_cv(0, 0x90, 1, 1);
    TEST_ASSERT_EQUAL(2, usbmidi_tx_encode(&tx, &note, pk));
    assert_packet(0, 0x07, 3, 4, 0xF7);
    assert_packet(1, 0x09, 0x90, 1, 1);
    ump_t stray = ump_sysex7(0, UMP_SYSEX_END, d, 2);  // its rest: dropped
    TEST_ASSERT_EQUAL(0, usbmidi_tx_encode(&tx, &stray, pk));
}

// MIDI 2.0 goes out translated down; an RPN becomes four CCs, four packets.
static void test_midi2_is_translated_down(void) {
    usbmidi_tx_init(&tx, 0);
    ump_t rpn = ump_m2_cv(0, 0x2, 0, 0, 1, 0x80000000);  // RPN 0/1 (fine tuning)
    TEST_ASSERT_EQUAL(4, usbmidi_tx_encode(&tx, &rpn, pk));
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(0x0B, pk[i][0]);
    ump_t pernote = ump_m2_cv(0, 0x0, 0, 60, 1, 0);  // per-note controller: no MIDI 1.0 equivalent
    TEST_ASSERT_EQUAL(0, usbmidi_tx_encode(&tx, &pernote, pk));
}

static void test_packet_bytes(void) {
    uint8_t b[3];
    const uint8_t note[4] = {0x09, 0x90, 60, 100}, cable[4] = {0x01, 1, 2, 3}, end1[4] = {0x05, 0xF7, 0, 0};
    TEST_ASSERT_EQUAL(3, usbmidi_packet_bytes(note, b));
    TEST_ASSERT_EQUAL(0, usbmidi_packet_bytes(cable, b));  // cable events: reserved
    TEST_ASSERT_EQUAL(1, usbmidi_packet_bytes(end1, b));
    TEST_ASSERT_EQUAL_HEX8(0xF7, b[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_messages_and_their_code_index_numbers);
    RUN_TEST(test_sysex_lengths);
    RUN_TEST(test_round_trip_through_a_parser);
    RUN_TEST(test_unfinished_sysex_is_ended);
    RUN_TEST(test_midi2_is_translated_down);
    RUN_TEST(test_packet_bytes);
    return UNITY_END();
}
