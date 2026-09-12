/* Silhouettes and the two primitives the face is drawn with.
 *
 * Ported from bloub (github.com/jeremy-prt/bloub), MIT Licence,
 * Copyright (c) 2026 Jeremy Perret - see LICENSE beside this file.
 *
 * The body is a filled 64-gon, not a per-pixel test against the radial
 * profile. That test is the same picture and about 66k transcendental calls a
 * frame, which is what made the first naive version unusable - the ESP32 port
 * measured it, and this is the shape they landed on too: per row, a handful of
 * edge crossings and one flat span write.
 *
 * The eyes are HOLES, not shapes laid on top. Filling the body first and then
 * erasing the eye interiors means an eye that slides towards the edge clips
 * itself against the silhouette with no clipping code at all - there is simply
 * nothing left to erase outside the body. It is also what bloub's SVG mask
 * does, and why they say their eyes never need cropping.
 *
 * Colours are the panel's own 16-bit words, background included, so nothing
 * here has to know how they were packed. */

#include "bloub_shapes.h"

#include "bloub_math.h"
#include "bloub_face.h"

#include <math.h>
#include <stddef.h>

typedef struct { float x, y; } pt_t;

/* Scanline fill of a closed polygon: per row, a handful of edge crossings and
 * one flat span write. Shared by the plain silhouette and the posed face. */
static void fill_poly(uint16_t* buf, int w, int h, const pt_t* pts, uint16_t color) {
    for (int y = 0; y < h; y++) {
        const float sy = (float)y + 0.5f;
        float xs[SHAPE_SAMPLES];
        int n = 0;
        for (int i = 0; i < SHAPE_SAMPLES; i++) {
            const pt_t a = pts[i], b = pts[(i + 1) % SHAPE_SAMPLES];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy))
                xs[n++] = a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x);
        }
        for (int i = 1; i < n; i++) {
            const float v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int k = 0; k + 1 < n; k += 2) {
            int x0 = (int)ceilf(xs[k] - 0.5f), x1 = (int)floorf(xs[k + 1] - 0.5f);
            if (x0 < 0) x0 = 0;
            if (x1 > w - 1) x1 = w - 1;
            uint16_t* row = buf + (size_t)y * w;
            for (int x = x0; x <= x1; x++) row[x] = color;
        }
    }
}

void bloub_fill_shape(uint16_t* buf, int w, int h, const float* radii, float scale,
                      float cx, float cy, uint16_t color) {
    if (buf == NULL || radii == NULL || w <= 0 || h <= 0) return;

    pt_t pts[SHAPE_SAMPLES];
    for (int i = 0; i < SHAPE_SAMPLES; i++) {
        const float a = (float)i / SHAPE_SAMPLES * BLOUB_TAU;
        pts[i].x = cx + cosf(a) * radii[i] * scale;
        pts[i].y = cy + sinf(a) * radii[i] * scale;
    }

    for (int y = 0; y < h; y++) {
        const float sy = (float)y + 0.5f;
        float xs[SHAPE_SAMPLES];
        int n = 0;
        for (int i = 0; i < SHAPE_SAMPLES; i++) {
            const pt_t a = pts[i], b = pts[(i + 1) % SHAPE_SAMPLES];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy))
                xs[n++] = a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x);
        }
        for (int i = 1; i < n; i++) {          /* a handful of crossings */
            const float v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int k = 0; k + 1 < n; k += 2) {
            int x0 = (int)ceilf(xs[k] - 0.5f), x1 = (int)floorf(xs[k + 1] - 0.5f);
            if (x0 < 0) x0 = 0;
            if (x1 > w - 1) x1 = w - 1;
            uint16_t* row = buf + (size_t)y * w;
            for (int x = x0; x <= x1; x++) row[x] = color;
        }
    }
}

void bloub_punch_eye(uint16_t* buf, int w, int h, float cx, float cy, float ew, float eh,
                     float tilt_deg, uint16_t background) {
    const float rad = tilt_deg * BLOUB_TAU / 360.0f;
    /* A square tangent frame: this entry point only tilts. */
    bloub_punch_eye_posed(buf, w, h, cx, cy, ew * 0.5f, eh * 0.5f,
                          cosf(rad), sinf(rad), -sinf(rad), cosf(rad), 0.0f, background);
}

void bloub_punch_eye_posed(uint16_t* buf, int w, int h, float cx, float cy, float hw,
                           float hh, float a, float b, float c, float d, float tilt_deg,
                           uint16_t background) {
    if (buf == NULL || w <= 0 || h <= 0 || hw <= 0.0f || hh <= 0.0f) return;
    /* The tangent frame is a projection of an orthonormal 3D frame, so its
     * determinant IS the eye's depth. Inverting it divides by that depth, and
     * the division is the foreshortening: the far eye comes out narrower and
     * leaning without anyone coding either. */
    const float det = a * d - b * c;
    if (fabsf(det) < 1e-4f) return;
    const float rad = tilt_deg * BLOUB_TAU / 360.0f;
    const float ct = cosf(rad), st = sinf(rad);
    const float rr = hw < hh ? hw : hh;        /* corner radius: a capsule */
    const float ix = hw - rr, iy = hh - rr;

    /* Only the eye's own bounding box is touched: an eye is a few hundred
     * pixels, the body it sits in a few thousand.
     *
     * The box comes from the FORWARD transform - the frame maps eye space to
     * screen as (dx, dy) = (a*u + b*v, c*u + d*v), so the widest the eye can
     * reach is |a|*hw + |b|*hh across and |c|*hw + |d|*hh down. Dividing by
     * det instead inverted the relationship: as an eye turns edge-on and det
     * falls towards zero the box grew without bound, and at the shallowest
     * angle still drawn it was scanning most of the canvas for an eye two
     * pixels wide. The orbit animation turns the head far enough to reach
     * exactly those poses. */
    const float ex = fabsf(a) * hw + fabsf(b) * hh;
    const float ey = fabsf(c) * hw + fabsf(d) * hh;
    int x0 = (int)floorf(cx - ex), x1 = (int)ceilf(cx + ex);
    int y0 = (int)floorf(cy - ey), y1 = (int)ceilf(cy + ey);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;

    for (int y = y0; y <= y1; y++) {
        uint16_t* row = buf + (size_t)y * w;
        for (int x = x0; x <= x1; x++) {
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float u0 = (d * dx - b * dy) / det;
            const float v0 = (-c * dx + a * dy) / det;
            const float u = fabsf(u0 * ct + v0 * st);
            const float v = fabsf(-u0 * st + v0 * ct);
            if (u <= ix && v <= hh) { row[x] = background; continue; }
            if (v <= iy && u <= hw) { row[x] = background; continue; }
            const float qx = u - ix, qy = v - iy;
            if (qx * qx + qy * qy <= rr * rr) row[x] = background;
        }
    }
}

void bloub_draw_face(uint16_t* buf, int w, int h, const bloub_face_cfg_t* f, uint16_t body,
                     uint16_t background) {
    if (buf == NULL || f == NULL || f->radii == NULL || f->gaze == NULL) return;

    /* 1. The silhouette, squashed and rotated. */
    const float cr = cosf(f->rot), sr = sinf(f->rot);
    pt_t pts[SHAPE_SAMPLES];
    for (int i = 0; i < SHAPE_SAMPLES; i++) {
        const float ang = (float)i / SHAPE_SAMPLES * BLOUB_TAU;
        const float rx = cosf(ang) * f->radii[i] * f->scale * f->sx;
        const float ry = sinf(ang) * f->radii[i] * f->scale * f->sy;
        pts[i].x = f->cx + rx * cr - ry * sr;
        pts[i].y = f->cy + rx * sr + ry * cr;
    }
    fill_poly(buf, w, h, pts, body);

    /* 2. The eyes, as holes, in their own tangent frames. */
    if (f->eye_alpha < 0.5f) return;
    bloub_eye_t pose[2];
    bloub_eye_poses(*f->gaze, f->scale, f->split, pose);
    for (int e = 0; e < 2; e++) {
        /* An eye that has gone round the back of the sphere is not drawn. It
         * only matters once the gaze swings far - at rest both eyes face the
         * viewer - but without it a big turn punches the far eye back through
         * the front of the head. */
        if (pose[e].depth <= 0.02f) continue;
        bloub_punch_eye_posed(buf, w, h, f->cx + pose[e].x * f->sx,
                              f->cy + pose[e].y * f->sy,
                              f->eyes[e].w * 0.5f * f->scale,
                              f->eyes[e].h * f->eyes[e].open * 0.5f * f->scale,
                              pose[e].a, pose[e].b, pose[e].c, pose[e].d, f->eyes[e].tilt,
                              background);
    }
}
