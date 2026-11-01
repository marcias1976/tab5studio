# Changes made for Tab5 Studio

This is `espressif/esp_lvgl_port` 2.5.0 (Apache License 2.0, see `license.txt`), copied into the project and changed:

- `include/esp_lvgl_port.h`, `src/lvgl9/esp_lvgl_port.c`: `lvgl_port_lock_holder()` returns the task that holds the LVGL mutex, for the screen watchdog (`firmware/main/uiwatch.cpp`).
- `src/lvgl9/esp_lvgl_port_touch.c`: the touch input stays in timer (polling) mode even with the touch interrupt.

Each change is marked with a `Tab5:` comment in the code.
