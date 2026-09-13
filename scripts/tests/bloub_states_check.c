/* Checks the working cycle: that each state is its own pose, that the chosen
 * shape survives the states that do not draw their own, that the cross-fade
 * actually lands on both ends, and that nothing leaves the canvas.
 *
 *   cc -I main/display/bloub -o /tmp/bloub_states_check \\
 *      scripts/tests/bloub_states_check.c main/display/bloub/bloub_states.c \\
 *      main/display/bloub/bloub_decor.c main/display/bloub/bloub_shapes.c \\
 *      main/display/bloub/bloub_face.c -lm
 *
 * No LVGL, no display: a buffer of panel words and the arithmetic. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bloub_states.h"

#define S 166
static uint16_t buf[S * S];
static bloub_pose_t a, b, mix;

static int differs(const bloub_pose_t* x, const bloub_pose_t* y) {
    for (int i = 0; i < SHAPE_SAMPLES; i++)
        if (fabsf(x->radii[i] - y->radii[i]) > 1e-4f) return 1;
    return fabsf(x->gaze.yaw - y->gaze.yaw) > 1e-4f ||
           fabsf(x->eyes[1].h - y->eyes[1].h) > 1e-4f ||
           x->dot_count != y->dot_count || x->arc_count != y->arc_count;
}

int main(void) {
    /* The wearer's shape is the body of every state that does not draw its
     * own. Droplet is the least circle-like of them, so it is the one that
     * would show a silent fallback to a ball. */
    const float* droplet = SHAPE_PROFILES[SHAPE_DROPLET];
    const bloub_state_id_t wears_it[] = {BLOUB_STATE_IDLE, BLOUB_STATE_WINK, BLOUB_STATE_WIDE};
    for (unsigned i = 0; i < sizeof(wears_it) / sizeof(wears_it[0]); i++) {
        bloub_pose_sample(wears_it[i], 0.4f, droplet, &a);
        for (int k = 0; k < SHAPE_SAMPLES; k++) assert(fabsf(a.radii[k] - droplet[k]) < 1e-5f);
    }

    /* Every state is a different pose - a cycle that repeats the same picture
     * under different names is the failure that would look like nothing. */
    for (int i = 0; i < BLOUB_STATE_COUNT; i++) {
        for (int j = i + 1; j < BLOUB_STATE_COUNT; j++) {
            bloub_pose_sample((bloub_state_id_t)i, 0.5f, droplet, &a);
            bloub_pose_sample((bloub_state_id_t)j, 0.5f, droplet, &b);
            assert(differs(&a, &b));
        }
    }

    /* thinking puts its dots out and takes the eyes away; play brings arcs. */
    bloub_pose_sample(BLOUB_STATE_THINKING, 0.8f, droplet, &a);
    assert(a.dot_count == 2 && a.eye_alpha < 0.5f);
    bloub_pose_sample(BLOUB_STATE_PLAY, 0.8f, droplet, &a);
    assert(a.arc_count == 4);

    /* The cross-fade lands exactly on each end, and carries both states'
     * decor through the middle rather than dropping one. */
    bloub_pose_sample(BLOUB_STATE_THINKING, 1.0f, droplet, &a);
    bloub_pose_sample(BLOUB_STATE_PLAY, 0.0f, droplet, &b);
    bloub_pose_blend(&a, &b, 0.0f, &mix);
    for (int k = 0; k < SHAPE_SAMPLES; k++) assert(fabsf(mix.radii[k] - a.radii[k]) < 1e-4f);
    bloub_pose_blend(&a, &b, 1.0f, &mix);
    for (int k = 0; k < SHAPE_SAMPLES; k++) assert(fabsf(mix.radii[k] - b.radii[k]) < 1e-4f);
    bloub_pose_blend(&a, &b, 0.5f, &mix);
    assert(mix.dot_count == a.dot_count && mix.arc_count == b.arc_count);

    /* Turning takes the short way round: from just under half a turn to just
     * over, the blend must not travel the long way. */
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.rot = 3.0f;
    b.rot = -3.0f;
    bloub_pose_blend(&a, &b, 0.5f, &mix);
    assert(fabsf(mix.rot) > 3.0f);

    /* Nothing the cycle draws leaves the canvas, at the resting body size.
     * PLAY is left out on purpose and for this exact reason: its swoosh sweeps
     * past 1.8 body radii, wider than the call screen's canvas, so it is
     * ported but not cycled - see kWorkingCycle in lcd_display.cc. */
    const bloub_state_id_t cycled[] = {BLOUB_STATE_IDLE, BLOUB_STATE_THINKING, BLOUB_STATE_WINK,
                                       BLOUB_STATE_WIDE};
    const float ball = (float)S * 0.46f, mid = (float)S * 0.5f;
    for (unsigned ci = 0; ci < sizeof(cycled) / sizeof(cycled[0]); ci++) {
        const int i = (int)cycled[ci];
        for (float t = 0.0f; t < 2.6f; t += 0.1f) {
            bloub_pose_sample((bloub_state_id_t)i, t, droplet, &a);
            memset(buf, 0, sizeof(buf));
            bloub_pose_draw_arcs(buf, S, S, &a, ball, mid, mid, true);
            bloub_pose_draw_arcs(buf, S, S, &a, ball, mid, mid, false);
            bloub_pose_draw_dots(buf, S, S, &a, ball, mid, mid, 0xFFFF);
            for (int x = 0; x < S; x++) assert(buf[x] == 0 && buf[(S - 1) * S + x] == 0);
            for (int y = 0; y < S; y++) assert(buf[y * S] == 0 && buf[y * S + S - 1] == 0);
        }
    }

    printf("bloub_states_check: ok\n");
    return 0;
}
