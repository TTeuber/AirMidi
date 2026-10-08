#include "airmidi/settings.h"

#include <stdio.h>
#include <string.h>

void settings_defaults(settings_t *s, const uint8_t mac[6]) {
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "airmidi-%02x%02x", mac[4], mac[5]);
    s->mode = AIRMIDI_MODE_PLAY;
    s->sysex_timeout_ms = 500;
}

const char *settings_check(const settings_t *s) {
    size_t n = strnlen(s->name, sizeof s->name);
    if (n == 0) return "name is empty";
    if (n > SETTINGS_NAME_MAX) return "name is too long";
    // The name doubles as an mDNS host name (RFC 1123 label): letters, digits and '-', not at either end.
    for (size_t i = 0; i < n; i++) {
        char c = s->name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return "name may only contain letters, digits and '-'";
    }
    if (s->name[0] == '-' || s->name[n - 1] == '-') return "name may not start or end with '-'";
    if ((unsigned)s->mode >= AIRMIDI_MODE_COUNT) return "unknown mode";
    if (s->sysex_timeout_ms < SETTINGS_SYSEX_TIMEOUT_MIN || s->sysex_timeout_ms > SETTINGS_SYSEX_TIMEOUT_MAX)
        return "SysEx timeout out of range";
    return NULL;
}

const char *airmidi_mode_name(airmidi_mode_t m) {
    switch (m) {
        case AIRMIDI_MODE_PLAY:
            return "play";
        case AIRMIDI_MODE_NETWORK:
            return "network";
        case AIRMIDI_MODE_SETUP:
            return "setup";
        default:
            return "?";
    }
}
