#pragma once

/* The small maths bloub builds everything on, from src/bot/math.ts.
 * MIT Licence, Copyright (c) 2026 Jeremy Perret - see LICENSE. */

#include <math.h>
#include <stdint.h>

#define BLOUB_TAU 6.28318530717958647692f

static inline float bloub_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float bloub_clamp01(float v) { return bloub_clampf(v, 0.0f, 1.0f); }
static inline float bloub_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* Easing. The source measured the reference video and found the body never
 * overshoots, so these are all ease-outs with no spring anywhere. */
static inline float bloub_ease_out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
static inline float bloub_ease_in_out_cubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - (u * u * u) / 2.0f;
}
static inline float bloub_ease_out_quint(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

/* Periodic 1D noise, seamless over `period`, three octaves. The gaze drift
 * runs on this with prime-ish periods so the wander never visibly repeats. */
static inline float bloub_loop_noise(float t, float period, float seed) {
    const float p = (t / period) * BLOUB_TAU;
    return 0.55f * sinf(p + seed)
         + 0.30f * sinf(2.0f * p + seed * 1.7f + 1.1f)
         + 0.15f * sinf(3.0f * p + seed * 2.3f + 2.4f);
}

/* mulberry32, the source's createRng. Same sequence as the browser's, which is
 * what makes a blink schedule identical on both. */
static inline float bloub_rng(uint32_t* state) {
    *state += 0x6d2b79f5u;
    uint32_t t = *state;
    t = (t ^ (t >> 15)) * (1u | t);
    t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
    return (float)((t ^ (t >> 14)) / 4294967296.0);
}
