// The web UI and its JSON API on port 80 (http://<name>.local, or 192.168.4.1 in Setup mode). No login: it is
// meant for the local network, and the one dangerous thing it can do, installing firmware, only accepts images
// signed with the project's key.
#pragma once

#include "airmidi/settings.h"

typedef struct {
    void (*set_mode)(airmidi_mode_t mode);  // switch now and remember it
} webui_hooks_t;

void webui_start(const webui_hooks_t *hooks);

// True once the server answers (the OTA self-test asks).
bool webui_running(void);
