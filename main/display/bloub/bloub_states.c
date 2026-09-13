#include "bloub_states.h"

#include "bloub_math.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* play's swoosh: a, k, tilt, speed, phase, sweep, hue, hueSpan, width, cx, cy. */
static const bloub_arc_seed_t SWOOSH[4] = {
    {0.78f, 0.05f, -0.62f, 0.30f, 0.00f, 0.40f,  95.0f, 100.0f, 0.05f, 0.0f, -0.12f},
    {0.98f, 0.07f, -0.57f, 0.30f, 0.06f, 0.40f, 157.0f, 100.0f, 0.05f, 0.0f, -0.12f},
    {1.18f, 0.09f, -0.52f, 0.30f, 0.12f, 0.40f, 219.0f, 100.0f, 0.05f, 0.0f, -0.12f},
    {1.38f, 0.11f, -0.47f, 0.30f, 0.18f, 0.40f, 281.0f, 100.0f, 0.05f, 0.0f, -0.12f},
};

/* thinking's three dots: where they sit, how big, and how far they swell. */
#define DOT_X0 (-0.557f)
#define DOT_X1 (-0.013f)
#define DOT_X2 (0.532f)
#define DOT_R 0.165f
#define DOT_PEAK 1.25f

/* The triangle's centre orbits a small circle rather than spinning on the
 * spot, which is what reads as it toppling. */
#define TRI_ORBIT 0.213f

/* Measured. Held for `duration`, faded into over `morph`. */
static const float DURATION[BLOUB_STATE_COUNT] = {2.4f, 2.6f, 1.6f, 1.8f, 2.0f};
static const float MORPH[BLOUB_STATE_COUNT] = {0.45f, 0.4f, 0.3f, 0.55f, 0.5f};

float bloub_state_duration(bloub_state_id_t id) {
    return (id >= 0 && id < BLOUB_STATE_COUNT) ? DURATION[id] : 2.0f;
}
float bloub_state_morph(bloub_state_id_t id) {
    return (id >= 0 && id < BLOUB_STATE_COUNT) ? MORPH[id] : 0.4f;
}

/* A wave that runs left to right across the three dots. */
static float dot_pulse(float t, int index) {
    float p = fmodf((t - (float)index * 0.5f) / 1.5f, 1.0f);
    if (p < 0.0f) p += 1.0f;
    const float k = p < 0.5f ? (0.5f - 0.5f * cosf(p * BLOUB_TAU)) : 0.0f;
    return bloub_clamp01(k * 2.0f);
}

static void pose_base(const float* body, bloub_pose_t* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < SHAPE_SAMPLES; i++) out->radii[i] = body[i];
    out->sx = out->sy = 1.0f;
    /* The call screen's resting gaze: facing whoever is in front of it. */
    out->gaze = (bloub_gaze_t){4.0f, 5.0f, -4.0f};
    out->split = 16.0f;
    out->eyes[0].w = out->eyes[1].w = 0.21f;
    out->eyes[0].h = out->eyes[1].h = 0.44f;
    out->eyes[0].open = out->eyes[1].open = 1.0f;
    out->eye_alpha = 1.0f;
}

static void set_circle(bloub_pose_t* out, float radius) {
    for (int i = 0; i < SHAPE_SAMPLES; i++) out->radii[i] = radius;
}
static void set_profile(bloub_pose_t* out, const float* profile) {
    for (int i = 0; i < SHAPE_SAMPLES; i++) out->radii[i] = profile[i];
}

void bloub_pose_sample(bloub_state_id_t id, float t, const float* body, bloub_pose_t* out) {
    if (out == NULL || body == NULL) return;
    pose_base(body, out);
    switch (id) {
    case BLOUB_STATE_IDLE:
        break;

    case BLOUB_STATE_THINKING: {
        /* The body itself becomes the middle dot, which is what keeps the
         * morph continuous - it shrinks into place rather than vanishing and
         * being replaced. */
        const float mid = dot_pulse(t, 1);
        const float emerge = 0.3f + 0.7f * bloub_ease_out_cubic(bloub_clamp01(t / 0.3f));
        set_circle(out, DOT_R * (1.0f + (DOT_PEAK - 1.0f) * mid));
        out->cx = DOT_X1;
        out->eye_alpha = 0.0f;
        const float xs[3] = {DOT_X0, DOT_X1, DOT_X2};
        const int outer[2] = {0, 2};
        out->dot_count = 2;
        for (int j = 0; j < 2; j++) {
            const int i = outer[j];
            const float k = dot_pulse(t, i);
            out->dots[j].x = xs[i] * emerge;
            out->dots[j].y = 0.0f;
            out->dots[j].r = DOT_R * (1.0f + (DOT_PEAK - 1.0f) * k);
            out->dots[j].opacity = 0.55f + 0.45f * k;
        }
        break;
    }

    case BLOUB_STATE_WINK:
        out->gaze = (bloub_gaze_t){-5.37f, 4.55f, 6.7f};
        out->split = 16.25f;
        /* The shut eye is a dash WIDER than the open one, not the open eye
         * squashed - 0.447 against 0.236. */
        out->eyes[0].w = 0.236f; out->eyes[0].h = 0.464f;
        out->eyes[1].w = 0.447f; out->eyes[1].h = 0.089f;
        break;

    case BLOUB_STATE_WIDE:
        out->gaze = (bloub_gaze_t){6.92f, -21.96f, 11.6f};
        out->split = 18.43f;
        out->eyes[0].w = out->eyes[1].w = 0.356f;
        out->eyes[0].h = out->eyes[1].h = 0.875f;
        break;

    case BLOUB_STATE_PLAY: {
        set_profile(out, SHAPE_PROFILES[SHAPE_TRIANGLE]);
        out->cx = 0.0f;
        out->cy = TRI_ORBIT;
        out->gaze = (bloub_gaze_t){12.0f, -8.0f, -6.0f};
        out->split = 15.0f;
        out->eyes[0].w = out->eyes[1].w = 0.18f;
        out->eyes[0].h = out->eyes[1].h = 0.34f;
        /* The bouquet sweeps right to left across the body and fades at both
         * ends of the block. */
        const float fade = bloub_clamp01(t / 0.35f) * bloub_clamp01((2.2f - t) / 0.5f);
        out->arc_count = 4;
        out->arc_t = t;
        for (int i = 0; i < 4; i++) {
            out->arcs[i] = SWOOSH[i];
            out->arcs[i].cx = 0.45f - t * 0.42f;
            out->arc_opacity[i] = fade;
        }
        break;
    }

    default:
        break;
    }
}

void bloub_pose_blend(const bloub_pose_t* a, const bloub_pose_t* b, float t, bloub_pose_t* out) {
    if (a == NULL || b == NULL || out == NULL) return;
    const float inv = 1.0f - t;
    for (int i = 0; i < SHAPE_SAMPLES; i++) out->radii[i] = bloub_lerp(a->radii[i], b->radii[i], t);
    /* Turn the short way round, so +170 degrees to -170 does not spin a whole
     * turn to get there. */
    float d_rot = b->rot - a->rot;
    while (d_rot > BLOUB_TAU * 0.5f) d_rot -= BLOUB_TAU;
    while (d_rot < -BLOUB_TAU * 0.5f) d_rot += BLOUB_TAU;
    out->rot = a->rot + d_rot * t;
    out->cx = bloub_lerp(a->cx, b->cx, t);
    out->cy = bloub_lerp(a->cy, b->cy, t);
    out->sx = bloub_lerp(a->sx, b->sx, t);
    out->sy = bloub_lerp(a->sy, b->sy, t);
    out->gaze.yaw = bloub_lerp(a->gaze.yaw, b->gaze.yaw, t);
    out->gaze.pitch = bloub_lerp(a->gaze.pitch, b->gaze.pitch, t);
    out->gaze.roll = bloub_lerp(a->gaze.roll, b->gaze.roll, t);
    out->split = bloub_lerp(a->split, b->split, t);
    for (int e = 0; e < 2; e++) {
        out->eyes[e].w = bloub_lerp(a->eyes[e].w, b->eyes[e].w, t);
        out->eyes[e].h = bloub_lerp(a->eyes[e].h, b->eyes[e].h, t);
        out->eyes[e].open = bloub_lerp(a->eyes[e].open, b->eyes[e].open, t);
    }
    out->eye_alpha = bloub_lerp(a->eye_alpha, b->eye_alpha, t);

    /* Decor cross-fades on opacity, both states' carried at once, rather than
     * trying to interpolate between different counts of things. */
    out->dot_count = 0;
    for (int i = 0; i < a->dot_count && out->dot_count < BLOUB_POSE_MAX_DOTS; i++) {
        out->dots[out->dot_count] = a->dots[i];
        out->dots[out->dot_count].opacity *= inv;
        out->dot_count++;
    }
    for (int i = 0; i < b->dot_count && out->dot_count < BLOUB_POSE_MAX_DOTS; i++) {
        out->dots[out->dot_count] = b->dots[i];
        out->dots[out->dot_count].opacity *= t;
        out->dot_count++;
    }
    out->arc_count = 0;
    for (int i = 0; i < a->arc_count && out->arc_count < BLOUB_POSE_MAX_ARCS; i++) {
        out->arcs[out->arc_count] = a->arcs[i];
        out->arc_opacity[out->arc_count] = a->arc_opacity[i] * inv;
        out->arc_count++;
    }
    for (int i = 0; i < b->arc_count && out->arc_count < BLOUB_POSE_MAX_ARCS; i++) {
        out->arcs[out->arc_count] = b->arcs[i];
        out->arc_opacity[out->arc_count] = b->arc_opacity[i] * t;
        out->arc_count++;
    }
    /* Both states' arcs want their own clock but a pose carries one. The
     * outgoing set is already fading, so follow whichever one is showing. */
    out->arc_t = t < 0.5f ? a->arc_t : b->arc_t;
}

void bloub_pose_draw_arcs(uint16_t* buf, int w, int h, const bloub_pose_t* p, float scale_px,
                          float cx, float cy, bool behind) {
    if (buf == NULL || p == NULL) return;
    for (int i = 0; i < p->arc_count; i++) {
        bloub_arc_draw(buf, w, h, &p->arcs[i], p->arc_t, scale_px, cx, cy, p->arc_opacity[i],
                       behind);
    }
}

void bloub_pose_draw_dots(uint16_t* buf, int w, int h, const bloub_pose_t* p, float scale_px,
                          float cx, float cy, uint16_t color) {
    if (buf == NULL || p == NULL) return;
    for (int i = 0; i < p->dot_count; i++) {
        bloub_draw_dot(buf, w, h, cx + p->dots[i].x * scale_px, cy + p->dots[i].y * scale_px,
                       p->dots[i].r * scale_px, color, p->dots[i].opacity);
    }
}
