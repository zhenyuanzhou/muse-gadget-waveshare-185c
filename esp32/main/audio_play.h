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

#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

/*
 * audio.play_url: download an MP3 and play it through the Muse speaker.
 *
 * The Muse sends a standard MP3 over http:// or https:// (the same fetch
 * contract as display.draw_url); the firmware downloads it into PSRAM,
 * decodes it with minimp3 and plays it at 16 kHz mono through muse_audio,
 * the codec path the reply speech uses. While the gadget is idle the UI
 * shows the speaking face with a caption; an active push-to-talk turn keeps
 * its own mode and caption and only the audio plays.
 */

#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND

typedef struct {
    bool ok;
    const char *code;       // failure classification, when !ok
    const char *message;    // human-readable failure detail, when !ok
    double seconds;         // decoded audio length
    unsigned bytes;         // MP3 bytes downloaded
    int ms;                 // total download + decode + play time
} audio_play_result_t;

typedef void (*audio_play_done_cb)(const audio_play_result_t *result, void *user);

// Takes over the audio path and starts the download on a new task, calling
// `done` exactly once from that task with the outcome. Only one playback runs
// at a time: while one is, false comes back with *code/"busy".
// On false *code and *message are set; on true they are untouched.
bool audio_play_start(const char *url, audio_play_done_cb done, void *user,
                      const char **code, const char **message);

#else

#include <stddef.h>

typedef struct audio_play_result_t {
    bool ok;
    const char *code;
    const char *message;
    double seconds;
    unsigned bytes;
    int ms;
} audio_play_result_t;

typedef void (*audio_play_done_cb)(const audio_play_result_t *result, void *user);

static inline bool audio_play_start(const char *url, audio_play_done_cb done, void *user,
                                    const char **code, const char **message) {
    (void)url;
    (void)done;
    (void)user;
    *code = "unsupported";
    *message = "this board has no Muse speaker path";
    return false;
}

#endif
