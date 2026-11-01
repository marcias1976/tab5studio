/*
 * microSD card on SDMMC slot 0, mounted as FAT at /sdcard.
 * The SDMMC host is shared with the Wi-Fi chip (esp_hosted on slot 1): host init is skipped when it is
 * already running and the card is never unmounted, so the Wi-Fi slot is never torn down.
 */
#include <string.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include "sdcard.h"
#include <sys/stat.h>
#include "layout.h"

static const char* TAG = "sdcard";
static const char* MOUNT = SDCARD_MOUNT;

static sdmmc_card_t* s_card;
static bool s_mounted;
static SemaphoreHandle_t s_lock;
static esp_vfs_fat_mount_config_t s_cfg = {
    .format_if_mount_failed = false,
    .max_files              = 5,
    .allocation_unit_size   = 32 * 1024,
    .disk_status_check_enable = false,
    .use_one_fat            = false,
};

static esp_err_t mount(bool format_if_needed)
{
    static sd_pwr_ctrl_handle_t pwr = NULL;
    if (!pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};  // LDO_VO4 powers the card's IO (see the BSP)
        esp_err_t err                = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
        if (err != ESP_OK) return err;
    }
    sdmmc_host_t host    = SDMMC_HOST_DEFAULT();
    host.slot            = SDMMC_HOST_SLOT_0;
    host.max_freq_khz    = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width               = 4;
    slot.clk                 = GPIO_NUM_43;
    slot.cmd                 = GPIO_NUM_44;
    slot.d0                  = GPIO_NUM_39;
    slot.d1                  = GPIO_NUM_40;
    slot.d2                  = GPIO_NUM_41;
    slot.d3                  = GPIO_NUM_42;

    esp_vfs_fat_mount_config_t cfg = s_cfg;
    cfg.format_if_mount_failed     = format_if_needed;
    esp_err_t err                  = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &s_card);
    s_mounted                      = err == ESP_OK;
    if (s_mounted) {
        // The panel's own folder (custom fonts); mkdir is a no-op when it exists
        mkdir(SYSTEM_DIR, 0777);
        mkdir(FONTS_DIR, 0777);
        ESP_LOGI(TAG, "card %s, %llu MB", s_card->cid.name,
                 (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no usable card (%s)", esp_err_to_name(err));
    }
    return err;
}

void sdcard_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mount(false);
    xSemaphoreGive(s_lock);
}

SdInfo sdcard_info(void)
{
    SdInfo info = {};
    // While formatting, the card is busy: report it as not mounted instead of waiting
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) return info;
    info.mounted = s_mounted;
    if (s_mounted) {
        strlcpy(info.name, s_card->cid.name, sizeof(info.name));
        esp_vfs_fat_info(MOUNT, &info.total, &info.free);
    }
    xSemaphoreGive(s_lock);
    return info;
}

static volatile bool s_formatting;
static volatile SdFormatResult s_result = SD_FORMAT_NONE;

bool sdcard_ready(void)
{
    return s_mounted && !s_formatting;
}

static void format_task(void* arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    if (s_mounted) {
        err = esp_vfs_fat_sdcard_format_cfg(MOUNT, s_card, &s_cfg);
    } else {
        // No readable file system (or the card was inserted later): mounting with format_if_mount_failed
        // creates a fresh FAT; a card that mounts fine this time gets formatted explicitly
        err = mount(true);
        if (err == ESP_OK) err = esp_vfs_fat_sdcard_format_cfg(MOUNT, s_card, &s_cfg);
    }
    if (err == ESP_OK) {
        mkdir(SYSTEM_DIR, 0777);
        mkdir(FONTS_DIR, 0777);
    }
    ESP_LOGI(TAG, "format: %s", esp_err_to_name(err));
    s_result     = err == ESP_OK ? SD_FORMAT_OK : SD_FORMAT_FAILED;
    s_formatting = false;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

bool sdcard_format_start(void)
{
    if (!s_lock || s_formatting) return false;
    s_formatting = true;
    s_result     = SD_FORMAT_NONE;
    if (xTaskCreate(format_task, "sd_format", 8192, NULL, 2, NULL) != pdPASS) {
        s_formatting = false;
        return false;
    }
    return true;
}

bool sdcard_formatting(void)
{
    return s_formatting;
}

SdFormatResult sdcard_format_result(void)
{
    return s_result;
}
