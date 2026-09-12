/* Checks the connecting animation: that the rings stay on the canvas, that
 * they pass both behind the body and in front of it, that the body turns and
 * the eyes run round the sphere with it, and that the handover lands back on
 * the resting character with a clean frame.
 *
 *   cc -I main/display/bloub -o /tmp/bloub_orbit_check \\
 *      scripts/tests/bloub_orbit_check.c main/display/bloub/bloub_decor.c \\
 *      main/display/bloub/bloub_shapes.c main/display/bloub/bloub_face.c -lm
 *
 * No LVGL, no display: a buffer of panel words and the arithmetic. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bloub_decor.h"
#include "bloub_shapes.h"

#define S 166
/* The call screen's own numbers, from RenderVoiceOrb. */
#define BALL ((float)S * 0.46f * 0.68f)
#define MID ((float)S * 0.5f)

static uint16_t buf[S * S];
static const bloub_gaze_t REST = {4.0f, 5.0f, -4.0f};

static int painted(void) {
    int n = 0;
    for (int i = 0; i < S * S; i++) n += buf[i] != 0;
    return n;
}

static void rings(float t, float exit, bool behind) {
    const bloub_orbit_t o = {t, exit};
    memset(buf, 0, sizeof(buf));
    bloub_orbit_draw(buf, S, S, &o, BALL, MID, MID, behind);
}

int main(void) {
    /* The rings come up over 0.8s and enter 0.13s apart, so a late frame has
     * far more ring on it than an early one. */
    rings(0.10f, 0.0f, false);
    const int early = painted();
    rings(1.20f, 0.0f, false);
    const int all_up = painted();
    assert(early > 0 && all_up > early * 2);

    for (float t = 1.2f; t < 5.0f; t += 0.033f) {
        /* Nothing reaches the canvas edge. The rings run to ~1.4 ball radii,
         * so a clipped ring is the failure this screen is most likely to hit. */
        for (int pass = 0; pass < 2; pass++) {
            rings(t, 0.0f, pass == 0);
            for (int x = 0; x < S; x++) assert(buf[x] == 0 && buf[(S - 1) * S + x] == 0);
            for (int y = 0; y < S; y++) assert(buf[y * S] == 0 && buf[y * S + S - 1] == 0);
            /* Both halves carry ring on every frame: that split is what makes
             * them orbit the character rather than sit behind it like a halo. */
            assert(painted() > 0);
        }
    }

    /* The handover empties the canvas, so the resting character is handed a
     * clean frame rather than one with a ghost ring on it. */
    rings(2.0f, 1.0f, true);
    assert(painted() == 0);
    rings(2.0f, 1.0f, false);
    assert(painted() == 0);

    /* The body turns, and the eyes run round the sphere faster than it does -
     * measured at 1.25 turns a second against a 6.5 rad/s gaze swing. */
    /* The angle is kept wrapped into one turn - see bloub_orbit_pose - so a
     * whole revolution shows up as the wrapped value sweeping its full range,
     * not as a total that keeps growing. At 1.25 turns a second, one second
     * has to visit both ends of it. */
    float rot_lo = 1e9f, rot_hi = -1e9f;
    for (float t = 1.0f; t < 2.0f; t += 0.01f) {
        bloub_orbit_pose_t p;
        const bloub_orbit_t s = {t, 0.0f};
        bloub_orbit_pose(&s, REST, &p);
        if (p.rot < rot_lo) rot_lo = p.rot;
        if (p.rot > rot_hi) rot_hi = p.rot;
    }
    assert(rot_hi - rot_lo > 6.0f);
    /* And it never jumps a whole turn at once: unwinding an accumulated angle
     * to zero is what made the settle stutter, so the wrap has to keep every
     * step small. */
    bloub_orbit_pose_t prev;
    const bloub_orbit_t t0 = {1.0f, 0.0f};
    bloub_orbit_pose(&t0, REST, &prev);
    for (float t = 1.0f + 0.033f; t < 3.0f; t += 0.033f) {
        bloub_orbit_pose_t p;
        const bloub_orbit_t s = {t, 0.0f};
        bloub_orbit_pose(&s, REST, &p);
        float step = fabsf(p.rot - prev.rot);
        if (step > 3.0f) step = fabsf(step - BLOUB_TAU);  /* the wrap itself */
        assert(step < 1.0f);
        prev = p;
    }
    /* The settle unwinds at most half a turn, however long the spin ran. */
    for (float t = 1.0f; t < 8.0f; t += 0.25f) {
        bloub_orbit_pose_t held, easing;
        const bloub_orbit_t a0 = {t, 0.0f}, a1 = {t, 0.05f};
        bloub_orbit_pose(&a0, REST, &held);
        bloub_orbit_pose(&a1, REST, &easing);
        assert(fabsf(easing.rot - held.rot) < BLOUB_TAU * 0.5f);
    }
    bloub_orbit_pose_t a, b;
    const bloub_orbit_t ta = {1.0f, 0.0f}, tb = {1.8f, 0.0f};
    bloub_orbit_pose(&ta, REST, &a);
    bloub_orbit_pose(&tb, REST, &b);
    /* The gaze sweeps its full width inside one 0.97s swing; sampling two
     * instants can catch the same phase twice, so take the range. */
    float lo = 1e9f, hi = -1e9f;
    for (float t = 1.0f; t < 2.0f; t += 0.01f) {
        bloub_orbit_pose_t p;
        const bloub_orbit_t s = {t, 0.0f};
        bloub_orbit_pose(&s, REST, &p);
        if (p.gaze.yaw < lo) lo = p.gaze.yaw;
        if (p.gaze.yaw > hi) hi = p.gaze.yaw;
    }
    assert(hi - lo > 120.0f);

    /* A gaze that far round takes one eye behind the head, and a back-facing
     * eye is not drawn - without that gate it punches through the front. */
    int hidden = 0;
    for (float t = 1.0f; t < 2.0f; t += 0.005f) {
        bloub_orbit_pose_t turned;
        const bloub_orbit_t tc = {t, 0.0f};
        bloub_orbit_pose(&tc, REST, &turned);
        bloub_eye_t eyes[2];
        bloub_eye_poses(turned.gaze, BALL, 16.0f, eyes);
        if (eyes[0].depth <= 0.02f || eyes[1].depth <= 0.02f) hidden++;
    }
    assert(hidden > 0);

    /* Once handed back, the body is upright and the face is the resting one. */
    bloub_orbit_pose_t done;
    const bloub_orbit_t td = {3.0f, 1.0f};
    bloub_orbit_pose(&td, REST, &done);
    assert(fabsf(done.rot) < 1e-3f);
    assert(fabsf(done.gaze.yaw - REST.yaw) < 1e-3f);
    assert(fabsf(done.eye_h - 0.44f) < 1e-3f);

    printf("bloub_orbit_check: ok\n");
    return 0;
}
