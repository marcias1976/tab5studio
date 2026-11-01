#pragma once

// Starts the web page (http://tab5.local) for controlling the panel from a phone or computer.
// Call after wifi_start.
void web_start(void);

#include <esp_http_server.h>

// Registers an API handler that only runs with the right PIN (X-PIN header, "pin" cookie or ?pin=)
void web_register_api(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*));
