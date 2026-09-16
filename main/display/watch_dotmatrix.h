#pragma once

/* Dot-matrix text, the app-pixels signature.
 *
 * Ported from the mockup at tools/lvgl-mcp/screens/v2/dotmatrix.h, where
 * the look was designed: a lit dot per pixel of a 5x7 cell, with the unlit dots
 * left faintly visible so the text reads as a physical matrix rather than as a
 * font. The ghost grid is the whole trick; without it this is just a pixel font.
 *
 * Two things changed on the way into firmware:
 *   - the canvas is RGB565 with an OPAQUE background, not ARGB8888 and
 *     transparent. Nothing here needs to show through: every screen that uses
 *     it is pure black.
 *   - the buffer is freed when its canvas dies. The mockup leaked it on purpose
 *     (it only ever took one screenshot); a watch that navigates cannot.
 *
 * ponytail: uppercase only, like the mockup. Lowercase folds to uppercase.
 */

#include <lvgl.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define DM_W 5
#define DM_H 7

static const uint8_t dm_font[][DM_W] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 space */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 apostrophe */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x08,0x14,0x22,0x41,0x00}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x00,0x41,0x22,0x14,0x08}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x07,0x08,0x70,0x08,0x07}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
};

/* Degree sign is not in the ASCII run, so it gets its own escape: ^ */
static const uint8_t dm_degree[DM_W] = {0x00,0x07,0x05,0x07,0x00};

typedef struct {
    int pitch;       /* dot centre spacing */
    int dot;         /* lit dot size */
    int gap_cols;    /* blank columns between glyphs */
    uint32_t color;
    uint32_t ghost;  /* unlit dot colour; 0 disables the grid */
} dm_style_t;

/* One glyph's 5 columns, bit n of each byte being row n, top to bottom. Space
 * is the fallback for anything outside the table, and the caret draws as a
 * degree sign because the mockups use it for one. */
static inline const uint8_t* dm_glyph(char ch) {
    if (ch == 0x5E) return dm_degree;
    if (ch >= 0x61 && ch <= 0x7A) ch = static_cast<char>(ch - 32);  /* fold lowercase */
    if (ch < 32 || ch > 90) ch = 32;
    return dm_font[static_cast<int>(ch) - 32];
}

/* Width of a string in pixels, for centring and for placing two blocks on one
 * row. Measuring beats guessing: at these sizes an eyeballed x lands outside
 * the circle. */
static inline int dm_width(const char* s, const dm_style_t* st) {
    const int n = static_cast<int>(strlen(s));
    return n ? (n * (DM_W + st->gap_cols) - st->gap_cols) * st->pitch : 0;
}

/* Text canvas buffers are tens of KB and there can be a dozen on a page, so on
 * the device they belong in PSRAM. The host harness has no heap_caps. */
#if defined(ESP_PLATFORM)
static inline void* dm_alloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}
static inline void dm_release(void* p) { if (p != nullptr) heap_caps_free(p); }
#else
static inline void* dm_alloc(size_t bytes) { return lv_malloc(bytes); }
static inline void dm_release(void* p) { lv_free(p); }
#endif

/* Frees the buffer with the widget: the canvas does not own its buffer, so
 * without this every page rebuild leaks one block per label. */
static inline void dm_free_buffer(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) dm_release(lv_event_get_user_data(e));
}

/* Draws s with its top-left at (x, y). The canvas is sized to the text extent,
 * so the ghost grid is exactly the block the text occupies - which is how the
 * reference screens look. */
static inline lv_obj_t* dm_text(lv_obj_t* parent, int x, int y, const char* s,
                                const dm_style_t* st) {
    const int n = static_cast<int>(strlen(s));
    if (n == 0) return nullptr;
    const int cols = n * (DM_W + st->gap_cols) - st->gap_cols;
    const int w = cols * st->pitch, h = DM_H * st->pitch;
    if (w <= 0 || h <= 0) return nullptr;

    auto* buf = static_cast<lv_color16_t*>(dm_alloc(static_cast<size_t>(w) * h * sizeof(lv_color16_t)));
    if (buf == nullptr) return nullptr;

    lv_obj_t* cv = lv_canvas_create(parent);
    if (cv == nullptr) { dm_release(buf); return nullptr; }
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, x, y);
    lv_obj_set_size(cv, w, h);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cv, dm_free_buffer, LV_EVENT_DELETE, buf);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.border_width = 0;
    dsc.radius = 1;

    const int inset = (st->pitch - st->dot) / 2;
    for (int i = 0; s[i]; i++) {
        const uint8_t* g = dm_glyph(s[i]);
        const int col0 = i * (DM_W + st->gap_cols);
        for (int c = 0; c < DM_W + st->gap_cols; c++) {
            for (int r = 0; r < DM_H; r++) {
                const int lit = c < DM_W && ((g[c] >> r) & 1);
                if (!lit && st->ghost == 0) continue;
                dsc.bg_color = lv_color_hex(lit ? st->color : st->ghost);
                lv_area_t a;
                a.x1 = (col0 + c) * st->pitch + inset;
                a.y1 = r * st->pitch + inset;
                a.x2 = a.x1 + st->dot - 1;
                a.y2 = a.y1 + st->dot - 1;
                lv_draw_rect(&layer, &dsc, &a);
            }
        }
    }
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

/* Same, centred on cx. */
static inline lv_obj_t* dm_text_center(lv_obj_t* parent, int cx, int y, const char* s,
                                       const dm_style_t* st) {
    return dm_text(parent, cx - dm_width(s, st) / 2, y, s, st);
}
