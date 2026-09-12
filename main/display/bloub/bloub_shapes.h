#pragma once

/* These headers are included from C++ (the display layer), so the definitions
 * in bloub_face.c and bloub_shapes.c need C linkage to be found. */
#ifdef __cplusplus
extern "C" {
#endif
/* The eight silhouettes, as bloub measured them: 64 radial samples each, from
 * src/bot/skins.ts. MIT Licence, Copyright (c) 2026 Jeremy Perret - see
 * LICENSE beside this file. The numbers are the source's; do not round them. */
#define SHAPE_SAMPLES 64

#include <stdint.h>

#include "bloub_face.h"

/* A face, assembled: a silhouette, a head orientation, and two eyes. The eyes
 * are placed by projecting them onto the sphere (bloub_face.h), so they lean
 * and narrow with the head instead of sitting flat on it. */
typedef struct {
    const float* radii;          /* the shape profile, 64 samples */
    const bloub_gaze_t* gaze;
    float split;                 /* half the eye gap, degrees on the sphere */
    float scale;                 /* body radius, in pixels */
    float cx, cy;
    float sx, sy;                /* squash and stretch, screen frame */
    float rot;                   /* body rotation, radians */
    /* Per eye: size in body radii, its own tilt in degrees, and how open it is. */
    struct { float w, h, tilt, open; } eyes[2];
    float eye_alpha;             /* below 0.5 the eyes are not drawn at all */
} bloub_face_cfg_t;

/* Drawing. See bloub_shapes.c: the body is filled into a buffer of the panel's
 * own 16-bit words, then the eyes are erased back to `background`. */
void bloub_fill_shape(uint16_t* buf, int w, int h, const float* radii, float scale,
                      float cx, float cy, uint16_t color);
void bloub_punch_eye(uint16_t* buf, int w, int h, float cx, float cy, float ew, float eh,
                     float tilt_deg, uint16_t background);

/* The same punch, but the eye is drawn in its own tangent frame - the 2x2 that
 * bloub_eye_poses returns - so it leans and foreshortens with the sphere. */
void bloub_punch_eye_posed(uint16_t* buf, int w, int h, float cx, float cy, float hw,
                           float hh, float a, float b, float c, float d, float tilt_deg,
                           uint16_t background);

/** Body plus eyes in one call. */
void bloub_draw_face(uint16_t* buf, int w, int h, const bloub_face_cfg_t* f, uint16_t body,
                     uint16_t background);

typedef enum { SHAPE_CIRCLE, SHAPE_PEBBLE, SHAPE_SQUIRCLE, SHAPE_CAPSULE, SHAPE_TRIANGLE, SHAPE_HEXAGON, SHAPE_CLOUD, SHAPE_DROPLET, SHAPE_COUNT } shape_id_t;

static const float SHAPE_PROFILES[SHAPE_COUNT][SHAPE_SAMPLES] = {
  // Circle: constant radius 1.0 (neutral base shape).
  { 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f },
  // Pebble: circle warped by two low-frequency cosine harmonics, normalized to peak 1.02.
  { 0.9765f, 0.9614f, 0.9462f, 0.9319f, 0.9190f, 0.9083f, 0.8998f, 0.8937f, 0.8899f, 0.8881f, 0.8879f, 0.8889f, 0.8906f, 0.8927f, 0.8948f, 0.8967f, 0.8985f, 0.9002f, 0.9021f, 0.9044f, 0.9076f, 0.9118f, 0.9175f, 0.9248f, 0.9336f, 0.9439f, 0.9553f, 0.9672f, 0.9790f, 0.9900f, 0.9992f, 1.0059f, 1.0095f, 1.0092f, 1.0048f, 0.9963f, 0.9837f, 0.9675f, 0.9486f, 0.9278f, 0.9064f, 0.8856f, 0.8666f, 0.8506f, 0.8386f, 0.8315f, 0.8297f, 0.8333f, 0.8422f, 0.8559f, 0.8736f, 0.8942f, 0.9164f, 0.9391f, 0.9608f, 0.9804f, 0.9967f, 1.0091f, 1.0169f, 1.0200f, 1.0184f, 1.0126f, 1.0031f, 0.9908f },
  // Squircle: superellipse with exponent n=4.2, normalized to peak 1.15 (superellipse diagonal is its max radius).
  { 0.9591f, 0.9637f, 0.9776f, 1.0007f, 1.0321f, 1.0696f, 1.1080f, 1.1383f, 1.1500f, 1.1383f, 1.1080f, 1.0696f, 1.0321f, 1.0007f, 0.9776f, 0.9637f, 0.9591f, 0.9637f, 0.9776f, 1.0007f, 1.0321f, 1.0696f, 1.1080f, 1.1383f, 1.1500f, 1.1383f, 1.1080f, 1.0696f, 1.0321f, 1.0007f, 0.9776f, 0.9637f, 0.9591f, 0.9637f, 0.9776f, 1.0007f, 1.0321f, 1.0696f, 1.1080f, 1.1383f, 1.1500f, 1.1383f, 1.1080f, 1.0696f, 1.0321f, 1.0007f, 0.9776f, 0.9637f, 0.9591f, 0.9637f, 0.9776f, 1.0007f, 1.0321f, 1.0696f, 1.1080f, 1.1383f, 1.1500f, 1.1383f, 1.1080f, 1.0696f, 1.0321f, 1.0007f, 0.9776f, 0.9637f },
  // Capsule: convex hull of two same-size circles (stadium shape), radii cast from polygon, no renormalization.
  { 1.0400f, 1.0363f, 1.0265f, 1.0095f, 0.9868f, 0.9576f, 0.9235f, 0.8842f, 0.8409f, 0.7945f, 0.7457f, 0.7030f, 0.6711f, 0.6479f, 0.6321f, 0.6230f, 0.6200f, 0.6230f, 0.6321f, 0.6479f, 0.6711f, 0.7030f, 0.7457f, 0.7945f, 0.8409f, 0.8842f, 0.9235f, 0.9576f, 0.9868f, 1.0095f, 1.0265f, 1.0363f, 1.0400f, 1.0363f, 1.0265f, 1.0095f, 0.9868f, 0.9576f, 0.9235f, 0.8842f, 0.8409f, 0.7945f, 0.7457f, 0.7030f, 0.6711f, 0.6479f, 0.6321f, 0.6230f, 0.6200f, 0.6230f, 0.6321f, 0.6479f, 0.6711f, 0.7030f, 0.7457f, 0.7945f, 0.8409f, 0.8842f, 0.9235f, 0.9576f, 0.9868f, 1.0095f, 1.0265f, 1.0363f },
  // Triangle: regular 3-gon with rounded corners (corner radius 0.34), point-up (-90deg rotation since y is down).
  { 0.8429f, 0.8981f, 0.9710f, 1.0481f, 1.0972f, 1.1168f, 1.1142f, 1.0831f, 1.0250f, 0.9444f, 0.8780f, 0.8277f, 0.7901f, 0.7628f, 0.7443f, 0.7335f, 0.7300f, 0.7335f, 0.7443f, 0.7628f, 0.7901f, 0.8277f, 0.8780f, 0.9444f, 1.0250f, 1.0831f, 1.1142f, 1.1168f, 1.0972f, 1.0481f, 0.9710f, 0.8981f, 0.8429f, 0.8014f, 0.7709f, 0.7496f, 0.7363f, 0.7304f, 0.7316f, 0.7399f, 0.7558f, 0.7800f, 0.8139f, 0.8596f, 0.9201f, 0.9991f, 1.0679f, 1.1057f, 1.1200f, 1.1057f, 1.0679f, 0.9991f, 0.9201f, 0.8596f, 0.8139f, 0.7800f, 0.7558f, 0.7399f, 0.7316f, 0.7304f, 0.7363f, 0.7496f, 0.7709f, 0.8014f },
  // Hexagon: regular 6-gon with rounded corners (corner radius 0.26), flat top/bottom (0deg rotation).
  { 1.0400f, 1.0245f, 0.9879f, 0.9606f, 0.9436f, 0.9360f, 0.9375f, 0.9482f, 0.9685f, 0.9996f, 1.0329f, 1.0381f, 1.0126f, 0.9776f, 0.9538f, 0.9400f, 0.9355f, 0.9400f, 0.9538f, 0.9776f, 1.0126f, 1.0381f, 1.0329f, 0.9996f, 0.9685f, 0.9482f, 0.9375f, 0.9360f, 0.9436f, 0.9606f, 0.9879f, 1.0245f, 1.0400f, 1.0245f, 0.9879f, 0.9606f, 0.9436f, 0.9360f, 0.9375f, 0.9482f, 0.9685f, 0.9996f, 1.0329f, 1.0381f, 1.0126f, 0.9776f, 0.9538f, 0.9400f, 0.9355f, 0.9400f, 0.9538f, 0.9776f, 1.0126f, 1.0381f, 1.0329f, 0.9996f, 0.9685f, 0.9482f, 0.9375f, 0.9360f, 0.9436f, 0.9606f, 0.9879f, 1.0245f },
  // Cloud: union of 5 offset circles (radial max of each), normalized to peak 1.02, giving lobed silhouette.
  { 0.9157f, 0.9505f, 0.9760f, 0.9922f, 0.9987f, 0.9956f, 0.9829f, 0.9606f, 0.9291f, 0.8886f, 0.8395f, 0.8587f, 0.8746f, 0.8866f, 0.8944f, 0.8980f, 0.8972f, 0.8921f, 0.8828f, 0.8694f, 0.8522f, 0.8346f, 0.8845f, 0.9273f, 0.9626f, 0.9897f, 1.0085f, 1.0186f, 1.0200f, 1.0126f, 0.9964f, 0.9718f, 0.9390f, 0.8985f, 0.8507f, 0.7964f, 0.7772f, 0.8064f, 0.8297f, 0.8469f, 0.8576f, 0.8618f, 0.8593f, 0.8502f, 0.8347f, 0.8129f, 0.7851f, 0.7519f, 0.7137f, 0.6712f, 0.6560f, 0.6974f, 0.7336f, 0.7641f, 0.7885f, 0.8064f, 0.8175f, 0.8219f, 0.8193f, 0.8098f, 0.7936f, 0.7709f, 0.8201f, 0.8721f },
  // Droplet: convex hull of a big circle (bottom) and a tiny circle (top point), normalized to peak 1.04.
  { 0.6151f, 0.6442f, 0.6737f, 0.7041f, 0.7351f, 0.7658f, 0.7954f, 0.8249f, 0.8515f, 0.8773f, 0.8995f, 0.9201f, 0.9363f, 0.9499f, 0.9596f, 0.9653f, 0.9679f, 0.9653f, 0.9596f, 0.9499f, 0.9363f, 0.9201f, 0.8995f, 0.8773f, 0.8515f, 0.8249f, 0.7954f, 0.7658f, 0.7351f, 0.7041f, 0.6737f, 0.6442f, 0.6151f, 0.5879f, 0.5661f, 0.5510f, 0.5418f, 0.5379f, 0.5393f, 0.5459f, 0.5582f, 0.5766f, 0.6024f, 0.6370f, 0.6827f, 0.7433f, 0.8244f, 0.9354f, 1.0400f, 0.9354f, 0.8244f, 0.7433f, 0.6827f, 0.6370f, 0.6024f, 0.5766f, 0.5582f, 0.5459f, 0.5393f, 0.5379f, 0.5418f, 0.5510f, 0.5661f, 0.5879f },
};

static inline const char *shape_name(shape_id_t s) {
  switch (s) {
    case SHAPE_CIRCLE: return "CIRCLE";
    case SHAPE_PEBBLE: return "PEBBLE";
    case SHAPE_SQUIRCLE: return "SQUIRCLE";
    case SHAPE_CAPSULE: return "CAPSULE";
    case SHAPE_TRIANGLE: return "TRIANGLE";
    case SHAPE_HEXAGON: return "HEXAGON";
    case SHAPE_CLOUD: return "CLOUD";
    case SHAPE_DROPLET: return "DROPLET";
    default: return "UNKNOWN";
  }
}

#ifdef __cplusplus
}
#endif
