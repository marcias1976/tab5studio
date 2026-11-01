/*
 * Studio designs on the Tab5: a live preview, and a design saved for good.
 *
 * Live preview: the Studio server sends a scene (images, fonts, elements) and the Tab5 shows it until it is stopped
 * or the device restarts.
 *   POST /api/scene/begin                       clears the previous live scene
 *   POST /api/scene/blob?name=img0&kind=img&w=..&h=..&alpha=1   raw RGB565(+A8) image
 *   POST /api/scene/blob?name=f_x&kind=font     TTF font
 *   POST /api/scene/show                        JSON with the two faces (and "third": Wariant-3); shows them
 *   POST /api/scene/stop                        back to the saved design (or the compiled look)
 *
 * Saved design: the scene on screen is written to the internal "scene" partition (LittleFS at /scene) and loaded
 * again at every start, so the look changes without building and flashing a program.
 *   POST /api/scene/save   {"name":"..."}   saves the scene shown now
 *   POST /api/scene/forget                  deletes the saved design, back to the compiled look
 * Files: /scene/cur/scene.json (name, time, blob list and the "show" JSON) plus b0, b1, ... (the blobs). A save is
 * written to /scene/new and swapped in at the end, so an interrupted save keeps the previous design.
 * A design that makes the Tab5 crash 3 starts in a row is skipped (safe mode) until it is saved again or deleted.
 */
#include <dirent.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <bsp/m5stack_tab5.h>
#include "alarm.h"
#include "layout.h"
#include "livescene.h"
#include "scene.h"
#include "ui.h"
#include "web.h"

static const char* TAG = "livescene";

struct Blob {
    char name[32];
    char kind[8];  // "img" or "font"
    int w, h;      // images only
    bool alpha;
    uint8_t* data;
    size_t size;
    lv_image_dsc_t* img;  // images only
};
static const int MAX_BLOBS = 40;
static Blob* s_blobs;  // [MAX_BLOBS] in PSRAM, made on first use
static SceneEl* s_els[6];  // the two faces, the intro, the alarm and timer screens, Wariant-3
static char* s_strings;  // all font names and texts of the scene
static size_t s_strings_used;
static const size_t STRINGS_SIZE = 16384;
static char* s_show;  // the "show" JSON of the scene on screen (PSRAM), kept for /api/scene/save

// What is on screen: nothing from Studio, the saved design, or a preview
enum Mode { MODE_NONE, MODE_SAVED, MODE_PREVIEW };
static Mode s_mode;

// A design is saved (NVS "livescene"/"loc" = 1)
#define ROOT "/scene"
static bool s_saved;
static char s_saved_name[48];
static int64_t s_saved_time;
static size_t s_saved_size;
static const char* s_saved_err = "";  // "safe_mode", "load_failed"
static bool s_int_mounted;
static int64_t s_int_free = -1;  // free bytes in the scene partition, updated after mount, save and delete (slow to read)
static SemaphoreHandle_t s_mutex;  // web handlers and the loaders at boot

static void lock()
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock()
{
    xSemaphoreGive(s_mutex);
}

static const char* keep_string(const char* s)
{
    size_t n = strlen(s) + 1;
    if (!s_strings) s_strings = (char*)heap_caps_malloc(STRINGS_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_strings || s_strings_used + n > STRINGS_SIZE) return "";
    char* p = s_strings + s_strings_used;
    memcpy(p, s, n);
    s_strings_used += n;
    return p;
}

// Stops the live scene and frees everything it used (display lock held)
static void clear_scene()
{
    layout_live_fonts_retire();    // the views rebuilt below must not pick up the live fonts
    ui_scene_live_stop();          // labels and images of the scene are gone after this
    ui_scene_live_intro(NULL);
    ui_scene_live_screens(NULL, NULL);
    ui_scene_live_third(NULL);
    layout_live_fonts_clear();
    if (!s_blobs) s_blobs = (Blob*)heap_caps_calloc(MAX_BLOBS, sizeof(Blob), MALLOC_CAP_SPIRAM);
    for (Blob* b = s_blobs; b && b < s_blobs + MAX_BLOBS; b++) {
        if (b->img) {
            lv_image_cache_drop(b->img);
            free(b->img);
        }
        free(b->data);
        *b = {};
    }
    for (SceneEl*& e : s_els) {
        free(e);
        e = NULL;
    }
    s_strings_used = 0;
    free(s_show);
    s_show = NULL;
    s_mode = MODE_NONE;
}

// Takes over data (PSRAM) on success
static bool add_blob(const char* name, const char* kind, int w, int h, bool alpha, uint8_t* data, size_t len)
{
    if (!s_blobs) s_blobs = (Blob*)heap_caps_calloc(MAX_BLOBS, sizeof(Blob), MALLOC_CAP_SPIRAM);
    Blob* slot = NULL;
    for (Blob* b = s_blobs; b && b < s_blobs + MAX_BLOBS; b++)
        if (!b->data) {
            slot = b;
            break;
        }
    if (!slot) return false;
    strlcpy(slot->name, name, sizeof(slot->name));
    strlcpy(slot->kind, kind, sizeof(slot->kind));
    slot->w = w, slot->h = h, slot->alpha = alpha;
    slot->data = data;
    slot->size = len;
    if (strcmp(kind, "img") == 0) {
        lv_image_dsc_t* d = (lv_image_dsc_t*)calloc(1, sizeof(lv_image_dsc_t));
        // bounded sizes, 64-bit maths: a crafted w × h must not wrap around and pass with a small body
        if (d && w > 0 && h > 0 && w <= 4096 && h <= 4096 && (uint64_t)len >= (uint64_t)w * h * (alpha ? 3 : 2)) {
            d->header.magic  = LV_IMAGE_HEADER_MAGIC;
            d->header.cf     = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
            d->header.w      = w;
            d->header.h      = h;
            d->header.stride = w * 2;
            d->data_size     = len;
            d->data          = data;
            slot->img        = d;
        } else {
            free(d);
        }
    } else {
        layout_live_font_add(name, data, len);
    }
    return true;
}

static esp_err_t ok(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t fail(httpd_req_t* req, const char* msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, msg);
}

static int query_int(const char* q, const char* key)
{
    char v[16];
    return httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK ? atoi(v) : 0;
}

static const lv_image_dsc_t* find_image(const char* name)
{
    for (const Blob* b = s_blobs; b && b < s_blobs + MAX_BLOBS; b++)
        if (b->img && strcmp(b->name, name) == 0) return b->img;
    return NULL;
}

static int geti(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
}

static uint32_t getu(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

static const char* gets(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// How a scene is put on screen: before ui_create() at boot, later at boot or after "stop" (no intro or screen
// preview), or as a preview from Studio (may play the intro or open a screen)
enum Phase { PH_EARLY, PH_QUIET, PH_PREVIEW };

// Builds the element arrays from a "show" JSON and shows them (display lock held). The blobs must be added first.
static bool apply_show(const cJSON* root, Phase ph)
{
    const cJSON* faces = cJSON_GetObjectItem(root, "faces");
    if (!cJSON_IsArray(faces) || cJSON_GetArraySize(faces) != 2) return false;
    ui_scene_live_stop();  // the old element arrays may still be on screen
    ui_scene_live_intro(NULL);
    ui_scene_live_screens(NULL, NULL);
    ui_scene_live_third(NULL);
    SceneFace out[6] = {};
    static const char* const extra[] = {"intro", "alarm", "timer", "third"};
    for (int f = 0; f < 6; f++) {
        const cJSON* face = f < 2 ? cJSON_GetArrayItem(faces, f) : cJSON_GetObjectItem(root, extra[f - 2]);
        if (!face || (f == 5 && !cJSON_GetObjectItem(face, "els"))) continue;  // Wariant-3 without a design
        const cJSON* els  = cJSON_GetObjectItem(face, "els");
        int n             = cJSON_IsArray(els) ? cJSON_GetArraySize(els) : 0;
        if (n > 64) n = 64;
        free(s_els[f]);
        s_els[f] = (SceneEl*)heap_caps_calloc(n ? n : 1, sizeof(SceneEl), MALLOC_CAP_SPIRAM);
        for (int i = 0; i < n && s_els[f]; i++) {
            const cJSON* e = cJSON_GetArrayItem(els, i);
            SceneEl& el    = s_els[f][i];
            el.type        = geti(e, "t");
            el.opa         = geti(e, "o");
            el.flags       = geti(e, "f");
            el.source      = geti(e, "s");
            el.x = geti(e, "x"), el.y = geti(e, "y"), el.w = geti(e, "w"), el.h = geti(e, "h");
            el.size = geti(e, "z"), el.size2 = geti(e, "z2");
            el.color = getu(e, "c"), el.color2 = getu(e, "c2"), el.color3 = getu(e, "c3"), el.color4 = getu(e, "c4");
            el.action = geti(e, "act"), el.action_val = geti(e, "av");
            el.decimals = geti(e, "d"), el.align = geti(e, "a"), el.icon = geti(e, "i");
            el.font     = keep_string(gets(e, "font"));
            el.text     = keep_string(gets(e, "text"));
            el.img      = el.type == SC_IMAGE ? find_image(gets(e, "img")) : NULL;
            if (el.type == SC_IMAGE && !el.img) el.type = SC_RECT;  // missing picture: an empty box
        }
        out[f] = {(uint32_t)strtoul(gets(face, "bg"), NULL, 16), s_els[f], s_els[f] ? n : 0, geti(face, "ms")};
    }
    bool play_intro = ph == PH_PREVIEW && cJSON_IsTrue(cJSON_GetObjectItem(root, "play_intro"));
    const cJSON* sv = cJSON_GetObjectItem(root, "show_screen");
    int show_screen = ph != PH_PREVIEW || !cJSON_IsString(sv) ? 0 : !strcmp(sv->valuestring, "alarm") ? 1 : !strcmp(sv->valuestring, "timer") ? 2 : 0;
    if (ph == PH_EARLY) ui_scene_live_preset(out);  // ui_create() builds the faces from it
    else ui_scene_live(out);
    if (out[2].els) ui_scene_live_intro(&out[2]);  // before ui_create(): the start plays this intro
    ui_scene_live_screens(out[3].els ? &out[3] : NULL, out[4].els ? &out[4] : NULL);
    ui_scene_live_third(out[5].els ? &out[5] : NULL);
    if (play_intro) ui_play_intro();
    if (show_screen) ui_preview_screen(show_screen);
    ESP_LOGI(TAG, "scene: %d + %d elements", out[0].count, out[1].count);
    return true;
}

/* ------------------------------ Saved design ------------------------------ */

static bool mount_int()
{
    if (s_int_mounted) return true;
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = ROOT;
    conf.partition_label        = "scene";
    conf.format_if_mount_failed = true;
    esp_err_t err               = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) ESP_LOGE(TAG, "scene partition: %s", esp_err_to_name(err));
    s_int_mounted = err == ESP_OK;
    return s_int_mounted;
}

static void update_int_free()
{
    size_t total = 0, used = 0;
    s_int_free = s_int_mounted && esp_littlefs_info("scene", &total, &used) == ESP_OK ? (int64_t)total - (int64_t)used : -1;
}

// Free bytes for a design, counting the space of the design saved now (a save replaces it); -1 = no storage
static int64_t room()
{
    if (!s_int_mounted) return -1;
    if (s_int_free < 0) update_int_free();
    if (s_int_free < 0) return -1;
    int64_t r = s_int_free - 64 * 1024 + (s_saved ? s_saved_size : 0);  // LittleFS needs a few spare blocks
    return r < 0 ? 0 : r;
}

static void nvs_put(const char* key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("livescene", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t nvs_get(const char* key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("livescene", NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v;
}

// Deletes the files of a folder and the folder (the design folders hold files only)
static void rm_dir(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return;
    char p[300];
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void rm_design()
{
    if (!mount_int()) return;
    for (const char* sub : {ROOT "/new", ROOT "/cur", ROOT "/old"}) rm_dir(sub);
}

static bool exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool write_file(const char* path, const void* data, size_t len)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool good = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && good;
}

// Writes the scene on screen to the scene partition. Returns NULL or an error code for Studio.
static const char* save_design(const char* name)
{
    if (!s_show || s_mode == MODE_NONE) return "nothing";
    if (!mount_int()) return "storage";
    update_int_free();
    size_t total = strlen(s_show) + 1024;
    for (const Blob* b = s_blobs; b && b < s_blobs + MAX_BLOBS; b++) total += b->data ? b->size : 0;
    int64_t free_room = room();
    if (free_room < (int64_t)total) return "no_space";

    const char* dir = ROOT "/new";
    char p[128];
    rm_dir(dir);
    // Not enough room for the new design next to the old one: the old one goes first
    if (s_saved && free_room - (int64_t)s_saved_size < (int64_t)total) {
        nvs_put("loc", 0);
        rm_design();
        s_saved = false;
    }
    if (mkdir(dir, 0777) != 0 && !exists(dir)) return "storage";

    cJSON* man = cJSON_CreateObject();
    cJSON_AddNumberToObject(man, "v", 1);
    cJSON_AddStringToObject(man, "name", name);
    cJSON_AddNumberToObject(man, "time", (double)time(NULL));
    cJSON* list = cJSON_AddArrayToObject(man, "blobs");
    size_t size = strlen(s_show);
    int n       = 0;
    bool good   = true;
    for (const Blob* b = s_blobs; good && b && b < s_blobs + MAX_BLOBS; b++) {
        if (!b->data) continue;
        snprintf(p, sizeof(p), "%s/b%d", dir, n);
        good     = write_file(p, b->data, b->size);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", b->name);
        cJSON_AddStringToObject(o, "k", b->kind);
        cJSON_AddNumberToObject(o, "w", b->w);
        cJSON_AddNumberToObject(o, "h", b->h);
        cJSON_AddBoolToObject(o, "a", b->alpha);
        cJSON_AddNumberToObject(o, "f", n);
        cJSON_AddItemToArray(list, o);
        size += b->size;
        n++;
    }
    // scene.json last: a folder without it is an unfinished save
    char* head = cJSON_PrintUnformatted(man);
    cJSON_Delete(man);
    if (good && head) {
        snprintf(p, sizeof(p), "%s/scene.json", dir);
        FILE* f = fopen(p, "wb");
        good    = f != NULL;
        if (f) {
            size_t hl = strlen(head) - 1;  // without the closing brace
            good      = fwrite(head, 1, hl, f) == hl && fputs(",\"show\":", f) >= 0 && fputs(s_show, f) >= 0 && fputs("}", f) >= 0;
            good      = fclose(f) == 0 && good;
        }
    } else {
        good = false;
    }
    free(head);
    if (!good) {
        rm_dir(dir);
        update_int_free();
        return "write";
    }
    // Swap it in: cur → old, new → cur, then the old one goes
    const char *cur = ROOT "/cur", *old = ROOT "/old";
    rm_dir(old);
    if (exists(cur)) rename(cur, old);
    if (rename(dir, cur) != 0) return "write";
    rm_dir(old);
    update_int_free();
    nvs_put("loc", 1);
    nvs_put("crash", 0);
    s_saved = true;
    strlcpy(s_saved_name, name, sizeof(s_saved_name));
    s_saved_time = time(NULL);
    s_saved_size = size;
    s_saved_err  = "";
    s_mode       = MODE_SAVED;
    ESP_LOGI(TAG, "design \"%s\" saved (%u bytes)", name, (unsigned)size);
    return NULL;
}

static char* read_file(const char* path, size_t* len, size_t max)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n >= 0 && (size_t)n <= max ? (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        *len   = n;
    }
    return buf;
}

// Loads the saved design and shows it (display lock held)
static bool load_design(Phase ph)
{
    if (!s_saved) return false;
    if (!mount_int()) {
        s_saved_err = "load_failed";
        return false;
    }
    const char* dir = ROOT "/cur";
    char p[128];
    size_t len = 0;
    char* text = NULL;
    for (const char* sub : {ROOT "/cur", ROOT "/old"}) {  // "old" only after a save cut off in the middle of the swap
        dir = sub;
        snprintf(p, sizeof(p), "%s/scene.json", dir);
        if ((text = read_file(p, &len, 256 * 1024))) break;
    }
    cJSON* man = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON* show = cJSON_GetObjectItem(man, "show");
    if (!show) {
        cJSON_Delete(man);
        s_saved_err = "load_failed";
        return false;
    }
    strlcpy(s_saved_name, gets(man, "name"), sizeof(s_saved_name));
    const cJSON* t = cJSON_GetObjectItem(man, "time");
    s_saved_time   = cJSON_IsNumber(t) ? (int64_t)t->valuedouble : 0;
    size_t size    = len;
    bool good      = true;
    const cJSON* b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItem(man, "blobs"))
    {
        snprintf(p, sizeof(p), "%s/b%d", dir, geti(b, "f"));
        size_t n      = 0;
        uint8_t* data = (uint8_t*)read_file(p, &n, 4 * 1024 * 1024);
        if (!data || !add_blob(gets(b, "n"), gets(b, "k"), geti(b, "w"), geti(b, "h"), cJSON_IsTrue(cJSON_GetObjectItem(b, "a")), data, n)) {
            free(data);
            good = false;
            break;
        }
        size += n;
    }
    char* printed = good ? cJSON_PrintUnformatted(show) : NULL;
    if (printed) {
        s_show = (char*)heap_caps_malloc(strlen(printed) + 1, MALLOC_CAP_SPIRAM);
        if (s_show) strcpy(s_show, printed);
        free(printed);
    }
    good = good && s_show && apply_show(show, ph);
    cJSON_Delete(man);
    if (!good) {
        s_saved_err = "load_failed";
        return false;
    }
    s_saved_size = size;
    s_saved_err  = "";
    s_mode       = MODE_SAVED;
    return true;
}

static void on_boot_ok(void* arg)
{
    (void)arg;
    nvs_put("crash", 0);  // the design ran long enough: it does not crash the Tab5
}

void livescene_boot_early(bool froze)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (mount_int()) update_int_free();  // ready for /api/state (and formatted on the first start)
    s_saved = nvs_get("loc") == 1;
    if (!s_saved) return;
    // Safe mode: the last starts with this design ended in a crash or a frozen screen
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed         = froze || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    uint8_t crashes      = crashed ? nvs_get("crash") + 1 : 0;
    nvs_put("crash", crashes);
    if (crashes >= 3) {
        s_saved_err = "safe_mode";
        ESP_LOGW(TAG, "saved design skipped after %d crashed starts", crashes);
        return;
    }
    static esp_timer_handle_t timer;
    esp_timer_create_args_t args = {};
    args.callback                = on_boot_ok;
    args.name                    = "scene_ok";
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 30 * 1000000);
    lock();
    load_design(PH_EARLY);
    unlock();
}

void livescene_state(cJSON* root)
{
    // Never wait long here: /api/state must answer while a save is running
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        cJSON_AddBoolToObject(root, "scene_busy", true);
        return;
    }
    cJSON_AddBoolToObject(root, "scene_live", s_mode == MODE_PREVIEW);
    if (s_saved) {
        cJSON* o = cJSON_AddObjectToObject(root, "scene_saved");
        cJSON_AddStringToObject(o, "name", s_saved_name);
        cJSON_AddNumberToObject(o, "kb", (double)(s_saved_size / 1024));
        cJSON_AddNumberToObject(o, "time", (double)s_saved_time);
        cJSON_AddBoolToObject(o, "shown", s_mode == MODE_SAVED);
    } else {
        cJSON_AddNullToObject(root, "scene_saved");
    }
    cJSON_AddStringToObject(root, "scene_saved_err", s_saved_err);
    int64_t r = room();
    cJSON_AddNumberToObject(root, "scene_room_kb", r < 0 ? -1 : (double)(r / 1024));
    unlock();
}

/* -------------------------------- Handlers -------------------------------- */

static esp_err_t begin_post(httpd_req_t* req)
{
    lock();
    if (!bsp_display_lock(3000)) {
        unlock();
        return fail(req, "busy");
    }
    clear_scene();
    bsp_display_unlock();
    unlock();
    return ok(req);
}

static esp_err_t stop_post(httpd_req_t* req)
{
    lock();
    if (!bsp_display_lock(3000)) {
        unlock();
        return fail(req, "busy");
    }
    clear_scene();
    if (strcmp(s_saved_err, "safe_mode") != 0) load_design(PH_QUIET);  // back to the saved design, if there is one
    if (!alarm_ringing()) ui_alarm_hide();  // previews of the alarm and timer screens close too
    ui_timer(-1, 0, false);
    bsp_display_unlock();
    unlock();
    return ok(req);
}

static esp_err_t blob_post(httpd_req_t* req)
{
    char q[160], name[32], kind[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
        httpd_query_key_value(q, "kind", kind, sizeof(kind)) != ESP_OK)
        return fail(req, "name/kind");
    size_t len = req->content_len;
    if (len == 0 || len > 4 * 1024 * 1024) return fail(req, "size");
    uint8_t* data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!data) return fail(req, "no memory");
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char*)data + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(data);
            return fail(req, "receive");
        }
        got += n;
    }
    lock();
    if (!bsp_display_lock(3000)) {
        unlock();
        free(data);
        return fail(req, "busy");
    }
    bool added = add_blob(name, strcmp(kind, "img") == 0 ? "img" : "font", query_int(q, "w"), query_int(q, "h"), query_int(q, "alpha"), data, len);
    bsp_display_unlock();
    unlock();
    if (!added) {
        free(data);
        return fail(req, "too many files");
    }
    return ok(req);
}

static char* recv_body(httpd_req_t* req, size_t max)
{
    size_t len = req->content_len;
    if (len == 0 || len > max) return NULL;
    char* body = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return NULL;
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';
    return body;
}

static esp_err_t show_post(httpd_req_t* req)
{
    char* body = recv_body(req, 64 * 1024);
    if (!body) return fail(req, "size");
    cJSON* root = cJSON_Parse(body);
    if (!cJSON_IsArray(cJSON_GetObjectItem(root, "faces"))) {
        cJSON_Delete(root);
        free(body);
        return fail(req, "faces");
    }
    lock();
    if (!bsp_display_lock(3000)) {
        unlock();
        cJSON_Delete(root);
        free(body);
        return fail(req, "busy");
    }
    bool shown = apply_show(root, PH_PREVIEW);
    cJSON_Delete(root);  // (apply_show copied what it needs)
    free(s_show);
    s_show = shown ? body : NULL;  // kept for /api/scene/save
    if (!shown) free(body);
    if (shown) s_mode = MODE_PREVIEW;
    bsp_display_unlock();
    unlock();
    return shown ? ok(req) : fail(req, "faces");
}

static esp_err_t save_post(httpd_req_t* req)
{
    char* body = recv_body(req, 1024);
    cJSON* root = body ? cJSON_Parse(body) : NULL;
    free(body);
    const char* name = gets(root, "name");
    lock();
    const char* err = save_design(name[0] ? name : "Studio");
    unsigned kb     = s_saved_size / 1024;
    unlock();
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    char msg[48];
    if (err) {
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(msg, sizeof(msg), "{\"error\":\"%s\"}", err);
    } else {
        snprintf(msg, sizeof(msg), "{\"kb\":%u}", kb);
    }
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t forget_post(httpd_req_t* req)
{
    lock();
    rm_design();
    update_int_free();
    nvs_put("loc", 0);
    nvs_put("crash", 0);
    s_saved         = false;
    s_saved_err     = "";
    s_saved_name[0] = '\0';
    s_saved_size    = 0;
    if (s_mode == MODE_SAVED && bsp_display_lock(3000)) {  // the saved design is on screen: back to the compiled look
        clear_scene();
        bsp_display_unlock();
    }
    unlock();
    return ok(req);
}

void livescene_register(httpd_handle_t server)
{
    web_register_api(server, "/api/scene/begin", HTTP_POST, begin_post);
    web_register_api(server, "/api/scene/blob", HTTP_POST, blob_post);
    web_register_api(server, "/api/scene/show", HTTP_POST, show_post);
    web_register_api(server, "/api/scene/stop", HTTP_POST, stop_post);
    web_register_api(server, "/api/scene/save", HTTP_POST, save_post);
    web_register_api(server, "/api/scene/forget", HTTP_POST, forget_post);
}
