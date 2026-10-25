// The web UI and its JSON API on port 80 (http://<name>.local, or 192.168.4.1 in Setup mode). No login: it is
// meant for the local network, and the one dangerous thing it can do, installing firmware, only accepts images
// signed with the project's key. The JSON endpoints are components/api's (call api_init() first); this serves the
// page, firmware uploads, the core dump and the /ws control surface.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void webui_start(void);

// Words the web pages could not keep up with (for /api/status).
uint32_t webui_ws_dropped(void);

// True once the server answers (the OTA self-test asks).
bool webui_running(void);
