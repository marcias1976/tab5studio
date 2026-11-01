#pragma once

#include <lvgl.h>

// Settings screen: saved Wi-Fi networks (up to 5), network scan with password entry,
// and the weather location by postal code. Call with the LVGL lock held.
void settings_build(void (*on_back)(void));
lv_obj_t* settings_screen(void);

// Refreshes the screen and starts a network scan; call right before showing it
void settings_on_open(void);

// Shows tab 0–4 (Wi-Fi i pogoda, Zegar, Ekran, Harmonogram, Inne) with current values; used for screenshots
void settings_show_tab(int tab);
