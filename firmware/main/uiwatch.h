#pragma once

#include <cJSON.h>

// Screen watchdog, see uiwatch.cpp
void uiwatch_boot(void);               // first thing at start: reads the breadcrumb of a crashed start
void uiwatch_start(void);              // display lock held, after ui_create()
bool uiwatch_froze_last_start(void);   // the last start ended in a freeze restart (call once at boot)
void uiwatch_state(cJSON* root);       // "last_freeze", "last_crash", "heap" for /api/state
void uiwatch_crumb(const char* step);  // names the step that runs now (shown as "last_crash" after a panic)
void uiwatch_mark(const char* step);   // notes the free internal RAM after a start step ("boot_heap"; step: a literal)
