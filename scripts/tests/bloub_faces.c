/* Renders one face per shape with the ported character code, as PPM files.
 *
 *   cc -I main/display/bloub -o /tmp/bloub_faces \\
 *      scripts/tests/bloub_faces.c main/display/bloub/bloub_shapes.c \\
 *      main/display/bloub/bloub_face.c -lm
 *   /tmp/bloub_faces /tmp
 *
 * A rendering aid, not a check: it is how you look at the character without a
 * watch. The firmware draws into an LVGL canvas; this writes PPMs. */
#include <stdio.h>
#include <string.h>

#include "bloub_shapes.h"

#define S 200
#define BODY 0xFFFF
#define BG 0x0000

static uint16_t buf[S * S];

static int save_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", S, S);
    for (int i = 0; i < S * S; i++) {
        const uint16_t p = buf[i];
        const unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((p >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((p & 0x1F) * 255 / 31)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    const bloub_gaze_t gaze = BLOUB_REST_GAZE;

    for (int s = 0; s < SHAPE_COUNT; s++) {
        for (int i = 0; i < S * S; i++) buf[i] = BG;

        bloub_face_cfg_t f;
        memset(&f, 0, sizeof(f));
        f.radii = SHAPE_PROFILES[s];
        f.gaze = &gaze;
        f.split = BLOUB_EYE_SPLIT;
        f.scale = 62.0f;
        f.cx = f.cy = S / 2.0f;
        f.sx = f.sy = 1.0f;
        f.eye_alpha = 1.0f;
        for (int e = 0; e < 2; e++) {
            f.eyes[e].w = 0.236f;   /* the resting expression's eye, from bloub */
            f.eyes[e].h = 0.447f;
            f.eyes[e].open = 1.0f;
        }

        bloub_draw_face(buf, S, S, &f, BODY, BG);

        char path[256];
        snprintf(path, sizeof(path), "%s/bloub-face-%d.ppm", dir, s);
        if (!save_ppm(path)) return 1;
        printf("%s\n", path);
    }
    return 0;
}
