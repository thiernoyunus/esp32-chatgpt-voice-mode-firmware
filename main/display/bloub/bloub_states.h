#pragma once

/* The states the character can be in, and the cross-fade between them.
 *
 * Ported from bloub's src/bot/states.ts (MIT Licence, Copyright (c) 2026
 * Jeremy Perret - see LICENSE beside this file), by way of
 * atq-ren/esp32-robot-face's C arrangement of the same catalogue. Every
 * number is a measurement off bloub's reference video, not a setting.
 *
 * Only the states that read as "working" are here. bloub's alert, notify and
 * sleep say something specific - a problem, a notification, asleep - and
 * showing them while the agent is busy would be telling the user something
 * untrue, so they wait until there is a real trigger. The ones that are here
 * are also, as it happens, exactly the ones that can be cut off at any moment:
 * bloub records a minimum duration for the others, below which "the ! does not
 * come back, the body stays exploded", and an answer arrives when it arrives. */

#ifdef __cplusplus
extern "C" {
#endif

#include "bloub_decor.h"
#include "bloub_face.h"
#include "bloub_shapes.h"

typedef enum {
    BLOUB_STATE_IDLE = 0,
    BLOUB_STATE_THINKING,
    BLOUB_STATE_WINK,
    BLOUB_STATE_WIDE,
    BLOUB_STATE_PLAY,
    BLOUB_STATE_COUNT,
} bloub_state_id_t;

#define BLOUB_POSE_MAX_DOTS 4
#define BLOUB_POSE_MAX_ARCS 8

/* A dot, in body-radius units relative to the body's centre. */
typedef struct {
    float x, y, r, opacity;
} bloub_dot_t;

/* Everything one moment of one state needs drawn. Lengths are in body-radius
 * units so the whole pose scales with the character. */
typedef struct {
    float radii[SHAPE_SAMPLES];  /* the silhouette */
    float rot, cx, cy, sx, sy;
    bloub_gaze_t gaze;
    float split;
    struct { float w, h, open; } eyes[2];
    float eye_alpha;             /* below 0.5 the eyes are not drawn */
    int dot_count;
    bloub_dot_t dots[BLOUB_POSE_MAX_DOTS];
    int arc_count;
    bloub_arc_seed_t arcs[BLOUB_POSE_MAX_ARCS];
    float arc_opacity[BLOUB_POSE_MAX_ARCS];
    float arc_t;
} bloub_pose_t;

/* State `id` at its own elapsed time `t`. `body` is the silhouette the watch
 * is set to - the states that do not draw their own shape wear it, which is
 * how the wearer's choice survives the cycle. */
void bloub_pose_sample(bloub_state_id_t id, float t, const float* body, bloub_pose_t* out);

/* Cross-fades two poses, `t` running 0 to 1 from `a` to `b`. The body morphs
 * because every silhouette is sampled at the same 64 angles, so a shape change
 * is a plain blend of radii - which is the whole reason bloub stores bodies
 * this way. Dots and arcs cross-fade on opacity instead, both states' carried
 * at once, rather than trying to interpolate between different counts. */
void bloub_pose_blend(const bloub_pose_t* a, const bloub_pose_t* b, float t, bloub_pose_t* out);

/* How long the state is held, and how long the fade into it takes, in seconds.
 * Both measured. */
float bloub_state_duration(bloub_state_id_t id);
float bloub_state_morph(bloub_state_id_t id);

/* Draws a pose's dots and arcs. Arcs come in two passes around the body, the
 * same as the rings: behind, then the body, then in front. */
void bloub_pose_draw_arcs(uint16_t* buf, int w, int h, const bloub_pose_t* p, float scale_px,
                          float cx, float cy, bool behind);
void bloub_pose_draw_dots(uint16_t* buf, int w, int h, const bloub_pose_t* p, float scale_px,
                          float cx, float cy, uint16_t color);

#ifdef __cplusplus
}
#endif
