#pragma once

#include <esp_http_server.h>

// Registers the SD card file manager API (/api/files, /api/file, /api/upload, /api/mkdir, /api/delete)
void files_register(httpd_handle_t server);
