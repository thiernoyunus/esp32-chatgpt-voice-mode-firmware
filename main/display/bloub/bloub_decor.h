#pragma once

/* bloub's `orbit`: rings running round the character while the character
 * itself turns and its eyes whip round the sphere to keep up.
 *
 * The ring seeds and every timing here are bloub's, from src/bot/decor.ts and
 * src/bot/states.ts (MIT Licence, Copyright (c) 2026 Jeremy Perret - see
 * LICENSE beside this file). The rasterizer - rings as chains of capsule
 * stamps, each knowing whether it passes in front of the body or behind it -
 * follows atq-ren/esp32-robot-face, which solved this on an ESP32 first. The
 * stroke is anti-aliased here, which theirs does not need: their rings are
 * 7px wide on a 466px panel, ours are under 3px on a 166px canvas, and a hard
 * edge at that width reads as a broken line rather than a thin one.
 *
 * What is ours: the body keeps whatever shape and colour the watch is set to.
 * bloub's orbit spins a triangle that relaxes into a ball, and the ball is its
 * resting body - here the chosen shape is the resting body, so it is the
 * chosen shape that turns. */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "bloub_face.h"

/* One ring: a circle on a plane tilted in space. Everything is in ball-radius
 * units, so the whole set scales with the character. */
typedef struct {
    float a;        /* semi-major axis */
    float k;        /* flattening, b/a - how edge-on the ring is */
    float tilt;     /* the major axis's angle on screen, radians */
    float speed;    /* turns per second */
    float phase;
    float sweep;    /* how much of a full turn is actually drawn */
    float hue;
    float hue_span; /* the hue shifts along the arc's length */
    float width;    /* stroke width */
    float cx, cy;
} bloub_arc_seed_t;

/* The six measured rings. */
#define BLOUB_ORBIT_RINGS 6
extern const bloub_arc_seed_t BLOUB_RINGS[BLOUB_ORBIT_RINGS];

/* Where orbit is. `t` is seconds since the rings appeared - the spin, the
 * eyes and the rings' entrance all run on it. `exit` goes 0 to 1 as the state
 * hands back to the resting character; bloub's own fixed block becomes an
 * entrance, an open-ended hold while the call connects, and that handover. */
typedef struct {
    float t;
    float exit;
} bloub_orbit_t;

/* What orbit does to the body and the face at this moment. */
typedef struct {
    float rot;              /* body rotation, radians */
    bloub_gaze_t gaze;      /* absolute, already blended back towards rest */
    float eye_h;            /* eye height, ball-radius units */
} bloub_orbit_pose_t;

/* `rest` is the gaze the character holds when it is not doing anything, and
 * what the pose returns to as `exit` runs. */
void bloub_orbit_pose(const bloub_orbit_t* o, bloub_gaze_t rest, bloub_orbit_pose_t* out);

/* Draws the half of the rings that passes behind the body, or the half in
 * front of it. Call the behind half, then the face, then the front half -
 * that is what makes a ring go round the character rather than sit on it.
 * `scale_px` is the body radius in pixels. */
void bloub_orbit_draw(uint16_t* buf, int w, int h, const bloub_orbit_t* o, float scale_px,
                      float cx, float cy, bool behind);

/* One arc, at its own time and opacity - the same primitive the rings are
 * made of, which the states reuse for play's swoosh. Same two passes: the
 * half behind the body, then the body, then the half in front. */
void bloub_arc_draw(uint16_t* buf, int w, int h, const bloub_arc_seed_t* seed, float t,
                    float scale_px, float cx, float cy, float opacity, bool behind);

/* A filled dot, anti-aliased and faded the same way an arc is. thinking's
 * three dots are drawn with it. */
void bloub_draw_dot(uint16_t* buf, int w, int h, float cx, float cy, float r, uint16_t color,
                    float opacity);

#ifdef __cplusplus
}
#endif
