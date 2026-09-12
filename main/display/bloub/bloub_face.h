#pragma once

/* These headers are included from C++ (the display layer), so the definitions
 * in bloub_face.c and bloub_shapes.c need C linkage to be found. */
#ifdef __cplusplus
extern "C" {
#endif

/* Eyes on a sphere, and the idle life. Ported from bloub src/bot/face.ts.
 * MIT Licence, Copyright (c) 2026 Jeremy Perret - see LICENSE. */

#include <stdbool.h>

#include "bloub_math.h"

/* Head orientation in degrees. Positive yaw looks right, positive pitch looks
 * up, roll tilts the head. */
typedef struct {
    float yaw, pitch, roll;
} bloub_gaze_t;

/* One eye, projected. (x, y) is its centre in screen pixels at the given scale;
 * (a,b,c,d) is the tangent frame the eye is drawn in, SVG matrix order; depth is
 * the z of its outward normal, so a positive depth faces the viewer. */
typedef struct {
    float x, y;
    float a, b, c, d;
    float depth;
} bloub_eye_t;

/* Measured, not chosen: the two eyes sit ~31 degrees apart on the sphere and
 * the resting gaze came out of a fit with ~1px residual on a 190px radius. */
#define BLOUB_EYE_SPLIT 15.46f
#define BLOUB_EYE_W 0.186f
#define BLOUB_EYE_H 0.412f

extern const bloub_gaze_t BLOUB_REST_GAZE;

/** Fills out[2] with the inner (0) and outer (1) eye. */
void bloub_eye_poses(bloub_gaze_t gaze, float scale, float split, bloub_eye_t out[2]);

/* Idle life at global time t, a pure function of t like everything else here.
 * `wander` scales the gaze drift, `blink` and `enable_float` gate the blink
 * schedule and the drift/breath - the caller turns them off while a state hides
 * the face, which is what the source's `alive` flag does. */
typedef struct {
    float d_yaw, d_pitch, d_roll;
    float lid;        /* 1 = open, 0 = shut */
    float drift_x, drift_y;
    float breath;     /* ~1; a tiny vertical modulation so the body is never still */
} bloub_liveliness_t;

bloub_liveliness_t bloub_liveliness(float t, float wander, bool blink, bool enable_float);

/** Vertical eye scale for a lid value: 1 open, 0.06 shut, never zero. */
float bloub_blink_scale(float lid);

/** The bare lid for `t`, for callers that only want the blink. */
float bloub_blink_lid(float t);

#ifdef __cplusplus
}
#endif
