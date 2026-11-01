#pragma once

#include <lvgl.h>

// Builds the clock screens. Call with the LVGL lock held.
void ui_create(void);

// Switch the clock between dark and light mode (call with the display lock held)
void ui_set_night(bool on);

// A still "Updating… N%" screen while a program arrives over Wi-Fi: every flash erase stalls both cores, and the
// clock faces redrawing each second then starve the idle tasks into a watchdog restart. Call under the display lock.
void ui_show_update(int percent);
void ui_hide_update(void);  // back to the screen before (the update failed)

// Web page controls (call with the display lock held)
int ui_face(void);           // current clock face 0–2, -1 on another screen
void ui_show_face(int k);
bool ui_night(void);
bool ui_screen_on(void);
bool ui_intro_playing(void);  // the intro (built-in or from Studio) is on screen
void ui_set_screen(bool on);
void ui_play_intro(void);
void ui_config_changed(void);  // after changing config()
void ui_layout_changed(void);  // after layout_set()

// Scene sent live from Tab5 Studio (display lock held). The arrays must stay valid until ui_scene_live_stop().
struct SceneFace;
void ui_scene_live(const SceneFace faces[2]);
void ui_scene_live_stop(void);
void ui_scene_live_third(const SceneFace* f);  // Wariant-3 from Studio (NULL = none); the arrays must stay valid until the next call
int ui_face_count(void);  // 2, or 3 with Wariant-3
void ui_scene_live_preset(const SceneFace faces[2]);  // like ui_scene_live, but before ui_create() (saved design at boot)
void ui_scene_live_intro(const SceneFace* intro);  // intro from Studio (NULL = the built-in one again)
void ui_scene_live_screens(const SceneFace* alarm, const SceneFace* timer);  // alarm / timer looks from Studio (NULL = built-in)
void ui_preview_screen(int which);  // 1 = the alarm screen (silent), 2 = the timer
bool ui_scene_is_live(void);

// Ringing alarm: full-screen view with Drzemka / Wyłącz (display lock held); snooze_min > 0 is shown on the button
void ui_alarm_show(const char* label, int snooze_min);
void ui_alarm_hide(void);
lv_obj_t* ui_alarm_view(void);  // the ringing alarm or the open timer view, else NULL

// Timer (display lock held): open 1 = show, -1 = hide; start_s > 0 starts counting; cancel stops
void ui_timer(int open, int start_s, bool cancel);
int ui_timer_left(bool* running);
void ui_timer_restart(int seconds);  // counts again from `seconds` in the background (timer snooze)
