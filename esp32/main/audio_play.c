/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "audio_play.h"
#include "sdkconfig.h"

#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "minimp3.h"

#include "muse_audio.h"
#include "muse_state.h"

static const char *TAG = "link.audio_play";

// The whole clip is buffered in PSRAM before it plays, so playback never
// stalls on the network. 1 MB of MP3 is several minutes of speech.
#define MP3_CAP_BYTES       (1024 * 1024)
// minimp3 wants ~16 KB of stack; the TLS handshake runs on the same task,
// as it does on the chat session's 48 KB PSRAM stack.
#define PLAY_STACK_BYTES    (48 * 1024)
// Per socket operation, and for the whole download: a server that trickles
// bytes never trips the first, and would otherwise hold the task, its
// buffers and the busy flag indefinitely. Long clips play after downloading,
// so the deadline covers the download only; playback can outlive it.
#define PLAY_TIMEOUT_MS     10000
#define PLAY_DEADLINE_MS    60000
#define PLAY_MAX_REDIRECTS  3
// Never start a download that would go below this much free internal RAM,
// and abort one that dips under the floor (HTTPS needs little here with
// mbedTLS in PSRAM, but the Noise session and lwIP still run).
#define PLAY_RESERVE_BYTES  (24 * 1024)
#define PLAY_FLOOR_BYTES    (16 * 1024)
// Decode output chunk fed to the speaker: 20 ms of 16 kHz mono.
#define OUT_CHUNK           (MUSE_AUDIO_RATE / 50)

#define BYTE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    char *url;
    audio_play_done_cb done;
    void *user;
    esp_http_client_handle_t http;
    int64_t deadline_us;
    uint8_t *mp3;               // PSRAM, MP3_CAP_BYTES
    size_t mp3_len;
    bool https;
    bool timed_out;
    bool low_memory;
    bool scheme_changed;
    size_t bytes;
} play_t;

static atomic_bool s_busy;

static size_t internal_free(void) {
    return heap_caps_get_free_size(BYTE_CAPS);
}

// Bound the next socket operation by the time left. False once it is up,
// when the next operation is left 1 ms so that a call in progress returns.
static bool within_deadline(play_t *p) {
    int64_t left_ms = (p->deadline_us - esp_timer_get_time()) / 1000;
    if (left_ms <= 0) {
        p->timed_out = true;
        esp_http_client_set_timeout_ms(p->http, 1);
        return false;
    }
    esp_http_client_set_timeout_ms(p->http,
                                   left_ms < PLAY_TIMEOUT_MS ? (int)left_ms : PLAY_TIMEOUT_MS);
    return true;
}

// Reads the whole body into the MP3 buffer, up to the cap, the deadline or a
// memory floor. Returns 0 on success (including a full buffer: the clip just
// plays truncated), -1 on a network error, low memory or timeout.
static int download_read(play_t *p) {
    while (p->mp3_len < MP3_CAP_BYTES) {
        if (internal_free() < PLAY_FLOOR_BYTES) {
            p->low_memory = true;
            return -1;
        }
        if (!within_deadline(p)) return -1;
        int n = esp_http_client_read(p->http, (char *)p->mp3 + p->mp3_len,
                                     (int)(MP3_CAP_BYTES - p->mp3_len));
        if (n < 0) return -1;
        if (n == 0) break;    // body complete
        p->mp3_len += (size_t)n;
        p->bytes += (size_t)n;
    }
    return 0;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    play_t *p = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA || evt->event_id == HTTP_EVENT_ON_HEADER) {
        within_deadline(p);
    }
    return ESP_OK;
}

// Does the client's current URL still use the scheme the fetch was sized for?
static bool same_scheme(play_t *p) {
    char url[16];
    if (esp_http_client_get_url(p->http, url, sizeof(url)) != ESP_OK) return false;
    return p->https ? strncasecmp(url, "https://", 8) == 0
                    : strncasecmp(url, "http://", 7) == 0;
}

static esp_err_t open_following_redirects(play_t *p, int *status) {
    for (int i = 0;; i++) {
        if (!within_deadline(p)) return ESP_ERR_TIMEOUT;
        esp_err_t err = esp_http_client_open(p->http, 0);
        if (err != ESP_OK) return err;
        esp_http_client_fetch_headers(p->http);
        *status = esp_http_client_get_status_code(p->http);
        bool redirect = *status == 301 || *status == 302 || *status == 303
                     || *status == 307 || *status == 308;
        if (!redirect || i == PLAY_MAX_REDIRECTS) return ESP_OK;
        err = esp_http_client_set_redirection(p->http);
        esp_http_client_close(p->http);
        if (err != ESP_OK) return err;
        if (!same_scheme(p)) {
            p->scheme_changed = true;
            return ESP_FAIL;
        }
    }
}

// ---- MP3 -> speaker ----------------------------------------------------------

typedef struct {
    uint32_t step;           // Q16 input samples per output sample
    uint32_t pos;
    int16_t prev;
} resampler_t;

static void resampler_init(resampler_t *r, int in_rate, int out_rate) {
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

// Linear interpolation; state carries across calls. out must hold n*out/in + 2.
// Same Q16 fixed-point resampler as the reply-speech path.
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out) {
    size_t o = 0;
    if (!n) return 0;
    /* Position 0 is the previous call's last sample, k is in[k-1]. */
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = a + (((b - a) * (int32_t)(r->pos & 0xffff)) >> 16);
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

/* Fails only on a bad or unsupported MP3. */
static const char *decode_and_play(play_t *p, double *seconds, bool *own_ui) {
    mp3dec_t *dec = heap_caps_malloc(sizeof(*dec), MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    // One decode frame resampled to 16 kHz fits with MINIMP3_MAX_SAMPLES_PER_FRAME
    // at the worst rate ratio (8 kHz -> 16 kHz doubles it).
    int16_t *out = heap_caps_malloc(2 * MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM);
    if (!dec || !pcm || !out) {
        free(dec);
        free(pcm);
        free(out);
        return "out of memory";
    }

    muse_audio_power(true);
    *own_ui = muse_state_mode(NULL) == MUSE_MODE_IDLE;
    if (*own_ui) {
        muse_state_set_mode(MUSE_MODE_SPEAKING);
        muse_state_set_caption("PLAYING AUDIO");
    }

    mp3dec_init(dec);
    resampler_t rs = { 0 };
    int rate = 0, frames = 0;
    size_t off = 0;
    size_t played = 0;
    const char *err = NULL;
    int64_t t0 = esp_timer_get_time();
    while (off < p->mp3_len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, p->mp3 + off, p->mp3_len - off, pcm, &info);
        if (!info.frame_bytes) {
            if (!frames) err = "not an MP3 file";
            break;    // trailing junk after the last frame is normal
        }
        off += info.frame_bytes;
        if (!samples) continue;
        frames++;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
            }
        }
        if (rate != info.hz) {
            rate = info.hz;
            resampler_init(&rs, info.hz, MUSE_AUDIO_RATE);
        }
        size_t n = resample(&rs, pcm, (size_t)samples, out);
        size_t at = 0;
        while (at < n) {
            size_t chunk = n - at < OUT_CHUNK ? n - at : OUT_CHUNK;
            if (*own_ui) muse_state_set_level(muse_audio_level(out + at, chunk));
            muse_audio_write(out + at, chunk);
            at += chunk;
        }
        played += n;
    }
    *seconds = played / (double)MUSE_AUDIO_RATE;
    ESP_LOGI(TAG, "played %.2fs from %u MP3 bytes, %d frames at %d Hz, in %lld ms",
             *seconds, (unsigned)p->mp3_len, frames, rate,
             (esp_timer_get_time() - t0) / 1000);
    if (!frames && !err) err = "no audio in the MP3";
    free(dec);
    free(pcm);
    free(out);
    return err;
}

static void play_task(void *arg) {
    play_t *p = arg;
    int64_t start = esp_timer_get_time();
    p->deadline_us = start + PLAY_DEADLINE_MS * 1000LL;
    audio_play_result_t result = { .ok = false, .code = "download_failed" };

    esp_http_client_config_t cfg = {
        .url = p->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = PLAY_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
        .event_handler = on_http_event,
        .user_data = p,
    };
    p->http = esp_http_client_init(&cfg);
    int status = 0;
    esp_err_t err = p->http ? open_following_redirects(p, &status) : ESP_ERR_NO_MEM;
    if (p->scheme_changed) {
        result.message = "redirect changed scheme";
    } else if (p->timed_out) {
        result.message = "download timed out";
    } else if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "could not connect: %s", esp_err_to_name(err));
        // The message outlives this scope only if copied; keep static codes
        // for the Muse and the detail in the log.
        ESP_LOGW(TAG, "audio download failed: %s", msg);
        result.code = "download_failed";
        result.message = "could not connect";
    } else if (status != 200) {
        ESP_LOGW(TAG, "audio download answered HTTP %d", status);
        result.code = "download_failed";
        result.message = "server did not answer 200";
    } else {
        if (download_read(p) < 0) {
            ESP_LOGW(TAG, "download broke off after %u bytes", (unsigned)p->mp3_len);
        }
        if (p->low_memory) {
            result.code = "out_of_memory";
            result.message = "memory ran low";
        } else if (p->mp3_len < 100) {
            // Too little to decode: say why.
            if (p->timed_out) result.message = "download timed out";
            else if (p->scheme_changed) result.message = "redirect changed scheme";
            else result.message = "download failed";
        } else {
            // Play what arrived, even if the download broke off.
            bool own_ui = false;
            const char *fail = decode_and_play(p, &result.seconds, &own_ui);
            if (own_ui) {
                muse_state_set_level(0);
                muse_state_set_mode(MUSE_MODE_IDLE);
                muse_state_set_caption("%s", "");
            }
            if (fail) {
                result.code = "invalid_mp3";
                result.message = fail;
            } else {
                result.ok = true;
                result.code = NULL;
            }
        }
    }
    if (p->http) esp_http_client_cleanup(p->http);

    result.bytes = (unsigned)p->bytes;
    result.ms = (int)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "audio.play_url %s: %.2fs, %u bytes in %d ms, stack %u free",
             result.ok ? "done" : "failed", result.seconds, result.bytes, result.ms,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    p->done(&result, p->user);
    free(p->mp3);
    free(p->url);
    free(p);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

bool audio_play_start(const char *url, audio_play_done_cb done, void *user,
                      const char **code, const char **message) {
    bool https = strncasecmp(url, "https://", 8) == 0;
    if (!https && strncasecmp(url, "http://", 7) != 0) {
        *code = "invalid_params";
        *message = "url must be http:// or https://";
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        *code = "busy";
        *message = "another audio clip is still playing";
        return false;
    }
    if (internal_free() < PLAY_RESERVE_BYTES) {
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = "not enough free memory for an audio download";
        return false;
    }

    play_t *p = calloc(1, sizeof(*p));
    if (p) p->url = strdup(url);
    if (p) p->mp3 = heap_caps_malloc(MP3_CAP_BYTES, MALLOC_CAP_SPIRAM);
    if (!p || !p->url || !p->mp3) {
        free(p ? p->mp3 : NULL);
        free(p ? p->url : NULL);
        free(p);
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = "failed to allocate";
        return false;
    }
    p->https = https;
    p->done = done;
    p->user = user;
    if (xTaskCreateWithCaps(play_task, "audio_play", PLAY_STACK_BYTES, p, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(p->mp3);
        free(p->url);
        free(p);
        atomic_store(&s_busy, false);
        *code = "out_of_memory";
        *message = "failed to start the playback task";
        return false;
    }
    return true;
}

#endif // CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND
