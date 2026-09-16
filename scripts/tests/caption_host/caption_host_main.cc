/* Renders the call screen's top caption slot, so a layout question can be
 * looked at rather than flashed. The slot is narrow: the screen is round, so
 * at the caption's height the usable width is a chord, not the full 360. */
#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
#include "display_driver.h"
#include "screenshot.h"
}

#include <lvgl.h>
#include "watch_dotmatrix.h"
#include "bloub/bloub_shapes.h"

static int g_shot = 0;

static void snap(const char* tag) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/voicemode-caption-%02d-%s.png", g_shot++, tag);
    /* LVGL only redraws on a tick, so a snapshot taken without one catches the
     * frame before everything on this screen was added. */
    for (int i = 0; i < 3; i++) { lv_tick_inc(20); lv_timer_handler(); }
    lv_refr_now(NULL);
    screenshot_save_png(path, headless_display_get_framebuffer(),
                        headless_display_get_width(), headless_display_get_height());
    printf("  %s\n", path);
}

/* Half the screen's width at height y - what a caption there actually has. */
static int chord_half(int y) {
    const int dy = y - 180, inside = 176 * 176 - dy * dy;
    return inside > 0 ? (int)sqrtf((float)inside) : 0;
}

/* The character, as a stand-in, so the caption is judged in context. */
static void draw_character(lv_obj_t* parent) {
    const int size = 166;
    auto* buf = (lv_color16_t*)lv_malloc((size_t)size * size * sizeof(lv_color16_t));
    memset(buf, 0, (size_t)size * size * sizeof(lv_color16_t));
    auto* cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(cv, size, size);
    lv_obj_align(cv, LV_ALIGN_CENTER, 0, 4);
    static const bloub_gaze_t gaze = {4.0f, 5.0f, -4.0f};
    bloub_face_cfg_t f{};
    f.radii = SHAPE_PROFILES[SHAPE_CIRCLE];
    f.gaze = &gaze; f.split = 16.0f;
    f.scale = size * 0.46f; f.cx = f.cy = size * 0.5f;
    f.sx = f.sy = 1.0f; f.eye_alpha = 1.0f;
    for (int e = 0; e < 2; e++) { f.eyes[e].w = 0.21f; f.eyes[e].h = 0.44f; f.eyes[e].open = 1; }
    bloub_draw_face((uint16_t*)buf, size, size, &f, lv_color_to_u16(lv_color_hex(0xF1EFE9)), 0);
}

static lv_obj_t* g_screen = nullptr;
static void reset_screen() {
    lv_obj_clean(g_screen);
    lv_obj_set_style_bg_color(g_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    draw_character(g_screen);
}

/* One caption line: an optional icon, then dot-matrix text, the pair centred
 * on x=180 and sharing the state word's own centre line. Returns its width. */
static int caption_line(const char* text, int pitch, bool icon, uint32_t color, int centre_y) {
    dm_style_t st = {pitch, pitch > 2 ? 2 : 1, 1, color, 0x101010};
    const int tw = dm_width(text, &st);
    const int th = DM_H * pitch;
    const int icon_w = icon ? 20 : 0, gap = icon ? 8 : 0;
    const int total = icon_w + gap + tw;
    const int x0 = 180 - total / 2;
    if (icon) {
        /* Stand-in for the connector's own 24px artwork, centred on the same
         * line as the text rather than on the box around it. */
        auto* dot = lv_obj_create(g_screen);
        lv_obj_set_size(dot, icon_w, icon_w);
        lv_obj_set_pos(dot, x0, centre_y - icon_w / 2);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    }
    dm_text(g_screen, x0 + icon_w + gap, centre_y - th / 2, text, &st);
    return total;
}

/* A rule showing how much width the round screen actually gives at that line. */
static void draw_limit(int centre_y) {
    const int half = chord_half(centre_y);
    auto* l = lv_obj_create(g_screen);
    lv_obj_set_size(l, half * 2, 1);
    lv_obj_set_pos(l, 180 - half, centre_y + 16);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(l, 0, 0);
}

int main() {
    lv_init();
    headless_display_init(360, 360);
    lv_tick_inc(100);
    lv_timer_handler();
    g_screen = lv_screen_active();

    /* The state word, as it is today: dot-matrix, pitch 3, top at y=42. */
    const int centre_y = 42 + DM_H * 3 / 2;
    printf("state word centre line y=%d, usable width there=%d\n", centre_y, chord_half(centre_y) * 2);

    reset_screen();
    caption_line("THINKING", 3, false, 0xF5A524, centre_y);
    draw_limit(centre_y);
    snap("a-thinking-today");

    /* Idle on the call screen: the thing to do, not a status. */
    reset_screen();
    caption_line("TAP TO WAKE", 3, false, 0x8E8E93, centre_y);
    draw_limit(centre_y);
    snap("f-tap-to-wake");

    /* The same size, with a tool caption in it. */
    reset_screen();
    caption_line("SEARCHING EMAIL", 3, true, 0xF5A524, centre_y);
    draw_limit(centre_y);
    snap("b-tool-pitch3");

    /* Same typeface, one step smaller, which is what actually fits. */
    reset_screen();
    caption_line("SEARCHING EMAIL", 2, true, 0xF5A524, centre_y);
    draw_limit(centre_y);
    snap("c-tool-pitch2");

    reset_screen();
    caption_line("CHECKING CALENDAR", 2, true, 0xF5A524, centre_y);
    draw_limit(centre_y);
    snap("d-tool-pitch2-long");

    /* Two lines: the state word kept, the tool under it at the smaller size. */
    reset_screen();
    caption_line("THINKING", 3, false, 0xF5A524, centre_y);
    caption_line("SEARCHING EMAIL", 2, true, 0x8E8E93, centre_y + 26);
    snap("e-both-stacked");

    const char* longest[] = {"SEARCHING EMAIL", "CHECKING CALENDAR", "READING DOCUMENTS",
                             "SEARCHING THE WEB"};
    for (unsigned i = 0; i < sizeof(longest) / sizeof(longest[0]); i++) {
        dm_style_t st2 = {2, 1, 1, 0, 0};
        dm_style_t st3 = {3, 2, 1, 0, 0};
        printf("%-20s pitch2=%dpx pitch3=%dpx (limit %d)\n", longest[i],
               dm_width(longest[i], &st2) + 28, dm_width(longest[i], &st3) + 28,
               chord_half(centre_y) * 2 - 20);
    }
    printf("done\n");
    return 0;
}
