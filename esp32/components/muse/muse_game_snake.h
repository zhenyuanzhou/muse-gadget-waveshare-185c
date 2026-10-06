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

#include "lvgl.h"

/*
 * Snake: a touch game for round screens (see devices/ for boards that run
 * it). The page is a full-screen LVGL canvas on a grid clipped to a circle;
 * swipes steer, tap restarts. Built lazily like the settings sub-pages and
 * deleted on the way back, so the canvas buffer only holds PSRAM while
 * playing. `on_exit` (go back to the settings home) is wired by the caller.
 */

lv_obj_t *muse_game_snake_build(lv_obj_t *tile, void (*on_exit)(void));

/* Called from the settings tick with whether the settings tile is on screen. */
void muse_game_snake_set_visible(bool visible);
