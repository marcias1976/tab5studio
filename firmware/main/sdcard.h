#pragma once

#include <stdint.h>
#include <esp_err.h>

// microSD card, mounted as FAT at /sdcard
#define SDCARD_MOUNT "/sdcard"

struct SdInfo {
    bool mounted;
    char name[8];     // card model from its ID
    uint64_t total;   // bytes
    uint64_t free;
};

// Mounts the card if one with a readable file system is inserted; call after wifi_start
void sdcard_start(void);
SdInfo sdcard_info(void);

// Card mounted and not being formatted
bool sdcard_ready(void);

// Erases the card and creates an empty FAT file system (also on a card that did not mount).
// Runs in the background (about a minute for 32 GB); sdcard_formatting() is true until it ends.
enum SdFormatResult { SD_FORMAT_NONE, SD_FORMAT_OK, SD_FORMAT_FAILED };
bool sdcard_format_start(void);
bool sdcard_formatting(void);
SdFormatResult sdcard_format_result(void);
