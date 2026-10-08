#include <stdlib.h>
#include <string.h>

#include "airmidi/rtp_journal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static rtpj_tx_t tx;
static rtpj_rx_t rx;
static uint8_t emitted[256][3];
static int n_emitted;

static void on_emit(void *ctx, uint8_t status, uint8_t d1, uint8_t d2) {
    (void)ctx;
    TEST_ASSERT_TRUE(n_emitted < 256);
    emitted[n_emitted][0] = status, emitted[n_emitted][1] = d1, emitted[n_emitted][2] = d2;
    n_emitted++;
}

static void reset(uint16_t first_seq) {
    rtpj_tx_init(&tx, first_seq);
    rtpj_rx_init(&rx);
    n_emitted = 0;
}

static void send(uint16_t seq, uint32_t now, uint8_t s, uint8_t d1, uint8_t d2) {
    rtpj_tx_record(&tx, seq, now, s, d1, d2);
}

static void assert_emitted(int i, uint8_t s, uint8_t d1, uint8_t d2) {
    TEST_ASSERT_TRUE_MESSAGE(i < n_emitted, "fewer repairs than expected");
    uint8_t want[3] = {s, d1, d2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, emitted[i], 3);
}

// One packet's worth of history, byte for byte against RFC 6295 (figures 8, 9, A.2.1, A.3.1, A.5.1, A.6.1, A.8.1).
static void test_journal_bytes_for_one_packet(void) {
    reset(100);
    uint8_t j[RTPJ_TIMES * 64];
    TEST_ASSERT_EQUAL(3, rtpj_tx_build(&tx, 100, 0, j, sizeof j));  // nothing yet: header only
    const uint8_t empty[] = {0x80, 0x00, 0x64};                     // S = 1, no channels, checkpoint 100
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, j, 3);

    send(100, 0, 0xB0, 0, 1);     // bank MSB 1
    send(100, 0, 0xB0, 32, 2);    // bank LSB 2
    send(100, 0, 0xC0, 5, 0);     // program 5
    send(100, 0, 0xB0, 7, 100);   // volume
    send(100, 0, 0x90, 60, 100);  // note on
    send(100, 0, 0x80, 62, 0);    // note off
    send(100, 0, 0xE0, 0x10, 0x40);
    send(100, 0, 0xD0, 0x30, 0);
    size_t n = rtpj_tx_build(&tx, 101, 10, j, sizeof j);
    const uint8_t want[] = {
        0x20, 0x00, 0x64,  // S = 0 (all of it was in the previous packet), A, 1 channel, checkpoint
        0x00, 0x15, 0xDA,  // channel 0, length 21, chapters P C W N T
        0x05, 0x81, 0x02,  // P: program 5, bank 1 / 2
        0x02, 0x00, 0x01, 0x07, 0x64, 0x20, 0x02,  // C: 3 logs, oldest first (one packet: by number)
        0x10, 0x40,                                // W
        0x01, 0x77, 0x3C, 0xE4, 0x02,              // N: B = 0 (NoteOff last packet), 1 log (60, Y, 100), bits 56-63: 62
        0x30,                                      // T
    };
    TEST_ASSERT_EQUAL(sizeof want, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, j, sizeof want);

    // A packet later the same history is no longer about the previous packet (S bits 1), and the note is too old
    // to be worth playing late (Y = 0).
    n = rtpj_tx_build(&tx, 102, 200, j, sizeof j);
    TEST_ASSERT_EQUAL(sizeof want, n);
    TEST_ASSERT_EQUAL_HEX8(0xA0, j[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, j[3]);
    TEST_ASSERT_EQUAL_HEX8(0x85, j[6]);
    TEST_ASSERT_EQUAL_HEX8(0x81, j[18]);  // B = 1
    TEST_ASSERT_EQUAL_HEX8(0xBC, j[20]);
    TEST_ASSERT_EQUAL_HEX8(0x64, j[21]);  // Y = 0
}

static void test_receiver_repairs_from_the_journal(void) {
    reset(100);
    uint8_t j[512];
    send(100, 0, 0xB0, 0, 1);
    send(100, 0, 0xB0, 32, 2);
    send(100, 0, 0xC0, 5, 0);
    send(100, 0, 0xB0, 7, 100);
    send(100, 0, 0x90, 60, 100);
    send(100, 0, 0x80, 62, 0);
    send(100, 0, 0xE0, 0x10, 0x40);
    send(100, 0, 0xD0, 0x30, 0);
    size_t n = rtpj_tx_build(&tx, 101, 10, j, sizeof j);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    assert_emitted(0, 0xB0, 0, 1);  // chapter P first: bank, then program
    assert_emitted(1, 0xB0, 32, 2);
    assert_emitted(2, 0xC0, 5, 0);
    assert_emitted(3, 0xB0, 7, 100);  // chapter C: the bank logs match what P just played
    assert_emitted(4, 0xE0, 0x10, 0x40);
    assert_emitted(5, 0x90, 60, 100);  // note 62 was never on here: nothing to switch off
    assert_emitted(6, 0xD0, 0x30, 0);
    TEST_ASSERT_EQUAL(7, n_emitted);
    TEST_ASSERT_EQUAL(7, rx.repairs);

    // The same journal again changes nothing: the receiver is in step now.
    n_emitted = 0;
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(0, n_emitted);
}

// The case the journal exists for: a NoteOff lost, the note would hang forever.
static void test_lost_note_off_is_played(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0x91, 64, 90);
    rtpj_rx_track(&rx, 0x91, 64, 90);  // packet 0 arrived
    send(1, 5, 0x81, 64, 0);           // packet 1 is lost
    size_t n = rtpj_tx_build(&tx, 2, 10, j, sizeof j);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(1, n_emitted);
    assert_emitted(0, 0x81, 64, 64);
}

// A NoteOn lost long ago is not played when the loss is found: the sender clears the Y bit after 100 ms.
static void test_stale_note_on_is_not_played(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0x90, 50, 80);
    size_t n = rtpj_tx_build(&tx, 1, RTPJ_PLAY_MS + 1, j, sizeof j);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(0, n_emitted);
    n = rtpj_tx_build(&tx, 1, RTPJ_PLAY_MS - 1, j, sizeof j);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(1, n_emitted);
}

// All Notes Off is journalled by count (its value means nothing); a lost one is played once, and the notes it ended
// leave the journal (they are no longer "N-active").
static void test_lost_all_notes_off_is_counted(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0x90, 60, 100);
    rtpj_rx_track(&rx, 0x90, 60, 100);
    send(1, 1, 0xB0, 123, 0);  // lost
    size_t n = rtpj_tx_build(&tx, 2, 2, j, sizeof j);
    const uint8_t want[] = {0x20, 0x00, 0x00, 0x00, 0x06, 0x40, 0x00, 0x7B, 0xC1};  // C: 123, count tool, 1
    TEST_ASSERT_EQUAL(sizeof want, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, j, sizeof want);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(1, n_emitted);
    assert_emitted(0, 0xB0, 123, 0);
    TEST_ASSERT_EQUAL_HEX32(0, rx.ch[0].notes[1]);  // note 60 is off
    // Seen once more: the counts agree, nothing is played twice.
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    TEST_ASSERT_EQUAL(1, n_emitted);
}

// A lost Program Change comes back with its bank, in the order a synth expects: MSB, LSB, program.
static void test_lost_program_change_comes_with_its_bank(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0xB2, 0, 3);
    send(0, 0, 0xB2, 32, 9);
    send(0, 0, 0xC2, 17, 0);
    send(1, 1, 0xB2, 0, 4);  // a new bank MSB without its LSB, then a program: the LSB counts as 0 (RFC 6295 A.2)
    send(1, 1, 0xC2, 18, 0);
    size_t n = rtpj_tx_build(&tx, 2, 2, j, sizeof j);
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, n, on_emit, NULL));
    assert_emitted(0, 0xB2, 0, 4);
    assert_emitted(1, 0xB2, 32, 0);
    assert_emitted(2, 0xC2, 18, 0);
    // Chapter C then puts the LSB controller back to the value it really has.
    assert_emitted(3, 0xB2, 32, 9);
    TEST_ASSERT_EQUAL(4, n_emitted);
}

// RPN/NRPN controllers would need chapter M, which is not written: they stay out of chapter C too.
static void test_parameter_controllers_are_not_journalled(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0xB0, 101, 0);
    send(0, 0, 0xB0, 100, 0);
    send(0, 0, 0xB0, 6, 2);
    send(0, 0, 0xB0, 38, 0);
    send(0, 0, 0xB0, 96, 0);
    TEST_ASSERT_EQUAL(3, rtpj_tx_build(&tx, 1, 1, j, sizeof j));
}

// Receiver feedback moves the checkpoint: what the receiver confirmed leaves the journal. Feedback from the past,
// or about packets not sent yet, is ignored.
static void test_feedback_moves_the_checkpoint(void) {
    reset(65534);  // across the sequence number wrap
    uint8_t j[512];
    send(65534, 0, 0x90, 40, 1);
    send(65535, 1, 0x90, 41, 1);
    send(0, 2, 0x90, 42, 1);
    rtpj_tx_ack(&tx, 5, 1);  // about a packet not sent yet
    TEST_ASSERT_EQUAL(65534, tx.checkpoint);
    rtpj_tx_ack(&tx, 65535, 1);  // the receiver has 65534 and 65535
    TEST_ASSERT_EQUAL(0, tx.checkpoint);
    size_t n = rtpj_tx_build(&tx, 1, 3, j, sizeof j);
    TEST_ASSERT_EQUAL(3 + 3 + 2 + 2, n);  // one channel, chapter N with one log: note 42 only
    TEST_ASSERT_EQUAL_HEX8(0x2A, j[8] & 0x7F);
    rtpj_tx_ack(&tx, 65000, 1);  // stale
    TEST_ASSERT_EQUAL(0, tx.checkpoint);
    rtpj_tx_ack(&tx, 0, 1);
    TEST_ASSERT_FALSE(rtpj_tx_pending(&tx));
    TEST_ASSERT_EQUAL(3, rtpj_tx_build(&tx, 1, 3, j, sizeof j));
}

// A journal that does not fit is not sent half: the checkpoint moves up and the journal goes out empty.
static void test_oversized_journal_moves_the_checkpoint(void) {
    reset(0);
    uint8_t j[RTPJ_TIMES * 64];
    for (int ch = 0; ch < RTPJ_SLOTS; ch++)
        for (int c = 0; c < 120; c++) send(0, 0, 0xB0 | ch, c, 1);
    TEST_ASSERT_EQUAL(3, rtpj_tx_build(&tx, 1, 1, j, 500));
    TEST_ASSERT_EQUAL(1, tx.overflows);
    TEST_ASSERT_EQUAL(1, tx.checkpoint);
    TEST_ASSERT_FALSE(rtpj_tx_pending(&tx));
}

// History is kept for four channels at once: a fifth busy channel takes the slot of the one idle longest, whose
// unconfirmed history is given up (counted). Once the receiver confirms, the slots are free again.
static void test_fifth_busy_channel_takes_the_oldest_slot(void) {
    reset(0);
    uint8_t j[512];
    for (int ch = 0; ch < 4; ch++) send(0, 0, 0x90 | ch, 60, 100);
    send(1, 1, 0x90, 61, 100);  // channel 0 again: now the newest
    send(2, 2, 0x94, 62, 100);  // a fifth channel: channel 1 is the one idle longest
    TEST_ASSERT_EQUAL(1, tx.overflows);
    size_t n = rtpj_tx_build(&tx, 3, 3, j, sizeof j);
    TEST_ASSERT_EQUAL_HEX8(0x23, j[0] & 0x2F);  // 4 channel journals: 0, 2, 3, 4
    int chans[4], k = 0;
    for (size_t pos = 3; pos < n; pos += (size_t)((j[pos] & 3) << 8 | j[pos + 1])) chans[k++] = j[pos] >> 3 & 0x0F;
    TEST_ASSERT_EQUAL(4, k);
    TEST_ASSERT_EQUAL(0, chans[0]);
    TEST_ASSERT_EQUAL(2, chans[1]);
    TEST_ASSERT_EQUAL(4, chans[3]);
    rtpj_tx_ack(&tx, 2, 3);
    TEST_ASSERT_FALSE(rtpj_tx_pending(&tx));
    for (int i = 0; i < RTPJ_SLOTS; i++) TEST_ASSERT_EQUAL(-1, tx.slot[i].ch);
}

// Journals from other senders may hold chapters we do not repair from (M, E, A, the system journal): they are
// skipped by their lengths and the chapters after them still work.
static void test_unsupported_chapters_are_skipped(void) {
    reset(0);
    const uint8_t j[] = {
        0x60, 0x00, 0x05,        // Y and A, 1 channel
        0x00, 0x04, 0xAA, 0xBB,  // system journal, length 4 (contents not looked at)
        0x08, 0x10, 0x3E,        // channel 1, length 16, chapters M W N E T
        0x00, 0x03, 0x7F,        // M: length 3
        0x00, 0x40,              // W: centre
        0x81, 0xF1, 0x40, 0xC0,  // N: 1 log (note 64 vel 64, Y), no NoteOff bits
        0x00, 0x40, 0x40,        // E: 1 log
        0x22,                    // T
    };
    TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, sizeof j, on_emit, NULL));
    TEST_ASSERT_EQUAL(3, n_emitted);
    assert_emitted(0, 0xE1, 0x00, 0x40);
    assert_emitted(1, 0x91, 64, 64);
    assert_emitted(2, 0xD1, 0x22, 0);
}

// Truncated or inconsistent journals are rejected without reading past their end (ASan watches), and random bytes
// never crash the parser.
static void test_malformed_journals_are_rejected(void) {
    reset(0);
    uint8_t j[512];
    send(0, 0, 0xB0, 7, 1);
    send(0, 0, 0x90, 60, 1);
    size_t n = rtpj_tx_build(&tx, 1, 1, j, sizeof j);
    for (size_t cut = 0; cut < n; cut++) {
        uint8_t *copy = malloc(cut ? cut : 1);
        memcpy(copy, j, cut);
        TEST_ASSERT_FALSE(rtpj_rx_recover(&rx, copy, cut, on_emit, NULL));
        free(copy);
    }
    const uint8_t bad_n[] = {0x20, 0, 0, 0x00, 0x05, 0x08, 0x00, 0x52};  // chapter N with LOW 5 > HIGH 2
    TEST_ASSERT_FALSE(rtpj_rx_recover(&rx, bad_n, sizeof bad_n, on_emit, NULL));
    srand(7);
    for (int round = 0; round < 20000; round++) {
        size_t len = (size_t)(rand() % 64);
        uint8_t *r = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) r[i] = (uint8_t)rand();
        n_emitted = 0;
        rtpj_rx_recover(&rx, r, len, on_emit, NULL);
        free(r);
    }
}

// The recovery journal mandate (RFC 6295 section 4): whatever is lost, once a packet gets through the receiver's
// state matches the sender's. A random stream on 4 channels through a channel that loses up to half the packets,
// with feedback every 10 packets; at the end the receiver must hold the same notes, controllers, programs, bends
// and pressures as the sender. (All Notes Off and Reset All Controllers are tested on their own above: they make
// some state stale on purpose.)
static rtpj_rx_t truth;

static void random_command(uint8_t *s, uint8_t *d1, uint8_t *d2) {
    static const uint8_t ctls[] = {1, 2, 7, 10, 11, 33, 64, 71, 74, 91, 93};
    uint8_t ch = (uint8_t)(rand() % 4);
    switch (rand() % 8) {
        case 0:
        case 1:
            *s = 0x90 | ch, *d1 = (uint8_t)(36 + rand() % 24), *d2 = (uint8_t)(1 + rand() % 127);
            break;
        case 2:
            *s = 0x80 | ch, *d1 = (uint8_t)(36 + rand() % 24), *d2 = 64;
            break;
        case 3:
            *s = 0x90 | ch, *d1 = (uint8_t)(36 + rand() % 24), *d2 = 0;  // NoteOn velocity 0
            break;
        case 4:
            *s = 0xB0 | ch, *d1 = ctls[rand() % sizeof ctls], *d2 = (uint8_t)(rand() % 128);
            break;
        case 5:
            *s = 0xC0 | ch, *d1 = (uint8_t)(rand() % 128), *d2 = 0;
            break;
        case 6:
            *s = 0xE0 | ch, *d1 = (uint8_t)(rand() % 128), *d2 = (uint8_t)(rand() % 128);
            break;
        default:
            *s = 0xD0 | ch, *d1 = (uint8_t)(rand() % 128), *d2 = 0;
            break;
    }
}

static void mandate_run(int loss_pct, unsigned seed) {
    reset(65400);  // crosses the sequence number wrap
    rtpj_rx_init(&truth);
    srand(seed);
    uint16_t seq = 65400, highest = 0;
    bool started = false;
    uint32_t now = 0;
    for (int i = 0; i < 3000; i++, seq++, now++) {
        uint8_t j[1400];
        size_t jl = rtpj_tx_build(&tx, seq, now, j, 1200);
        uint8_t cmds[4][3];
        int n = i < 2990 ? 1 + rand() % 4 : 0;  // the last packets carry only their journals
        for (int k = 0; k < n; k++) {
            random_command(&cmds[k][0], &cmds[k][1], &cmds[k][2]);
            rtpj_tx_record(&tx, seq, now, cmds[k][0], cmds[k][1], cmds[k][2]);
            rtpj_rx_track(&truth, cmds[k][0], cmds[k][1], cmds[k][2]);
        }
        bool lost = i < 2990 && rand() % 100 < loss_pct;
        if (lost) continue;
        if (!started || seq != (uint16_t)(highest + 1)) TEST_ASSERT_TRUE(rtpj_rx_recover(&rx, j, jl, on_emit, NULL));
        n_emitted = 0;
        for (int k = 0; k < n; k++) rtpj_rx_track(&rx, cmds[k][0], cmds[k][1], cmds[k][2]);
        started = true;
        highest = seq;
        if (i % 10 == 9) rtpj_tx_ack(&tx, highest, seq + 1);
    }
    for (int ch = 0; ch < 16; ch++) {
        TEST_ASSERT_EQUAL_HEX32_ARRAY(truth.ch[ch].notes, rx.ch[ch].notes, 4);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(truth.ch[ch].ctl, rx.ch[ch].ctl, 128);
        TEST_ASSERT_EQUAL(truth.ch[ch].program, rx.ch[ch].program);
        TEST_ASSERT_EQUAL(truth.ch[ch].bend, rx.ch[ch].bend);
        TEST_ASSERT_EQUAL(truth.ch[ch].pressure, rx.ch[ch].pressure);
    }
    TEST_ASSERT_TRUE(rx.repairs > 0 || loss_pct == 0);
    TEST_ASSERT_EQUAL(0, tx.overflows);
}

static void test_mandate_light_loss(void) { mandate_run(5, 1); }
static void test_mandate_heavy_loss(void) { mandate_run(50, 2); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_journal_bytes_for_one_packet);
    RUN_TEST(test_receiver_repairs_from_the_journal);
    RUN_TEST(test_lost_note_off_is_played);
    RUN_TEST(test_stale_note_on_is_not_played);
    RUN_TEST(test_lost_all_notes_off_is_counted);
    RUN_TEST(test_lost_program_change_comes_with_its_bank);
    RUN_TEST(test_parameter_controllers_are_not_journalled);
    RUN_TEST(test_feedback_moves_the_checkpoint);
    RUN_TEST(test_oversized_journal_moves_the_checkpoint);
    RUN_TEST(test_fifth_busy_channel_takes_the_oldest_slot);
    RUN_TEST(test_unsupported_chapters_are_skipped);
    RUN_TEST(test_malformed_journals_are_rejected);
    RUN_TEST(test_mandate_light_loss);
    RUN_TEST(test_mandate_heavy_loss);
    return UNITY_END();
}
