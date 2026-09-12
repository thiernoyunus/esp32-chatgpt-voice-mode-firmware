/* See bloub_face.h. Ported from bloub (github.com/jeremy-prt/bloub),
 * src/bot/face.ts - MIT Licence, Copyright (c) 2026 Jeremy Perret. The full
 * licence text is in LICENSE beside this file. */

#include "bloub_face.h"

const bloub_gaze_t BLOUB_REST_GAZE = { 28.49f, 28.62f, -13.0f };

typedef struct { float x, y, z; } vec3_t;

static float to_rad(float deg) { return deg * 3.14159265358979323846f / 180.0f; }

/* Rotate a pair of orthonormal vectors in their common plane. The source calls
 * this spin; it is how the head is turned without any matrix library. */
static void spin(vec3_t* u, vec3_t* v, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    const vec3_t u0 = *u, v0 = *v;
    u->x = u0.x * c + v0.x * s;
    u->y = u0.y * c + v0.y * s;
    u->z = u0.z * c + v0.z * s;
    v->x = v0.x * c - u0.x * s;
    v->y = v0.y * c - u0.y * s;
    v->z = v0.z * c - u0.z * s;
}

void bloub_eye_poses(bloub_gaze_t gaze, float scale, float split, bloub_eye_t out[2]) {
    /* Screen frame: x right, y down, z towards the viewer. */
    vec3_t f = { 0.0f, 0.0f, 1.0f };
    vec3_t right = { 1.0f, 0.0f, 0.0f };
    vec3_t down = { 0.0f, 1.0f, 0.0f };

    spin(&f, &right, to_rad(gaze.yaw));
    spin(&down, &f, to_rad(gaze.pitch));
    spin(&right, &down, to_rad(gaze.roll));

    for (int i = 0; i < 2; i++) {
        const float side = (i == 0) ? -1.0f : 1.0f;   /* 0 inner, 1 outer */
        vec3_t ef = f, er = right;
        spin(&ef, &er, to_rad(split * side));
        out[i].x = ef.x * scale;
        out[i].y = ef.y * scale;
        out[i].a = er.x;
        out[i].b = er.y;
        out[i].c = down.x;
        out[i].d = down.y;
        out[i].depth = ef.z;
    }
}

/* --- the blink schedule ---------------------------------------------------
 * The source draws a list of blink times from a seeded PRNG and keeps it, so
 * the same date always gives the same face. Rebuilt here on every call instead
 * of stored: no static state can get out of step after a pause or a seek, and
 * the walk is about one step per three seconds of uptime. */
#define BLOUB_BLINK_DUR 0.18f    /* source: one to two frames at 10 fps */
#define BLOUB_FIRST_BLINK 1.4f
#define BLOUB_BLINK_SEED 0x5eedu

static float blink_lid(float t) {
    if (t < BLOUB_FIRST_BLINK) return 1.0f;
    uint32_t rng = BLOUB_BLINK_SEED;
    float entry = BLOUB_FIRST_BLINK, last = BLOUB_FIRST_BLINK;
    while (entry <= t) {
        last = entry;
        /* 1.9 to 4.6 seconds apart... */
        float next = entry + 1.9f + bloub_rng(&rng) * 2.7f;
        /* ...and about one in five doubles up 0.24 s later. */
        if (bloub_rng(&rng) < 0.18f) {
            if (next <= t) last = next;
            next += 0.24f;
        }
        entry = next;
    }
    const float k = (t - last) / BLOUB_BLINK_DUR;
    if (k < 0.0f || k > 1.0f) return 1.0f;
    /* Shut fast, open slower. That asymmetry is measured, and it is most of
     * what stops a blink reading as a fade. */
    return k < 0.45f ? 1.0f - k / 0.45f : (k - 0.45f) / 0.55f;
}

float bloub_blink_lid(float t) { return blink_lid(t); }

float bloub_blink_scale(float lid) { return 0.06f + 0.94f * bloub_clamp01(lid); }

bloub_liveliness_t bloub_liveliness(float t, float wander, bool blink, bool enable_float) {
    bloub_liveliness_t l;
    /* Two octaves per axis, on periods that share no common multiple. */
    l.d_yaw = (bloub_loop_noise(t, 11.3f, 0.4f) * 5.5f
             + bloub_loop_noise(t, 3.7f, 2.1f) * 1.6f) * wander;
    l.d_pitch = (bloub_loop_noise(t, 9.1f, 1.3f) * 4.2f
               + bloub_loop_noise(t, 4.3f, 0.7f) * 1.3f) * wander;
    l.d_roll = bloub_loop_noise(t, 13.7f, 3.2f) * 2.2f * wander;
    l.lid = blink ? blink_lid(t) : 1.0f;
    l.drift_x = enable_float ? bloub_loop_noise(t, 7.9f, 1.9f) * 0.006f : 0.0f;
    l.drift_y = enable_float ? bloub_loop_noise(t, 5.3f, 0.3f) * 0.007f : 0.0f;
    l.breath = enable_float ? 1.0f + sinf((t / 3.4f) * BLOUB_TAU) * 0.005f : 1.0f;
    return l;
}
