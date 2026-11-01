#pragma once

#include <esp_http_server.h>

#include <cJSON.h>

// Live preview of Tab5 Studio scenes and the saved design (/api/scene/...), see livescene.cpp
void livescene_register(httpd_handle_t server);
void livescene_boot_early(bool froze);  // display lock held, before ui_create(): loads the saved design (froze: the last start ended in a freeze)
void livescene_state(cJSON* root);  // scene_live, scene_saved, scene_saved_err, scene_room_kb for /api/state; not under the display lock
