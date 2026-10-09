#include "usb_midi.h"

#include <stdio.h>
#include <string.h>

#include "airmidi/midi1.h"
#include "airmidi/usbmidi.h"
#include "bus.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/periph_ctrl.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb";

#define BACKLOG 32  // packets waiting for room in TinyUSB's FIFO

enum { ITF_CDC, ITF_CDC_DATA, ITF_MIDI, ITF_MIDI_STREAMING, ITF_COUNT };
enum { EP_CDC_NOTIF = 0x81, EP_CDC_OUT = 0x02, EP_CDC_IN = 0x82, EP_MIDI_OUT = 0x03, EP_MIDI_IN = 0x83 };

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 100),  // bus powered, 100 mA
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI, 5, EP_MIDI_OUT, EP_MIDI_IN, 64),
};

static char product[32], serial[13];
static const char *strings[] = {
    (const char[]){0x09, 0x04},  // English
    "airmidi",                   // manufacturer
    product,                     // product: the box's name, so the Mac lists it by it
    serial,                      // the MAC address
    "airmidi console",
    product,  // the MIDI port
};

static midi1_in_t parser;
static usbmidi_tx_t tx;
static uint8_t backlog[BACKLOG][4];
static int n_backlog;
static usb_midi_stats_t stats;

// ---- receive (TinyUSB task) ----------------------------------------------------------------------------------

void tud_midi_rx_cb(uint8_t itf) {
    uint8_t pkt[4];
    while (tud_midi_n_packet_read(itf, pkt)) {
        stats.rx_packets++;
        uint8_t b[3];
        int n = usbmidi_packet_bytes(pkt, b);
        for (int i = 0; i < n; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&parser, b[i], u);
            for (int j = 0; j < k; j++) bus_send(PORT_USB, &u[j]);
        }
    }
}

// A host has taken the device (configured it) or let it go: PORT_USB is up while it is attached.
static void on_event(tinyusb_event_t *e, void *arg) {
    if (e->id != TINYUSB_EVENT_ATTACHED && e->id != TINYUSB_EVENT_DETACHED) return;
    stats.mounted = e->id == TINYUSB_EVENT_ATTACHED;
    bus_set_up(PORT_USB, stats.mounted);
}

// ---- send (bus task) -----------------------------------------------------------------------------------------

static void send_backlog(void) {
    int sent = 0;
    while (sent < n_backlog && tud_midi_packet_write(backlog[sent])) sent++;
    if (!sent) return;
    stats.tx_packets += sent;
    n_backlog -= sent;
    memmove(backlog, backlog[sent], n_backlog * sizeof backlog[0]);
}

static void usb_write(void *ctx, const ump_t *u) {
    uint8_t pk[USBMIDI_MAX_PACKETS][4];
    int n = usbmidi_tx_encode(&tx, u, pk);
    send_backlog();
    for (int i = 0; i < n; i++) {
        if (!n_backlog && tud_midi_packet_write(pk[i])) {
            stats.tx_packets++;
        } else if (n_backlog < BACKLOG) {
            memcpy(backlog[n_backlog++], pk[i], 4);
        } else {
            stats.tx_dropped++;  // the host is not reading
        }
    }
}

static void usb_flush(void *ctx) { send_backlog(); }

static size_t usb_room(void *ctx) { return n_backlog + USBMIDI_MAX_PACKETS <= BACKLOG ? MIDI1_OUT_MAX : 0; }

static void usb_on_up(void *ctx) {
    n_backlog = 0;
    usbmidi_tx_init(&tx, 0);
}

static const bus_output_ops_t ops = {
    .write = usb_write, .flush = usb_flush, .room = usb_room, .on_up = usb_on_up, .byte_stream = true};

// ---- the serial port: console, and esptool's reset into the bootloader -----------------------------------------

// esptool resets a chip by its serial port's control lines: RTS (EN low) with DTR off, then DTR (GPIO0 low) with
// RTS off; each line is set on its own, so both are on for a moment in between. Over USB there are no such wires,
// so the same sequence restarts the chip into its ROM bootloader. A terminal opening the port sets both and never
// RTS alone, so it does not trigger it.
static void line_state(int itf, cdcacm_event_t *e) {
    static bool armed;
    bool dtr = e->line_state_changed_data.dtr, rts = e->line_state_changed_data.rts;
    if (rts && !dtr) {
        armed = true;
    } else if (armed && dtr && !rts) {
        ESP_LOGW(TAG, "esptool reset");
        usb_midi_bootloader();
    } else if (!dtr && !rts) {
        armed = false;
    }
}

// Every restart: give the USB PHY back to the chip's USB-Serial-JTAG first. The PHY selection lives in the RTC
// domain and survives a restart; left on USB OTG, the next boot's console output waited forever for a USB-Serial-
// JTAG nobody could reach, and the box never came back (found on the dev kit). Restarts into the ROM bootloader need
// it too: the bootloader talks USB-Serial-JTAG.
static void give_back_phy(void) {
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);  // the internal PHY
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

void usb_midi_start(const char *name) {
    esp_register_shutdown_handler(give_back_phy);
    snprintf(product, sizeof product, "%s", name);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(serial, sizeof serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    midi1_in_init(&parser, 0);
    usbmidi_tx_init(&tx, 0);
    bus_register_output(PORT_USB, &ops, NULL);

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(on_event);
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof strings / sizeof strings[0];
    cfg.descriptor.full_speed_config = config_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0, .callback_line_state_changed = line_state};
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
    // The console (log and commands) moves here. A dev kit's UART console keeps what comes before this and panics.
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
    ESP_LOGI(TAG, "USB MIDI device \"%s\", console on its serial port", product);
}

void usb_midi_get_stats(usb_midi_stats_t *out) { *out = stats; }

void usb_midi_bootloader(void) {
    ESP_LOGW(TAG, "restarting into the ROM bootloader");
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
