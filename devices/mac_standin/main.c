// A MIDI-CI device on the Mac, for testing the box without a real synth: it sits on a MIDI port pair (the
// AudioFuse, cabled to the box's TRS jacks), answers Discovery and Property Exchange with lib/midici_responder,
// and prints every channel message it receives, so a test can see a web UI fader arrive as a Control Change.
//
//   devices/mac_standin/build.sh
//   devices/mac_standin/build/mac_standin [--port <name part>] [--seconds <n>]   (default port: Audiofuse)
//   devices/mac_standin/build/mac_standin --dump    the resources it serves, as one JSON object
//
// Output lines: "ready <MUID>", "ci <what it answered>", "cc <channel 1-16> <controller> <value>",
// "msg <hex>" for other channel messages.
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "airmidi/midici.h"
#include "airmidi/midici_responder.h"
#include "airmidi/ump.h"

// A small synth voice: the controls the demo page should draw (M2-117 AllCtrlList).
static const char *device_info =
    "{\"manufacturerId\":[125,0,0],\"manufacturer\":\"Educational Use\",\"familyId\":[0,0],\"family\":\"airmidi\","
    "\"modelId\":[2,0],\"model\":\"Mac stand-in\",\"versionId\":[0,1,0,0],\"version\":\"0.1\"}";

static const char *ctrl_list =
    "["
    "{\"title\":\"Cutoff\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[74],\"priority\":1,"
    "\"default\":2147483648,\"paramPath\":\"/filter/cutoff\"},"
    "{\"title\":\"Resonance\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[71],\"priority\":1,"
    "\"default\":0,\"paramPath\":\"/filter/resonance\"},"
    "{\"title\":\"Waveform\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[70],\"priority\":1,"
    "\"default\":0,\"typeHint\":\"valueSelect\",\"ctrlMapId\":\"wave\",\"paramPath\":\"/osc/wave\"},"
    "{\"title\":\"Attack\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[73],\"priority\":2,"
    "\"default\":0,\"paramPath\":\"/env/attack\"},"
    "{\"title\":\"Decay\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[75],\"priority\":2,"
    "\"default\":1073741824,\"paramPath\":\"/env/decay\"},"
    "{\"title\":\"Release\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[72],\"priority\":2,"
    "\"default\":1073741824,\"paramPath\":\"/env/release\"},"
    "{\"title\":\"Glide\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[65],\"priority\":3,"
    "\"default\":0,\"typeHint\":\"toggle\",\"paramPath\":\"/osc/glide\"},"
    "{\"title\":\"Bend\",\"channel\":1,\"ctrlType\":\"pBend\",\"priority\":3,\"default\":2147483648}"
    "]";

static const char *wave_map =
    "[{\"value\":0,\"title\":\"Saw\"},{\"value\":1431655765,\"title\":\"Square\"},"
    "{\"value\":2863311530,\"title\":\"Triangle\"}]";

static const midici_resource_t resources[] = {
    {"DeviceInfo", NULL, NULL},
    {"AllCtrlList", NULL, NULL},
    {"CtrlMapList", "wave", NULL},
};

static midici_resource_t table[3];
static midici_responder_t responder;
static MIDIPortRef out_port;
static MIDIEndpointRef dest;
static uint8_t rx_buf[4096];
static sysex7_rx_t rx;

// Collect a SysEx's packets into one event list and send it.
typedef struct {
    union {
        MIDIEventList list;
        uint8_t room[8192];
    } u;
    MIDIEventPacket *pk;
} sender_t;

static void add_packet(void *ctx, const ump_t *p) {
    sender_t *s = ctx;
    MIDIEventPacket *next = MIDIEventListAdd(&s->u.list, sizeof s->u, s->pk, 0, 2, p->w);
    if (!next) {  // full: send what is there and start again
        MIDISendEventList(out_port, dest, &s->u.list);
        s->pk = MIDIEventListInit(&s->u.list, kMIDIProtocol_1_0);
        next = MIDIEventListAdd(&s->u.list, sizeof s->u, s->pk, 0, 2, p->w);
    }
    s->pk = next;
}

static void send_sysex(void *ctx, const uint8_t *sx, size_t n) {
    (void)ctx;
    static sender_t s;
    s.pk = MIDIEventListInit(&s.u.list, kMIDIProtocol_1_0);
    sysex7_split(0, sx, n, add_packet, &s);
    MIDISendEventList(out_port, dest, &s.u.list);
    midici_msg_t m;
    if (midici_parse(sx, n, &m)) {
        const char *what = m.sub2 == MIDICI_DISCOVERY_REPLY ? "discovery reply"
                           : m.sub2 == MIDICI_PE_CAPS_REPLY ? "pe capabilities reply"
                           : m.sub2 == MIDICI_PE_GET_REPLY  ? "get reply"
                           : m.sub2 == MIDICI_NAK           ? "nak"
                                                            : "other";
        printf("ci %s to %07X (%zu bytes)\n", what, m.dst, n + 2);
        fflush(stdout);
    }
}

static void on_ump(const ump_t *u) {
    if (ump_mt(u) == UMP_MT_SYSEX7) {
        if (sysex7_rx_push(&rx, u)) midici_responder_receive(&responder, rx_buf, rx.len);
        return;
    }
    if (ump_mt(u) != UMP_MT_M1_CV) return;
    uint8_t st = ump_status(u);
    if ((st & 0xF0) == 0xB0)
        printf("cc %d %d %d\n", (st & 0x0F) + 1, ump_byte3(u), ump_byte4(u));
    else
        printf("msg %02X %02X %02X\n", st, ump_byte3(u), ump_byte4(u));
    fflush(stdout);
}

static MIDIEndpointRef find(bool source, const char *part) {
    ItemCount n = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = source ? MIDIGetSource(i) : MIDIGetDestination(i);
        CFStringRef name = NULL;
        char buf[128] = "";
        MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name);
        if (name) CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8), CFRelease(name);
        if (strcasestr(buf, part)) return e;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *part = "Audiofuse";
    double seconds = 0;
    table[0] = resources[0], table[0].json = device_info;
    table[1] = resources[1], table[1].json = ctrl_list;
    table[2] = resources[2], table[2].json = wave_map;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) part = argv[++i];
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        if (!strcmp(argv[i], "--dump")) {
            printf("{\"info\":%s,\"controls\":%s,\"maps\":{\"wave\":%s}}\n", device_info, ctrl_list, wave_map);
            return 0;
        }
    }
    MIDIEndpointRef src = find(true, part);
    dest = find(false, part);
    if (!src || !dest) {
        fprintf(stderr, "no MIDI port pair matching '%s'\n", part);
        return 1;
    }
    uint32_t muid = midici_muid(arc4random());
    const midici_ident_t id = {{0x7D, 0, 0}, 0, 2, {0, 1, 0, 0}};
    midici_responder_init(&responder, muid, &id, table, 3, send_sysex, NULL);
    sysex7_rx_init(&rx, rx_buf, sizeof rx_buf);

    MIDIClientRef client;
    MIDIPortRef in_port;
    MIDIClientCreate(CFSTR("airmidi stand-in"), NULL, NULL, &client);
    MIDIOutputPortCreate(client, CFSTR("out"), &out_port);
    // MIDI 1.0 protocol: channel voice arrives as MT 0x2, SysEx as MT 0x3 packets
    MIDIInputPortCreateWithProtocol(client, CFSTR("in"), kMIDIProtocol_1_0, &in_port,
                                    ^(const MIDIEventList *list, void *ref) {
                                      (void)ref;
                                      const MIDIEventPacket *p = &list->packet[0];
                                      for (UInt32 i = 0; i < list->numPackets; i++) {
                                          for (UInt32 k = 0; k < p->wordCount;) {
                                              ump_t u = {{0}};
                                              int nw = ump_words_for_mt(p->words[k] >> 28);
                                              for (int j = 0; j < nw && k + j < p->wordCount; j++)
                                                  u.w[j] = p->words[k + j];
                                              on_ump(&u);
                                              k += nw;
                                          }
                                          p = MIDIEventPacketNext(p);
                                      }
                                    });
    MIDIPortConnectSource(in_port, src, NULL);
    printf("ready %07X\n", muid);
    fflush(stdout);
    if (seconds > 0)
        usleep((useconds_t)(seconds * 1e6));
    else
        for (;;) pause();
    return 0;
}
