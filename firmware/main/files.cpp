/*
 * File manager API for the microSD card (used by the web page):
 *   GET  /api/files?path=/dir        directory listing (JSON)
 *   GET  /api/file?path=/dir/x.jpg   download
 *   POST /api/upload?path=/dir/x.jpg raw file body, overwrites
 *   POST /api/mkdir?path=/dir/new
 *   POST /api/delete?path=/dir/x     file, or folder with everything inside
 * Paths are relative to the card; ".." is rejected.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include "files.h"
#include "lang.h"
#include "sdcard.h"
#include "web.h"

static const char* TAG = "files";
static const size_t CHUNK = 8192;

/* -------------------------------- Helpers -------------------------------- */

// Decodes %XX and '+' in place
static void url_decode(char* s)
{
    char* out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *out++      = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

// The "path" query parameter as a full path under /sdcard; false when missing or unsafe
static bool full_path(httpd_req_t* req, char* out, size_t size)
{
    char query[600], rel[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, "path", rel, sizeof(rel)) != ESP_OK) return false;
    url_decode(rel);
    if (strstr(rel, "..") || strchr(rel, '\\')) return false;
    while (rel[0] == '/') memmove(rel, rel + 1, strlen(rel));
    size_t n = strlen(rel);
    while (n && rel[n - 1] == '/') rel[--n] = '\0';  // no trailing slash
    snprintf(out, size, "%s%s%s", SDCARD_MOUNT, rel[0] ? "/" : "", rel);
    return true;
}

static esp_err_t fail(httpd_req_t* req, const char* status, const char* msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

static bool check_card(httpd_req_t* req)
{
    if (sdcard_ready()) return true;
    fail(req, "503 Service Unavailable", "Brak karty SD");
    return false;
}

static esp_err_t send_ok(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

/* -------------------------------- Handlers -------------------------------- */

static esp_err_t list_get(httpd_req_t* req)
{
    char path[540];
    if (!full_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", TR("Zła ścieżka", "Bad path"));
    if (!check_card(req)) return ESP_OK;
    DIR* dir = opendir(path);
    if (!dir) return fail(req, "404 Not Found", "Nie ma takiego folderu");

    cJSON* root  = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    char child[800];
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", e->d_name);
        cJSON_AddBoolToObject(it, "dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(it, "size", (double)st.st_size);
        cJSON_AddNumberToObject(it, "mtime", (double)st.st_mtime);
        cJSON_AddItemToArray(items, it);
    }
    closedir(dir);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{\"items\":[]}");
    cJSON_free(text);
    return err;
}

static const char* mime(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    static const char* types[][2] = {{".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
                                     {".gif", "image/gif"},  {".bmp", "image/bmp"},   {".txt", "text/plain; charset=utf-8"},
                                     {".csv", "text/csv; charset=utf-8"}, {".json", "application/json"},
                                     {".pdf", "application/pdf"}, {".mp3", "audio/mpeg"}, {".wav", "audio/wav"}};
    for (auto& t : types)
        if (strcasecmp(dot, t[0]) == 0) return t[1];
    return "application/octet-stream";
}

static esp_err_t file_get(httpd_req_t* req)
{
    char path[540];
    if (!full_path(req, path, sizeof(path))) return fail(req, "400 Bad Request", TR("Zła ścieżka", "Bad path"));
    if (!check_card(req)) return ESP_OK;
    FILE* f = fopen(path, "rb");
    if (!f) return fail(req, "404 Not Found", "Nie ma takiego pliku");
    char* buf = (char*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return fail(req, "500 Internal Server Error", TR("Brak pamięci", "Out of memory"));
    }
    const char* name = strrchr(path, '/') + 1;
    httpd_resp_set_type(req, mime(name));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    free(buf);
    return ESP_OK;
}

static esp_err_t upload_post(httpd_req_t* req)
{
    char path[540];
    if (!full_path(req, path, sizeof(path)) || strcmp(path, SDCARD_MOUNT) == 0)
        return fail(req, "400 Bad Request", TR("Zła ścieżka", "Bad path"));
    if (!check_card(req)) return ESP_OK;
    FILE* f = fopen(path, "wb");
    if (!f) return fail(req, "500 Internal Server Error", TR("Nie można zapisać pliku", "Cannot write the file"));
    char* buf = (char*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        unlink(path);
        return fail(req, "500 Internal Server Error", TR("Brak pamięci", "Out of memory"));
    }
    size_t left = req->content_len;
    bool ok     = true;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) {
            ok = false;
            break;
        }
        left -= n;
    }
    fclose(f);
    free(buf);
    if (!ok) {
        unlink(path);  // no half-written files
        ESP_LOGW(TAG, "upload failed: %s", path);
        return fail(req, "500 Internal Server Error", TR("Przesyłanie przerwane", "Upload interrupted"));
    }
    return send_ok(req);
}

static esp_err_t mkdir_post(httpd_req_t* req)
{
    char path[540];
    if (!full_path(req, path, sizeof(path)) || strcmp(path, SDCARD_MOUNT) == 0)
        return fail(req, "400 Bad Request", TR("Zła ścieżka", "Bad path"));
    if (!check_card(req)) return ESP_OK;
    if (mkdir(path, 0777) != 0) return fail(req, "409 Conflict", TR("Nie można utworzyć folderu", "Cannot create the folder"));
    return send_ok(req);
}

// Deletes a file, or a folder with everything inside
static bool remove_all(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR* dir = opendir(path);
    if (!dir) return false;
    char* child = (char*)malloc(800);
    struct dirent* e;
    bool ok = child != NULL;
    while (ok && (e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(child, 800, "%s/%s", path, e->d_name);
        ok = remove_all(child);
    }
    closedir(dir);
    free(child);
    return ok && rmdir(path) == 0;
}

static esp_err_t delete_post(httpd_req_t* req)
{
    char path[540];
    if (!full_path(req, path, sizeof(path)) || strcmp(path, SDCARD_MOUNT) == 0)
        return fail(req, "400 Bad Request", TR("Zła ścieżka", "Bad path"));
    if (!check_card(req)) return ESP_OK;
    if (!remove_all(path)) return fail(req, "500 Internal Server Error", TR("Nie można usunąć", "Cannot delete"));
    return send_ok(req);
}

void files_register(httpd_handle_t server)
{
    web_register_api(server, "/api/files", HTTP_GET, list_get);
    web_register_api(server, "/api/file", HTTP_GET, file_get);
    web_register_api(server, "/api/upload", HTTP_POST, upload_post);
    web_register_api(server, "/api/mkdir", HTTP_POST, mkdir_post);
    web_register_api(server, "/api/delete", HTTP_POST, delete_post);
}
