/*
 * Tab5 ControlHome panel (demo build, no network yet)
 * Hardware init follows m5stack/M5Tab5-UserDemo (platforms/tab5/main/hal/hal_esp32.cpp).
 */
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bsp/m5stack_tab5.h>
#include "config.h"
#include "layout.h"
#include "livescene.h"
#include "uiwatch.h"
#include "rx8130.h"
#include "sdcard.h"
#include "alarm.h"
#include "audio.h"
#include "ui.h"
#include "weather.h"
#include "web.h"
#include "wifi.h"

static const char* TAG = "tab5panel";
static RX8130_Class rtc;

// Poland: CET/CEST with automatic daylight saving. The RTC keeps local wall time.
static const char* TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

// After an NTP sync, store the exact local time in the RTC so it survives reboots without network
static void save_time_to_rtc()
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    rtc.setTime(&t);  // note: modifies t
}

// Set the RTC from the firmware build time when it has never been set (dead coin cell / first boot).
static void sync_time_from_rtc(i2c_master_bus_handle_t bus)
{
    if (!rtc.begin(bus, 0x32)) {
        ESP_LOGW(TAG, "RTC not found");
        return;
    }
    rtc.initBat();
    rtc.clearIrqFlags();
    rtc.disableIrq();

    struct tm t = {};
    rtc.getTime(&t);
    int year = t.tm_year + 1900;
    if (year < 2026 || year > 2035) {
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = {};
        int day, year, hh, mm, ss;
        sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
        t = {};
        t.tm_year = year - 1900;
        t.tm_mon  = (strstr(months, mon) - months) / 3;
        t.tm_mday = day;
        t.tm_hour = hh;
        t.tm_min  = mm;
        t.tm_sec  = ss;
        ESP_LOGI(TAG, "RTC not set, using build time");
        rtc.setTime(&t);
        vTaskDelay(pdMS_TO_TICKS(50));
        rtc.getTime(&t);
    }
    ESP_LOGI(TAG, "time: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    t.tm_isdst         = -1;  // RTC holds local time; let mktime work out CET/CEST
    struct timeval now = {.tv_sec = mktime(&t), .tv_usec = 0};
    settimeofday(&now, NULL);
}

extern "C" void app_main(void)
{
    uiwatch_boot();
    bsp_i2c_init();
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    bsp_io_expander_pi4ioe_init(bus);

    uiwatch_mark("start");
    config_load();
    layout_load();
    alarm_init();
    uiwatch_mark("settings, alarms");
    setenv("TZ", TIMEZONE, 1);
    tzset();
    sync_time_from_rtc(bus);

    bsp_reset_tp();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 6;
    bsp_display_cfg_t cfg = {.lvgl_port_cfg = port_cfg,
                             .buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES,
                             .double_buffer = true,
                             .flags         = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
                                 .buff_dma = false,
#else
                                 .buff_dma = true,
#endif
                                 .buff_spiram = true,
                                 .sw_rotate   = true,
                             }};
    lv_display_t* disp = bsp_display_start_with_config(&cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    uiwatch_mark("display, LVGL");

    bsp_display_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    livescene_boot_early(uiwatch_froze_last_start());  // the saved design: ui_create() shows it right away
    uiwatch_mark("saved design");
    ui_create();
    uiwatch_start();  // a frozen screen restarts the Tab5
    bsp_display_unlock();
    uiwatch_mark("screens");

    bsp_display_backlight_on();
    ESP_LOGI(TAG, "ready");

    wifi_start(save_time_to_rtc);
    uiwatch_mark("Wi-Fi");
    weather_start();
    uiwatch_mark("weather");
    web_start();
    uiwatch_mark("web page");
    sdcard_start();
    uiwatch_mark("SD card");
    audio_start();  // the speaker and the sound task at boot, while internal RAM is still free
    uiwatch_mark("sound");

    // Never return: when app_main ends, FreeRTOS cleans up the task's thread-local storage (set by the
    // network stack for the web server) and rejects its cleanup function because the code runs from PSRAM.
    for (;;) vTaskDelay(portMAX_DELAY);
}
