#include "bloub_decor.h"

#include "bloub_math.h"

#include <math.h>
#include <stddef.h>

/* a, k, tilt, speed, phase, sweep, hue, hueSpan, width, cx, cy.
 * bloub generated these once from a seeded PRNG; they are carried over as
 * numbers rather than re-running the generator, so there is no chance of an
 * off-by-one in the call sequence. */
const bloub_arc_seed_t BLOUB_RINGS[BLOUB_ORBIT_RINGS] = {
    {1.374316f, 0.274937f, 0.111637f, 3.531251f, 3.521581f, 0.745288f, 4.5382f, 63.3283f, 0.052606f, 0.0000f, 0.1000f},
    {1.347803f, 0.316944f, 1.009311f, 3.411252f, 6.146908f, 0.646808f, 81.6460f, 66.5198f, 0.060184f, 0.0000f, 0.1000f},
    {1.396025f, 0.407338f, 1.103976f, 3.680664f, 1.062764f, 0.841260f, 149.8437f, 61.3668f, 0.056178f, 0.0000f, 0.1000f},
    {1.385397f, 0.396188f, 1.996122f, 3.015526f, 1.116472f, 0.674048f, 185.7906f, 108.9912f, 0.051693f, 0.0000f, 0.1000f},
    {1.375075f, 0.413304f, 2.554288f, 3.306341f, 5.341918f, 0.834125f, 269.8506f, 73.2949f, 0.052619f, 0.0000f, 0.1000f},
    {1.303824f, 0.359854f, 3.015789f, 3.219782f, 2.674188f, 0.672155f, 327.0996f, 87.6283f, 0.055452f, 0.0000f, 0.1000f},
};

/* bloub measured 32 segments against a 199px semi-major axis; the facet left
 * over scales with the radius, so at our ~70px rings 24 keeps it under a
 * third of a pixel. One ring's worth at a time, so nothing here needs a
 * kilobyte-scale buffer. */
#define ARC_SEGMENTS 24

void bloub_orbit_pose(const bloub_orbit_t* o, bloub_gaze_t rest, bloub_orbit_pose_t* out) {
    if (o == NULL || out == NULL) return;
    const float t = o->t;
    /* Measured: the spin ramps over 0.35s, then holds 1.25 turns a second,
     * counter-clockwise. */
    const float ramp = bloub_ease_in_out_cubic(bloub_clamp01(t / 0.35f));
    out->rot = -BLOUB_TAU * 1.25f * t * ramp;
    /* The eyes run round the sphere about three times faster than the body,
     * which is what stops it reading as a decal on a spinning ball. The swing
     * is centred on bloub's own resting gaze, not ours: theirs is a
     * three-quarter view, so the swing carries an eye fully round the back of
     * the head and the face reads as a sphere. Centred on our front-facing
     * rest instead, the eye only ever narrows and it reads as a wobble.
     * Our gaze is still where it lands - see the blend below. */
    out->gaze.yaw = BLOUB_REST_GAZE.yaw + sinf(t * 6.5f) * 65.0f;
    out->gaze.pitch = -4.0f;
    out->gaze.roll = -13.0f;
    out->eye_h = 0.34f;

    /* Handing back: bloub cross-fades a state's geometry into the next one on
     * an ease-out quint, and here the next one is the character at rest. */
    const float back = bloub_ease_out_quint(bloub_clamp01(o->exit));
    /* Settle upright the short way round. By now the body has turned through
     * several whole revolutions, and easing that raw total back to zero would
     * spin it backwards through every one of them - fast enough to alias into
     * a stutter rather than read as motion. Only the part that is not a whole
     * turn has to be given back, and dropping the whole turns changes nothing
     * on screen: an angle and that angle plus a full turn draw the same. */
    out->rot = fmodf(out->rot, BLOUB_TAU);
    if (out->rot > BLOUB_TAU * 0.5f) out->rot -= BLOUB_TAU;
    if (out->rot < -BLOUB_TAU * 0.5f) out->rot += BLOUB_TAU;
    out->rot = bloub_lerp(out->rot, 0.0f, back);
    out->gaze.yaw = bloub_lerp(out->gaze.yaw, rest.yaw, back);
    out->gaze.pitch = bloub_lerp(out->gaze.pitch, rest.pitch, back);
    out->gaze.roll = bloub_lerp(out->gaze.roll, rest.roll, back);
    out->eye_h = bloub_lerp(out->eye_h, 0.44f, back);
}

/* A stadium, prepared so the inside test needs no square root and only the
 * one-pixel edge band does. */
typedef struct {
    float x0, y0, dx, dy;
    float inv_l2;
    float r_in2, r_out, r_out2;
} capsule_t;

static void capsule_prepare(capsule_t* c, float x0, float y0, float x1, float y1, float r) {
    c->x0 = x0; c->y0 = y0;
    c->dx = x1 - x0; c->dy = y1 - y0;
    const float l2 = c->dx * c->dx + c->dy * c->dy;
    c->inv_l2 = l2 > 1e-9f ? 1.0f / l2 : 0.0f;
    const float r_in = r > 0.5f ? r - 0.5f : 0.0f;
    c->r_in2 = r_in * r_in;
    c->r_out = r + 0.5f;
    c->r_out2 = c->r_out * c->r_out;
}

/* 0 outside, 255 inside, and the pixel's own coverage across the edge. The
 * square root only runs in that one-pixel band. */
static inline int capsule_coverage(const capsule_t* c, float px, float py) {
    const float ax = px - c->x0, ay = py - c->y0;
    float t = (ax * c->dx + ay * c->dy) * c->inv_l2;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    const float qx = ax - t * c->dx, qy = ay - t * c->dy;
    const float q2 = qx * qx + qy * qy;
    if (q2 <= c->r_in2) return 255;
    if (q2 >= c->r_out2) return 0;
    return (int)((c->r_out - sqrtf(q2)) * 255.0f);
}

static void draw_capsule(uint16_t* buf, int w, int h, const capsule_t* c, float min_x, float max_x,
                         float min_y, float max_y, uint16_t color, uint32_t alpha) {
    int y0 = (int)floorf(min_y), y1 = (int)ceilf(max_y);
    if (y0 < 0) y0 = 0;
    if (y1 > h - 1) y1 = h - 1;
    int box_x0 = (int)floorf(min_x), box_x1 = (int)ceilf(max_x);
    if (box_x0 < 0) box_x0 = 0;
    if (box_x1 > w - 1) box_x1 = w - 1;
    if (y0 > y1 || box_x0 > box_x1) return;

    /* A diagonal stroke fills a sliver of its bounding box. Solving the
     * segment for the rows it actually crosses cuts the tested area by about
     * three times. */
    const bool steep = fabsf(c->dy) > 1e-3f;
    const float inv_dy = steep ? 1.0f / c->dy : 0.0f;
    for (int y = y0; y <= y1; y++) {
        const float fy = (float)y;
        int x0 = box_x0, x1 = box_x1;
        if (steep) {
            float ta = (fy - c->r_out - c->y0) * inv_dy, tb = (fy + c->r_out - c->y0) * inv_dy;
            if (ta > tb) { const float sw = ta; ta = tb; tb = sw; }
            ta = bloub_clamp01(ta); tb = bloub_clamp01(tb);
            float xa = c->x0 + ta * c->dx, xb = c->x0 + tb * c->dx;
            if (xa > xb) { const float sw = xa; xa = xb; xb = sw; }
            const int lo = (int)floorf(xa - c->r_out), hi = (int)ceilf(xb + c->r_out);
            if (lo > x0) x0 = lo;
            if (hi < x1) x1 = hi;
            if (x0 > x1) continue;
        }
        uint16_t* row = buf + (size_t)y * w;
        for (int x = x0; x <= x1; x++) {
            const int cov = capsule_coverage(c, (float)x, fy);
            if (cov == 0) continue;
            const uint32_t a = ((uint32_t)cov * alpha + 127u) / 255u;
            row[x] = a >= 255u ? color : bloub_blend565(row[x], color, a);
        }
    }
}

/* One ring's half. A ring is a circle on a tilted plane, so every sample
 * carries a z: a segment with z < 0 is on the far side of the body and has to
 * be painted before it. */
void bloub_arc_draw(uint16_t* buf, int w, int h, const bloub_arc_seed_t* seed, float t,
                    float scale_px, float ox, float oy, float opacity, bool behind) {
    if (buf == NULL || seed == NULL) return;
    if (opacity <= 0.02f) return;
    const uint32_t alpha = (uint32_t)(bloub_clamp01(opacity) * 255.0f + 0.5f);

    const float spin = seed->phase + t * seed->speed * BLOUB_TAU;
    const float cu = cosf(seed->tilt), su = sinf(seed->tilt);
    const float kz = sqrtf(fmaxf(0.0f, 1.0f - seed->k * seed->k));
    const float span = seed->sweep * BLOUB_TAU;

    float px[ARC_SEGMENTS + 1], py[ARC_SEGMENTS + 1], pz[ARC_SEGMENTS + 1];
    for (int i = 0; i <= ARC_SEGMENTS; i++) {
        const float th = spin + ((float)i / (float)ARC_SEGMENTS) * span;
        const float ct = cosf(th), st = sinf(th);
        px[i] = (seed->a * (ct * cu + st * (-su) * seed->k) + seed->cx) * scale_px + ox;
        py[i] = (seed->a * (ct * su + st * cu * seed->k) + seed->cy) * scale_px + oy;
        pz[i] = st * kz;
    }

    const float radius = (seed->width * scale_px) * 0.5f;
    for (int i = 0; i < ARC_SEGMENTS; i++) {
        const bool behind_a = pz[i] < 0.0f;
        if (behind_a != (pz[i + 1] < 0.0f)) continue;  /* bloub's own gap at the crossing */
        if (behind_a != behind) continue;
        capsule_t cap;
        capsule_prepare(&cap, px[i], py[i], px[i + 1], py[i + 1], radius);
        const float pad = cap.r_out;
        draw_capsule(buf, w, h, &cap, fminf(px[i], px[i + 1]) - pad, fmaxf(px[i], px[i + 1]) + pad,
                     fminf(py[i], py[i + 1]) - pad, fmaxf(py[i], py[i + 1]) + pad,
                     bloub_wheel565(seed->hue + seed->hue_span * ((float)i / (float)ARC_SEGMENTS)),
                     alpha);
    }
}

void bloub_orbit_draw(uint16_t* buf, int w, int h, const bloub_orbit_t* o, float scale_px,
                      float cx, float cy, bool behind) {
    if (buf == NULL || o == NULL) return;
    /* The rings come up together over 0.8s and enter one after another 0.13s
     * apart, and they leave as the state hands back. */
    const float leaving = 1.0f - bloub_clamp01(o->exit);
    const float fade = bloub_clamp01(o->t / 0.8f) * leaving;
    if (fade <= 0.02f) return;
    for (int i = 0; i < BLOUB_ORBIT_RINGS; i++) {
        bloub_arc_draw(buf, w, h, &BLOUB_RINGS[i], o->t, scale_px, cx, cy,
                       fade * bloub_clamp01((o->t - (float)i * 0.13f) / 0.3f), behind);
    }
}

void bloub_draw_dot(uint16_t* buf, int w, int h, float cx, float cy, float r, uint16_t color,
                    float opacity) {
    if (buf == NULL || r <= 0.0f || opacity <= 0.02f) return;
    /* A capsule with both ends in the same place is a disc, so the dots get
     * the arcs' anti-aliasing for free. */
    capsule_t cap;
    capsule_prepare(&cap, cx, cy, cx, cy, r);
    const float pad = cap.r_out;
    draw_capsule(buf, w, h, &cap, cx - pad, cx + pad, cy - pad, cy + pad, color,
                 (uint32_t)(bloub_clamp01(opacity) * 255.0f + 0.5f));
}
