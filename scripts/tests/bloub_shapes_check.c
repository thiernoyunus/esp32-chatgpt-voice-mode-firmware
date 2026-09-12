/* Checks the ported silhouettes and the two primitives that draw them.
 *
 *   cc -I main/display/bloub -o /tmp/bloub_shapes_check \\
 *      scripts/tests/bloub_shapes_check.c main/display/bloub/bloub_shapes.c \\
 *      main/display/bloub/bloub_face.c -lm
 *
 * No LVGL, no display: a buffer of panel words and the arithmetic. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bloub_shapes.h"

/* M_PI is POSIX, not ISO C: a strict -std=c99 build gets <math.h> without it.
 * This check is meant to run anywhere with a C compiler, so it brings its own. */
#define CHECK_PI 3.14159265358979323846

#define W 200
#define H 200
static uint16_t buf[W * H];
static const uint16_t BODY = 0xFFFF, BG = 0x0000;

static int count_body(void) {
    int n = 0;
    for (int i = 0; i < W * H; i++) if (buf[i] == BODY) n++;
    return n;
}

static int count_body_of(const uint16_t* b) {
    int n = 0;
    for (int i = 0; i < W * H; i++) if (b[i] == BODY) n++;
    return n;
}

/* Mean x of the pixels a face erased, measured against the bare silhouette. */
static double holes_centroid_x(const uint16_t* b, const uint16_t* bare) {
    double sx = 0.0;
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        if (bare[i] == BODY && b[i] != BODY) { sx += (double)(i % W); n++; }
    }
    return n ? sx / n : 0.0;
}

int main(void) {
    /* 1. Every silhouette is a usable profile. */
    for (int s = 0; s < SHAPE_COUNT; s++) {
        float peak = 0.0f;
        for (int i = 0; i < SHAPE_SAMPLES; i++) {
            assert(SHAPE_PROFILES[s][i] > 0.0f);
            if (SHAPE_PROFILES[s][i] > peak) peak = SHAPE_PROFILES[s][i];
        }
        assert(peak > 0.9f && peak < 1.2f);
        assert(shape_name((shape_id_t)s)[0] != 0);
    }

    /* 2. A filled circle is a circle: the right area, and nothing outside it. */
    for (int i = 0; i < W * H; i++) buf[i] = BG;
    bloub_fill_shape(buf, W, H, SHAPE_PROFILES[SHAPE_CIRCLE], 50.0f, 100.0f, 100.0f, BODY);
    const int filled = count_body();
    const double expected = CHECK_PI * 50.0 * 50.0;
    assert(filled > 0);
    assert(fabs(filled - expected) / expected < 0.03);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (buf[y * W + x] != BODY) continue;
            const double dx = x + 0.5 - 100.0, dy = y + 0.5 - 100.0;
            assert(sqrt(dx * dx + dy * dy) <= 50.5);
        }
    }

    /* 3. The eye is erased, and only the eye. */
    bloub_punch_eye(buf, W, H, 100.0f, 100.0f, 20.0f, 20.0f, 0.0f, BG);
    const int after = count_body();
    const int removed = filled - after;
    const double capsule = CHECK_PI * 10.0 * 10.0;   /* a 20x20 capsule is a disc */
    assert(removed > capsule * 0.9 && removed < capsule * 1.1);

    /* 4. Punching the same eye twice changes nothing, and an eye that lands
     *    outside the body erases background rather than body - which is how the
     *    silhouette clips the eye for free. */
    bloub_punch_eye(buf, W, H, 100.0f, 100.0f, 20.0f, 20.0f, 0.0f, BG);
    assert(count_body() == after);
    bloub_punch_eye(buf, W, H, 100.0f, 20.0f, 20.0f, 20.0f, 0.0f, BG);
    assert(count_body() == after);

    /* 5. A shape that is not a circle stays inside its own profile: the
     *    droplet's point is at the top, so no body pixel may appear above it. */
    for (int i = 0; i < W * H; i++) buf[i] = BG;
    bloub_fill_shape(buf, W, H, SHAPE_PROFILES[SHAPE_DROPLET], 60.0f, 100.0f, 100.0f, BODY);
    int top = H;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (buf[y * W + x] == BODY && y < top) top = y;
    assert(top > 100 - 65 && top < 100 - 55);   /* the peak radius, to the pixel */

    /* 6. A whole face: body filled, two eyes punched, nothing painted outside
     *    the silhouette, and the eyes actually move when the head turns. */
    static uint16_t ref[W * H];
    bloub_gaze_t gaze = BLOUB_REST_GAZE;
    bloub_face_cfg_t face;
    memset(&face, 0, sizeof(face));
    face.radii = SHAPE_PROFILES[SHAPE_CIRCLE];
    face.gaze = &gaze;
    face.split = BLOUB_EYE_SPLIT;
    face.scale = 60.0f;
    face.cx = 100.0f;
    face.cy = 100.0f;
    face.sx = face.sy = 1.0f;
    face.eye_alpha = 1.0f;
    for (int e = 0; e < 2; e++) {
        face.eyes[e].w = 0.30f;
        face.eyes[e].h = 0.45f;
        face.eyes[e].open = 1.0f;
    }

    for (int i = 0; i < W * H; i++) ref[i] = BG;
    bloub_fill_shape(ref, W, H, SHAPE_PROFILES[SHAPE_CIRCLE], 60.0f, 100.0f, 100.0f, BODY);
    const int bare = count_body_of(ref);

    for (int i = 0; i < W * H; i++) buf[i] = BG;
    bloub_draw_face(buf, W, H, &face, BODY, BG);
    assert(count_body() < bare);                       /* the eyes are holes */
    for (int i = 0; i < W * H; i++)
        if (buf[i] == BODY) assert(ref[i] == BODY);    /* never outside the body */

    /* Turn the head: the eyes have to move with it. */
    bloub_gaze_t turned = { BLOUB_REST_GAZE.yaw + 45.0f, BLOUB_REST_GAZE.pitch,
                            BLOUB_REST_GAZE.roll };
    face.gaze = &turned;
    static uint16_t buf2[W * H];
    for (int i = 0; i < W * H; i++) buf2[i] = BG;
    bloub_draw_face(buf2, W, H, &face, BODY, BG);
    const double moved = fabs(holes_centroid_x(buf, ref) - holes_centroid_x(buf2, ref));
    assert(moved > 4.0);

    /* Eyes off: the face is the bare silhouette, to the pixel. */
    face.gaze = &gaze;
    face.eye_alpha = 0.0f;
    for (int i = 0; i < W * H; i++) buf[i] = BG;
    bloub_draw_face(buf, W, H, &face, BODY, BG);
    assert(count_body() == bare);

    /* A steep gaze must erase exactly what an unbounded scan would. The eye's
     * box used to be divided by the frame's determinant, which grew it without
     * bound as the eye turned edge-on - slow, but also the kind of thing that
     * is easy to "fix" into clipping the eye instead. This fails either way. */
    {
        const bloub_gaze_t steep = { 72.0f, 5.0f, -4.0f };
        bloub_face_cfg_t f;
        memset(&f, 0, sizeof(f));
        f.radii = SHAPE_PROFILES[SHAPE_CIRCLE];
        f.gaze = &steep;
        f.split = 16.0f;
        f.scale = 52.0f;
        f.cx = f.cy = W / 2.0f;
        f.sx = f.sy = 1.0f;
        f.eye_alpha = 1.0f;
        for (int e = 0; e < 2; e++) { f.eyes[e].w = 0.21f; f.eyes[e].h = 0.44f; f.eyes[e].open = 1.0f; }
        memset(buf, 0, sizeof(buf));
        bloub_draw_face(buf, W, H, &f, BODY, BG);
        const int drawn = count_body();

        /* The same face, erased by hand over the whole canvas. */
        memset(buf, 0, sizeof(buf));
        bloub_fill_shape(buf, W, H, f.radii, f.scale, f.cx, f.cy, BODY);
        bloub_eye_t pose[2];
        bloub_eye_poses(steep, f.scale, f.split, pose);
        for (int e = 0; e < 2; e++) {
            if (pose[e].depth <= 0.02f) continue;
            const float a = pose[e].a, b = pose[e].b, c = pose[e].c, d = pose[e].d;
            const float det = a * d - b * c;
            const float hw = f.eyes[e].w * 0.5f * f.scale, hh = f.eyes[e].h * 0.5f * f.scale;
            /* The eye sits where the sphere put it, not at the face's centre. */
            const float ecx = f.cx + pose[e].x * f.sx, ecy = f.cy + pose[e].y * f.sy;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                const float dx = (float)x + 0.5f - ecx, dy = (float)y + 0.5f - ecy;
                const float u = fabsf((d * dx - b * dy) / det), v = fabsf((-c * dx + a * dy) / det);
                const float rr = hw < hh ? hw : hh, ix = hw - rr, iy = hh - rr;
                int inside = (u <= ix && v <= hh) || (v <= iy && u <= hw);
                if (!inside) { const float qx = u - ix, qy = v - iy; inside = qx * qx + qy * qy <= rr * rr; }
                if (inside) buf[y * W + x] = BG;
            }
        }
        assert(drawn == count_body());
    }

    printf("ok: %d shapes, circle %d px (%.1f%% of pi r^2), eye removed %d px, "
           "turning the head moves the eyes %.1f px\n",
           SHAPE_COUNT, filled, 100.0 * filled / expected, removed, moved);
    return 0;
}
