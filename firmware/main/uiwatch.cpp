/*
 * Screen watchdog: when LVGL has not run for 15 s (touch and drawing are frozen), the Tab5
 * restarts instead of staying stuck, and remembers which task held the display lock. /api/state shows it as
 * "last_freeze", and the start after a freeze counts like a crash for the saved design's safe mode.
 * Breadcrumbs (uiwatch_crumb) name the step that runs now; they survive a crash in RTC memory, so after a panic
 * /api/state shows where it happened ("last_crash"), together with the free memory ("heap").
 */
#include <time.h>
#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdlib.h>
#if CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>
#endif
#include "uiwatch.h"

static const char* TAG = "uiwatch";
static const int64_t FROZEN_US = 15 * 1000000;
static volatile int64_t s_beat;  // last run of the LVGL timer below (us since boot)

// The step running now; kept over a crash restart (RTC memory is not cleared by a software reset)
static const uint32_t CRUMB_MAGIC = 0x7AB5C0DE;
RTC_NOINIT_ATTR static uint32_t s_crumb_magic;
RTC_NOINIT_ATTR static char s_crumb[40];
RTC_NOINIT_ATTR static uint32_t s_crumb_up;    // seconds since start when it was set
RTC_NOINIT_ATTR static uint32_t s_crumb_heap;  // free internal RAM then
static char s_last_crash[40];                  // the step at the last panic (read at boot)
static uint32_t s_last_crash_up, s_last_crash_heap;

// Free internal RAM after each start step ("boot_heap" in /api/state): shows what takes it
struct BootMark {
    const char* step;
    uint32_t free, block;
};
static BootMark s_marks[16];
static int s_nmarks;

void uiwatch_mark(const char* step)
{
    char crumb[40];
    snprintf(crumb, sizeof(crumb), "start: %s", step);
    uiwatch_crumb(crumb);
    if (s_nmarks < 16) s_marks[s_nmarks++] = {step, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)};
}

void uiwatch_crumb(const char* step)
{
    strlcpy(s_crumb, step ? step : "", sizeof(s_crumb));
    s_crumb_up    = (uint32_t)(esp_timer_get_time() / 1000000);
    s_crumb_heap  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_crumb_magic = CRUMB_MAGIC;
}

static void on_beat(lv_timer_t* t)
{
    LV_UNUSED(t);
    s_beat = esp_timer_get_time();
}

// A program updated over Wi-Fi counts as good after 30 s of a working screen; until then a restart goes back
// to the previous one (bootloader rollback)
static void on_app_ok(void* arg)
{
    (void)arg;
    uiwatch_mark("30 s after start");
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_timer_get_time() - s_beat < FROZEN_US && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "new program marked as good");
    }
}

// esp_timer task (internal stack, may write NVS)
static void on_check(void* arg)
{
    (void)arg;
    if (!s_beat || esp_timer_get_time() - s_beat < FROZEN_US) return;
    TaskHandle_t holder = (TaskHandle_t)lvgl_port_lock_holder();
    const char* who     = holder ? pcTaskGetName(holder) : "?";
    ESP_LOGE(TAG, "screen frozen for %lld s, display lock held by %s: restarting", (esp_timer_get_time() - s_beat) / 1000000, who);
    nvs_handle_t h;
    if (nvs_open("uiwatch", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t n = 0;
        nvs_get_u32(h, "n", &n);
        nvs_set_u32(h, "n", n + 1);
        nvs_set_str(h, "who", who);
        nvs_set_u32(h, "up", (uint32_t)(esp_timer_get_time() / 1000000));
        nvs_set_u32(h, "time", (uint32_t)time(NULL));
        nvs_set_u8(h, "pending", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

void uiwatch_boot(void)
{
    // The breadcrumb of a start that ended in a crash (read before this start sets new ones)
    esp_reset_reason_t r = esp_reset_reason();
    if (s_crumb_magic == CRUMB_MAGIC && (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT)) {
        s_crumb[sizeof(s_crumb) - 1] = 0;
        strlcpy(s_last_crash, s_crumb[0] ? s_crumb : "?", sizeof(s_last_crash));
        s_last_crash_up = s_crumb_up, s_last_crash_heap = s_crumb_heap;
        ESP_LOGW(TAG, "last start crashed at \"%s\" (%u s, %u B free)", s_last_crash, (unsigned)s_last_crash_up, (unsigned)s_last_crash_heap);
    }
    uiwatch_crumb("");
}

bool uiwatch_froze_last_start(void)
{
    nvs_handle_t h;
    uint8_t pending = 0;
    if (nvs_open("uiwatch", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "pending", &pending);
    if (pending) {
        nvs_set_u8(h, "pending", 0);
        nvs_commit(h);
    }
    nvs_close(h);
    return pending;
}

void uiwatch_start(void)
{
    s_beat = esp_timer_get_time();
    lv_timer_create(on_beat, 1000, NULL);
    esp_timer_create_args_t args = {};
    args.callback                = on_check;
    args.name                    = "uiwatch";
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 5 * 1000000);
    esp_timer_create_args_t ok_args = {};
    ok_args.callback                = on_app_ok;
    ok_args.name                    = "app_ok";
    esp_timer_handle_t ok_timer;
    if (esp_timer_create(&ok_args, &ok_timer) == ESP_OK) esp_timer_start_once(ok_timer, 30 * 1000000);
}

void uiwatch_state(cJSON* root)
{
    cJSON* hp = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(hp, "int_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(hp, "int_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(hp, "int_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(hp, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_HEAP_TASK_TRACKING
    // Internal RAM held by each task (blocks it allocated and has not freed): who takes it. Names only for the
    // tasks that still run; a finished task's handle is not touched.
    static const int MAX_T = 40;
    heap_task_totals_t* totals = (heap_task_totals_t*)heap_caps_calloc(MAX_T, sizeof(heap_task_totals_t), MALLOC_CAP_SPIRAM);
    TaskStatus_t* tasks        = (TaskStatus_t*)heap_caps_calloc(MAX_T, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (totals && tasks) {
        size_t n                  = 0;
        heap_task_info_params_t p = {};
        p.caps[0] = p.mask[0] = MALLOC_CAP_INTERNAL;
        p.caps[1] = p.mask[1] = MALLOC_CAP_SPIRAM;
        p.totals              = totals;
        p.num_totals          = &n;
        p.max_totals          = MAX_T;
        heap_caps_get_per_task_info(&p);
        UBaseType_t nt = uxTaskGetSystemState(tasks, MAX_T, NULL);
        qsort(totals, n, sizeof(heap_task_totals_t), [](const void* a, const void* b) {
            size_t x = ((const heap_task_totals_t*)a)->size[0], y = ((const heap_task_totals_t*)b)->size[0];
            return x < y ? 1 : x > y ? -1 : 0;
        });
        cJSON* ht = cJSON_AddArrayToObject(root, "heap_tasks");
        for (size_t i = 0; i < n && i < 12; i++) {
            const char* name = totals[i].task ? "(finished)" : "(before start)";
            for (UBaseType_t k = 0; k < nt; k++)
                if (tasks[k].xHandle == totals[i].task) name = tasks[k].pcTaskName;
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "task", name);
            cJSON_AddNumberToObject(o, "int", totals[i].size[0]);
            cJSON_AddNumberToObject(o, "blocks", totals[i].count[0]);
            cJSON_AddNumberToObject(o, "psram", totals[i].size[1]);
            cJSON_AddItemToArray(ht, o);
        }
    }
    free(totals);
    free(tasks);
#endif
    cJSON* marks = cJSON_AddArrayToObject(root, "boot_heap");
    for (int i = 0; i < s_nmarks; i++) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "step", s_marks[i].step);
        cJSON_AddNumberToObject(m, "free", s_marks[i].free);
        cJSON_AddNumberToObject(m, "block", s_marks[i].block);
        cJSON_AddItemToArray(marks, m);
    }
    // Program slots: which runs, which starts next time, the state of the other one (updates over Wi-Fi)
    const esp_partition_t *run = esp_ota_get_running_partition(), *boot = esp_ota_get_boot_partition(), *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    cJSON* app = cJSON_AddObjectToObject(root, "app");
    cJSON_AddStringToObject(app, "running", run ? run->label : "?");
    cJSON_AddStringToObject(app, "boot", boot ? boot->label : "?");
    cJSON_AddStringToObject(app, "other", other ? other->label : "?");
    if (other && esp_ota_get_state_partition(other, &st) == ESP_OK) cJSON_AddNumberToObject(app, "other_state", st);
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK) cJSON_AddNumberToObject(app, "running_state", st);
    if (s_last_crash[0]) {
        cJSON* c = cJSON_AddObjectToObject(root, "last_crash");
        cJSON_AddStringToObject(c, "step", s_last_crash);
        cJSON_AddNumberToObject(c, "uptime", s_last_crash_up);
        cJSON_AddNumberToObject(c, "int_free", s_last_crash_heap);
    }
    nvs_handle_t h;
    uint32_t n = 0, up = 0, t = 0;
    char who[24] = "";
    size_t len   = sizeof(who);
    if (nvs_open("uiwatch", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "n", &n);
    if (n) {
        nvs_get_str(h, "who", who, &len);
        nvs_get_u32(h, "up", &up);
        nvs_get_u32(h, "time", &t);
        cJSON* o = cJSON_AddObjectToObject(root, "last_freeze");
        cJSON_AddNumberToObject(o, "count", n);
        cJSON_AddStringToObject(o, "task", who);
        cJSON_AddNumberToObject(o, "uptime", up);
        cJSON_AddNumberToObject(o, "time", t);
    }
    nvs_close(h);
}
