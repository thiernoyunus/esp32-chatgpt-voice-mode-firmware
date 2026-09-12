/* Checks the ported character maths against what the source says they are.
 *
 *   cc -I main/display/bloub -o /tmp/bloub_face_check \
 *      scripts/tests/bloub_face_check.c main/display/bloub/bloub_face.c -lm
 *   /tmp/bloub_face_check
 *
 * No LVGL: this is the geometry and the clock, not the drawing. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "bloub_face.h"

int main(void) {
    /* 1. The measured constants survive the trip. */
    assert(fabsf(BLOUB_EYE_SPLIT - 15.46f) < 1e-4f);
    assert(fabsf(BLOUB_EYE_W - 0.186f) < 1e-4f);
    assert(fabsf(BLOUB_EYE_H - 0.412f) < 1e-4f);
    assert(fabsf(BLOUB_REST_GAZE.yaw - 28.49f) < 1e-4f);
    assert(fabsf(BLOUB_REST_GAZE.pitch - 28.62f) < 1e-4f);
    assert(fabsf(BLOUB_REST_GAZE.roll + 13.0f) < 1e-4f);

    /* 2. The sphere does the work. At the resting gaze the two eyes must NOT
     *    be mirror images of each other - the near-edge eye is foreshortened
     *    and leans, and that is the whole reason to project instead of drawing
     *    two flat ellipses. Both stay in front of the viewer. */
    bloub_eye_t eyes[2];
    bloub_eye_poses(BLOUB_REST_GAZE, 190.0f, BLOUB_EYE_SPLIT, eyes);
    assert(eyes[0].depth > 0.0f && eyes[1].depth > 0.0f);
    assert(fabsf(eyes[0].depth - eyes[1].depth) > 0.2f);
    /* Each tangent axis is a unit vector in 3D, so what lands on screen is a
     * projection of it: shorter than one whenever the axis leans away from the
     * viewer, which is exactly the foreshortening the eyes inherit. */
    for (int i = 0; i < 2; i++) {
        const float n = sqrtf(eyes[i].a * eyes[i].a + eyes[i].b * eyes[i].b);
        assert(n > 0.5f && n <= 1.001f);
    }
    printf("rest gaze: depths %.3f / %.3f, centres (%.1f, %.1f) (%.1f, %.1f)\n",
           eyes[0].depth, eyes[1].depth, eyes[0].x, eyes[0].y, eyes[1].x, eyes[1].y);

    /* 3. Nothing blinks before the first scheduled one, and the lid never
     *    leaves 0..1 nor stays shut. */
    assert(bloub_blink_lid(0.0f) == 1.0f);
    assert(bloub_blink_lid(1.39f) == 1.0f);
    assert(bloub_blink_lid(1.45f) < 0.6f);
    float lowest = 1.0f;
    for (float t = 0.0f; t < 30.0f; t += 0.001f) {
        const float lid = bloub_blink_lid(t);
        assert(lid >= 0.0f && lid <= 1.0f);
        if (lid < lowest) lowest = lid;
    }
    assert(lowest < 0.01f);
    assert(fabsf(bloub_blink_scale(1.0f) - 1.0f) < 1e-6f);
    assert(bloub_blink_scale(0.0f) > 0.05f && bloub_blink_scale(0.0f) < 0.10f);

    /* 4. The idle life is bounded and never still, and it is a pure function
     *    of time - the same date always gives the same face. */
    float lo = 1e9f, hi = -1e9f;
    for (float t = 0.0f; t < 60.0f; t += 0.01f) {
        const bloub_liveliness_t l = bloub_liveliness(t, 1.0f, true, true);
        if (l.d_yaw < lo) lo = l.d_yaw;
        if (l.d_yaw > hi) hi = l.d_yaw;
        assert(l.breath > 0.99f && l.breath < 1.01f);
        assert(l.drift_x > -0.01f && l.drift_x < 0.01f);
    }
    assert(hi - lo > 3.0f && hi - lo < 14.0f);
    const bloub_liveliness_t a = bloub_liveliness(2.5f, 1.0f, true, true);
    const bloub_liveliness_t b = bloub_liveliness(2.5f, 1.0f, true, true);
    assert(a.d_yaw == b.d_yaw && a.lid == b.lid && a.breath == b.breath);
    /* Wander 0 kills the drift but not the blink: a state that hides the face
     * still gets its lids. */
    const bloub_liveliness_t still = bloub_liveliness(2.5f, 0.0f, true, false);
    assert(still.d_yaw == 0.0f && still.drift_x == 0.0f && still.breath == 1.0f);

    printf("ok: 2 eyes, gaze swing %.1f deg, lid floor %.2f\n",
           hi - lo, bloub_blink_scale(0.0f));
    return 0;
}
