#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bsp/m5stack_tab5.h>
#include "audio.h"

static const char* TAG = "audio";

static TaskHandle_t s_task;
static char s_path[128];
static int s_volume = 80, s_fade = 0;
static bool s_loop;
static volatile bool s_stop, s_busy;
static volatile uint32_t s_request;  // increments with every audio_play()

static esp_codec_dev_handle_t s_spk;  // opened in audio_start(), never closed, 16 kHz stereo
static const int s_rate = 16000, s_channels = 2;

// Fade-in: gain from 0.05 to 1 over s_fade seconds of sound
static uint32_t s_played;  // samples per channel since the start
static void write_pcm(int16_t* pcm, int frames)
{
    if (s_fade > 0) {
        uint32_t total = (uint32_t)s_fade * s_rate;
        for (int i = 0; i < frames; i++) {
            float g = s_played + i >= total ? 1.0f : 0.05f + 0.95f * (float)(s_played + i) / total;
            for (int c = 0; c < s_channels; c++) pcm[i * s_channels + c] = (int16_t)(pcm[i * s_channels + c] * g);
        }
    }
    s_played += frames;
    esp_codec_dev_write(s_spk, pcm, frames * s_channels * sizeof(int16_t));
}

// "Beep-beep … beep-beep": two short 880 Hz tones, then a pause; about one second per round
static void play_beep()
{
    static const int RATE = 16000, N = 320;  // 20 ms blocks
    int16_t* buf = (int16_t*)heap_caps_malloc(N * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return;
    float phase = 0;
    do {
        for (int block = 0; block < 50 && !s_stop; block++) {  // 50 × 20 ms = 1 s
            bool on = block < 8 || (block >= 13 && block < 21);
            for (int i = 0; i < N; i++) {
                int16_t v = 0;
                if (on) {
                    v = (int16_t)(sinf(phase) * 12000);
                    phase += 2 * (float)M_PI * 880 / RATE;
                    if (phase > 2 * (float)M_PI) phase -= 2 * (float)M_PI;
                }
                buf[i * 2] = buf[i * 2 + 1] = v;
            }
            write_pcm(buf, N);
        }
    } while (s_loop && !s_stop);
    free(buf);
}

static void audio_task(void* arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t req = s_request;
        s_busy       = true;
        if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_volume);
        while (s_spk && !s_stop) {
            s_played = 0;
            play_beep();
            if (s_request == req) break;  // finished (or stopped); a newer request plays next round
            req    = s_request;
            s_stop = false;
            esp_codec_dev_set_out_vol(s_spk, s_volume);
        }
        if (s_spk) esp_codec_dev_set_out_vol(s_spk, 0);
        s_busy = false;
    }
}

// The task (6 KB of internal RAM for its stack) is made at boot: made at the first alarm after hours of running,
// it could fail for lack of internal RAM, and the alarm crashed the Tab5
// The speaker opens at boot and stays open: opening the audio interface later needs DMA memory that may be gone
// by then (that crashed the alarm)
static void open_speaker()
{
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return;
    }
    esp_codec_dev_sample_info_t out = {};
    out.sample_rate     = s_rate;
    out.channel         = s_channels;
    out.bits_per_sample = 16;
    if (esp_codec_dev_open(s_spk, &out) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        s_spk = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, 0);
}

void audio_start(void)
{
    if (!s_spk) open_speaker();
    if (!s_task && xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 4, &s_task, 1) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no memory for the sound task");
    }
}

void audio_play(const char* path, int volume, bool loop, int fade_s)
{
    audio_start();
    if (!s_task) return;  // no sound rather than a crash
    s_stop = s_busy;  // cuts the sound now playing; the task then starts this one
    strlcpy(s_path, path ? path : "", sizeof(s_path));
    s_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    s_loop   = loop;
    s_fade   = fade_s;
    s_request++;
    if (!s_busy) s_stop = false;
    xTaskNotifyGive(s_task);
}

void audio_stop(void)
{
    s_loop = false;
    s_stop = true;
}

bool audio_playing(void)
{
    return s_busy;
}
