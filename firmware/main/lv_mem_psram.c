/*
 * LVGL memory in PSRAM (LV_USE_STDLIB_MALLOC = custom, sdkconfig CONFIG_LV_USE_CUSTOM_MALLOC).
 *
 * With the C library allocator every LVGL object, style and font cache entry is a small malloc, and with
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL those land in the scarce internal RAM, where each rebuild of the clock faces
 * (a new design from Studio) left it a little more fragmented. PSRAM has more than 20 MB free; internal RAM stays for
 * Wi-Fi, DMA and the tasks. Falls back to internal RAM only if PSRAM is full.
 */
#include <esp_heap_caps.h>
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size)
{
    void* p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    void* n = heap_caps_realloc(p, new_size, LV_CAPS);
    return n || !new_size ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void* p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
