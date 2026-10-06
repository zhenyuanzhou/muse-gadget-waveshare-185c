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

/*
 * Snake for the Muse UI: a grid arena clipped to a circle on a round screen,
 * rendered into an LVGL canvas. Swipes set the heading (the first swipe also
 * starts a round), a tap restarts after game over, and the small X at the top
 * leaves back to the settings home. Runs on the LVGL timer at ~5 steps a
 * second; each step repaints the arena into an off-screen canvas buffer in
 * PSRAM and LVGL blits it.
 */

#include "muse_game_snake.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "muse_board.h"
#include "muse_state.h"

static const char *TAG = "game_snake";

#define CELL 15                 /* arena cell size, px */
#define MAX_LEN 160             /* plenty: the circle holds ~90 cells on the 1.85C */
#define START_MS 210            /* step interval at score 0 */
#define MIN_MS 110              /* fastest step */
#define SPEED_UP_MS 3           /* interval drop per food */

#define COLOR_BG        lv_color_hex(0x0b0f0a)
#define COLOR_RIM       lv_color_hex(0x24352a)
#define COLOR_CELL_OFF  lv_color_hex(0x101a12)
#define COLOR_HEAD      lv_color_hex(0x7be382)
#define COLOR_TONGUE    lv_color_hex(0xff5470)
#define COLOR_FOOD      lv_color_hex(0xff4f6d)
#define COLOR_FOOD_HL   lv_color_hex(0xffb3c2)
#define COLOR_TEXT      lv_color_hex(0xf2fff4)
#define COLOR_DIM       lv_color_hex(0x7d997f)
#define COLOR_ACCENT    lv_color_hex(0x7be382)

typedef struct {
    int8_t x, y;
} cell_t;

typedef enum {
    ST_READY,       /* title shown, waiting for the first swipe */
    ST_PLAYING,
    ST_DEAD,
} state_t;

static lv_obj_t *s_page;        /* full-screen container on the settings tile */
static lv_obj_t *s_canvas;
static lv_obj_t *s_score_lbl;
static lv_obj_t *s_overlay_lbl;
static lv_obj_t *s_exit_btn;
static lv_timer_t *s_timer;
static void (*s_on_exit)(void);

static uint8_t *s_buf;          /* canvas pixels, PSRAM */
static int s_side;              /* canvas is s_side x s_side */
static int s_n;                 /* cells per row */
static int s_field_r2;          /* squared arena radius, cell coords */

static cell_t s_snake[MAX_LEN];
static int s_len;
static cell_t s_dir;            /* current heading */
static cell_t s_queued;         /* one buffered turn */
static bool s_has_queued;
static cell_t s_food;
static bool s_food_live;
static state_t s_state = ST_READY;
static int s_score;
static int s_best;
static uint32_t s_step_ms = START_MS;
static uint32_t s_tick;         /* tongue wiggle counter */

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static int cell_px(int c)
{
    return c * CELL + CELL / 2;   /* cell centre in canvas px */
}

/* Inside the round arena? Cell coords. */
static bool in_arena(int x, int y)
{
    int dx = x - (s_n - 1) / 2;
    int dy = y - (s_n - 1) / 2;
    return dx * dx + dy * dy <= s_field_r2;
}

static bool is_opposite(cell_t a, cell_t b)
{
    return a.x == -b.x && a.y == -b.y;
}

static void food_spawn(void)
{
    for (int tries = 0; tries < 200; tries++) {
        int x = esp_random() % s_n;
        int y = esp_random() % s_n;
        if (!in_arena(x, y)) {
            continue;
        }
        bool on_snake = false;
        for (int i = 0; i < s_len; i++) {
            if (s_snake[i].x == x && s_snake[i].y == y) {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) {
            s_food = (cell_t){ (int8_t)x, (int8_t)y };
            s_food_live = true;
            return;
        }
    }
    /* Board full: you win. Treat as game over with a perfect score. */
    s_food_live = false;
    s_state = ST_DEAD;
}

static void snake_reset(void)
{
    int c = (s_n - 1) / 2;
    s_len = 3;
    s_snake[0] = (cell_t){ (int8_t)c, (int8_t)c };
    s_snake[1] = (cell_t){ (int8_t)(c - 1), (int8_t)c };
    s_snake[2] = (cell_t){ (int8_t)(c - 2), (int8_t)c };
    s_dir = (cell_t){ 1, 0 };
    s_has_queued = false;
    s_score = 0;
    s_step_ms = START_MS;
    s_tick = 0;
    s_state = ST_READY;
    s_food_live = false;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

/* LVGL 9 canvas: no lv_canvas_draw_* helpers; open a layer per frame and use
 * the generic lv_draw_rect / lv_draw_line on it. Coordinates are canvas-local. */
static void fill_cell(lv_layer_t *layer, int x, int y, lv_color_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 3;
    lv_area_t a = { x * CELL + 1, y * CELL + 1, x * CELL + CELL - 2, y * CELL + CELL - 2 };
    lv_draw_rect(layer, &d, &a);
}

/* The rounded head with eyes that look along the heading and a tongue that
 * flicks out on alternate ticks. */
static void draw_head(lv_layer_t *layer)
{
    cell_t h = s_snake[0];
    int hx = cell_px(h.x);
    int hy = cell_px(h.y);
    int x0 = h.x * CELL + 1, y0 = h.y * CELL + 1;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = COLOR_HEAD;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 6;
    lv_area_t a = { x0, y0, x0 + CELL - 3, y0 + CELL - 3 };
    lv_draw_rect(layer, &d, &a);

    /* Eyes: two white beads with dark pupils, offset sideways from the
     * heading and pushed a little forward. */
    int fx = s_dir.x, fy = s_dir.y;         /* forward */
    int sx = -fy, sy = fx;                  /* side */
    for (int s = -1; s <= 1; s += 2) {
        int ex = hx + fx * 2 + sx * 4 * s;
        int ey = hy + fy * 2 + sy * 4 * s;
        lv_draw_rect_dsc_t e;
        lv_draw_rect_dsc_init(&e);
        e.bg_color = lv_color_hex(0xffffff);
        e.bg_opa = LV_OPA_COVER;
        e.radius = LV_RADIUS_CIRCLE;
        lv_area_t ea = { ex - 3, ey - 3, ex + 3, ey + 3 };
        lv_draw_rect(layer, &e, &ea);
        int px = ex + fx, py = ey + fy;
        lv_draw_rect_dsc_t p;
        lv_draw_rect_dsc_init(&p);
        p.bg_color = lv_color_hex(0x14210f);
        p.bg_opa = LV_OPA_COVER;
        p.radius = LV_RADIUS_CIRCLE;
        lv_area_t pa = { px - 1, py - 1, px + 1, py + 1 };
        lv_draw_rect(layer, &p, &pa);
    }

    /* Tongue: a short fork flicking out of the front on alternate ticks. */
    if ((s_tick & 2) && s_state == ST_PLAYING) {
        int tx = hx + fx * (CELL / 2 + 1);
        int ty = hy + fy * (CELL / 2 + 1);
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.p1.x = tx;
        l.p1.y = ty;
        l.p2.x = tx + fx * 5;
        l.p2.y = ty + fy * 5;
        l.color = COLOR_TONGUE;
        l.width = 2;
        l.round_end = 1;
        lv_draw_line(layer, &l);
    }
}

static void draw_food(lv_layer_t *layer)
{
    if (!s_food_live) {
        return;
    }
    int fx = cell_px(s_food.x);
    int fy = cell_px(s_food.y);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = COLOR_FOOD;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { fx - 5, fy - 5, fx + 5, fy + 5 };
    lv_draw_rect(layer, &d, &a);
    lv_draw_rect_dsc_t h;
    lv_draw_rect_dsc_init(&h);
    h.bg_color = COLOR_FOOD_HL;
    h.bg_opa = LV_OPA_COVER;
    h.radius = LV_RADIUS_CIRCLE;
    lv_area_t ha = { fx - 3, fy - 3, fx - 1, fy - 1 };
    lv_draw_rect(layer, &h, &ha);
}

/* Body with a soft head-to-tail green ramp; the neck is the brightest so the
 * head pops against it. */
static void draw_body(lv_layer_t *layer)
{
    for (int i = s_len - 1; i >= 1; i--) {
        int t = i * 255 / (s_len > 1 ? s_len - 1 : 1);
        lv_color_t c = lv_color_mix(COLOR_HEAD, lv_color_hex(0x2e7d43), t);
        fill_cell(layer, s_snake[i].x, s_snake[i].y, c);
    }
}

static void draw_frame(void)
{
    lv_canvas_fill_bg(s_canvas, COLOR_BG, LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    /* The arena disc, a touch lighter than the surround. */
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = COLOR_CELL_OFF;
    d.bg_opa = LV_OPA_COVER;
    d.border_color = COLOR_RIM;
    d.border_width = 2;
    d.border_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    int r = s_side / 2 - 2;
    int c = s_side / 2;
    lv_area_t a = { c - r, c - r, c + r, c + r };
    lv_draw_rect(&layer, &d, &a);

    draw_food(&layer);
    draw_body(&layer);
    draw_head(&layer);

    lv_canvas_finish_layer(s_canvas, &layer);
}

/* ------------------------------------------------------------------ */
/* game logic                                                          */

static void update_labels(void)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%d", s_score);
    if (strcmp(lv_label_get_text(s_score_lbl), buf) != 0) {
        lv_label_set_text(s_score_lbl, buf);
    }

    const char *overlay = "";
    if (s_state == ST_READY) {
        overlay = "SNAKE\n\nswipe to play";
    } else if (s_state == ST_DEAD) {
        static char obuf[64];
        snprintf(obuf, sizeof(obuf), "game over\n\nscore %d   best %d\n\ntap to retry", s_score, s_best);
        overlay = obuf;
    }
    if (strcmp(lv_label_get_text(s_overlay_lbl), overlay) != 0) {
        lv_label_set_text(s_overlay_lbl, overlay);
    }
}

static void step(void)
{
    s_tick++;

    if (s_has_queued && !is_opposite(s_queued, s_dir)) {
        s_dir = s_queued;
    }
    s_has_queued = false;

    cell_t head = { (int8_t)(s_snake[0].x + s_dir.x), (int8_t)(s_snake[0].y + s_dir.y) };

    bool eats = s_food_live && head.x == s_food.x && head.y == s_food.y;
    int body_limit = eats ? s_len : s_len - 1;   /* the tail vacates unless we grow */

    if (!in_arena(head.x, head.y)) {
        s_state = ST_DEAD;
        if (s_score > s_best) {
            s_best = s_score;
        }
        ESP_LOGI(TAG, "hit the wall, score %d", s_score);
        update_labels();
        return;
    }
    for (int i = 0; i < body_limit; i++) {
        if (s_snake[i].x == head.x && s_snake[i].y == head.y) {
            s_state = ST_DEAD;
            if (s_score > s_best) {
                s_best = s_score;
            }
            ESP_LOGI(TAG, "bit itself, score %d", s_score);
            update_labels();
            return;
        }
    }

    if (eats) {
        if (s_len < MAX_LEN) {
            s_len++;
        }
        s_score++;
        if (s_step_ms > MIN_MS) {
            s_step_ms -= SPEED_UP_MS;
            lv_timer_set_period(s_timer, s_step_ms);
        }
        food_spawn();
        ESP_LOGI(TAG, "ate, score %d", s_score);
    }
    memmove(&s_snake[1], &s_snake[0], (size_t)(s_len - 1) * sizeof(s_snake[0]));
    s_snake[0] = head;
    update_labels();
}

static void on_timer(lv_timer_t *t)
{
    (void)t;
    if (s_state == ST_PLAYING) {
        step();
        draw_frame();
    }
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static void queue_turn(cell_t d)
{
    if (s_state == ST_READY) {
        s_dir = d;
        s_state = ST_PLAYING;
        if (!s_food_live) {
            food_spawn();
        }
        update_labels();
        draw_frame();
        return;
    }
    if (s_state == ST_PLAYING && !is_opposite(d, s_dir) && !(d.x == s_dir.x && d.y == s_dir.y)) {
        s_queued = d;
        s_has_queued = true;
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t g = lv_indev_get_gesture_dir(lv_indev_active());
    (void)e;
    switch (g) {
    case LV_DIR_TOP:    queue_turn((cell_t){ 0, -1 }); break;
    case LV_DIR_BOTTOM: queue_turn((cell_t){ 0, 1 });  break;
    case LV_DIR_LEFT:   queue_turn((cell_t){ -1, 0 }); break;
    case LV_DIR_RIGHT:  queue_turn((cell_t){ 1, 0 });  break;
    default: break;
    }
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_DEAD) {
        snake_reset();
        update_labels();
        draw_frame();
    } else if (s_state == ST_READY) {
        /* A tap starts too, heading right. */
        queue_turn((cell_t){ 1, 0 });
    }
}

static void on_exit_click(lv_event_t *e)
{
    (void)e;
    if (s_on_exit) {
        s_on_exit();
    }
}

/* ------------------------------------------------------------------ */
/* page lifecycle                                                      */

static void on_page_delete(lv_event_t *e)
{
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_buf) {
        free(s_buf);
        s_buf = NULL;
    }
    s_page = NULL;
    s_canvas = NULL;
    ESP_LOGI(TAG, "page dropped");
}

lv_obj_t *muse_game_snake_build(lv_obj_t *tile, void (*on_exit)(void))
{
    s_on_exit = on_exit;

    int side = LV_MIN(muse_board->width, muse_board->height);
    side = side * 11 / 12;                 /* clear the round bezel */
    side = (side / CELL) * CELL;
    s_side = side;
    s_n = side / CELL;
    int fr = (s_n - 1) / 2;                /* arena radius in cells, corners off */
    s_field_r2 = fr * fr;

    s_buf = heap_caps_malloc((size_t)side * side * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the canvas");
        return NULL;
    }

    s_page = lv_obj_create(tile);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    /* Swipes steer the snake; they must not bubble to the tileview. */
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_page, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_page, on_press, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_page, on_page_delete, LV_EVENT_DELETE, NULL);

    s_canvas = lv_canvas_create(s_page);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, side, side);
    lv_obj_center(s_canvas);
    lv_canvas_set_buffer(s_canvas, s_buf, side, side, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_canvas, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_canvas, on_press, LV_EVENT_CLICKED, NULL);

    /* Score, top centre. */
    s_score_lbl = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_score_lbl, COLOR_TEXT, 0);
    lv_label_set_text(s_score_lbl, "0");
    lv_obj_align(s_score_lbl, LV_ALIGN_TOP_MID, 0, 4);

    /* Center overlay: title / game over. */
    s_overlay_lbl = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_overlay_lbl, COLOR_TEXT, 0);
    lv_obj_set_style_text_line_space(s_overlay_lbl, 6, 0);
    lv_obj_set_style_text_align(s_overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_overlay_lbl, side - 40);
    lv_label_set_long_mode(s_overlay_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_overlay_lbl, "");
    lv_obj_align(s_overlay_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Exit X, top right, clear of the score. */
    s_exit_btn = lv_button_create(s_page);
    lv_obj_remove_style_all(s_exit_btn);
    lv_obj_set_size(s_exit_btn, 40, 34);
    lv_obj_align(s_exit_btn, LV_ALIGN_TOP_RIGHT, -6, 2);
    lv_obj_set_style_radius(s_exit_btn, 8, 0);
    lv_obj_set_style_bg_opa(s_exit_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_exit_btn, lv_color_hex(0x24352a), 0);
    lv_obj_set_style_bg_color(s_exit_btn, lv_color_hex(0x3a5442), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_exit_btn, on_exit_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(s_exit_btn);
    lv_obj_set_style_text_font(x, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(x, COLOR_ACCENT, 0);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    snake_reset();
    update_labels();
    draw_frame();

    s_timer = lv_timer_create(on_timer, START_MS, NULL);
    lv_timer_pause(s_timer);

    ESP_LOGI(TAG, "built: %d x %d, grid %d, arena r %d cells", side, side, s_n, fr);
    return s_page;
}

void muse_game_snake_set_visible(bool visible)
{
    if (!s_timer) {
        return;
    }
    bool running = visible && s_state == ST_PLAYING && !muse_state_asleep();
    if (running) {
        lv_timer_resume(s_timer);
    } else {
        lv_timer_pause(s_timer);
    }
}
