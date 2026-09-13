#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the shared animation clock.
 *
 * One ev_timer for everything. Each registered animation receives its
 * progress already passed through its curve, and the frame ends with a single
 * xcb_flush and, if anyone asked for it, a single synchronising round-trip.
 *
 * With a timer per module the frames landed at different phases and each one
 * flushed separately, which was where most of the stutter came from.
 *
 */
#include <stdbool.h>

/* p is linear in 0..1; e is p passed through the curve, and may exceed 1 when
 * the curve overshoots. */
typedef void (*aiwr_anim_step_cb)(double p, double e, void *data);
typedef void (*aiwr_anim_done_cb)(void *data);

void aiwr_anim_init(void);
void aiwr_anim_set_fps(int fps);
int aiwr_anim_fps(void);

/**
 * Starts an animation and returns its id, or 0 on failure. A duration of 0 or
 * less runs the final step immediately and registers nothing.
 *
 * The curve is copied by value, so redefining it later cannot affect a
 * running animation. A spring curve supplies its own duration, overriding the
 * one given here.
 *
 */
int aiwr_anim_start(double duration_ms, const char *curve,
                    aiwr_anim_step_cb step, aiwr_anim_done_cb done, void *data);

/**
 * Stops an animation without calling its done callback, for when the owner is
 * cleaning up itself.
 *
 */
void aiwr_anim_cancel(int id);

/**
 * Jumps to the end: step(1, 1) followed by done.
 *
 */
void aiwr_anim_finish(int id);

bool aiwr_anim_alive(int id);
bool aiwr_anim_any(void);

/**
 * Called from within a step to request this frame's synchronising
 * round-trip. Only animations that draw need it; those that merely issue
 * configure requests do not.
 *
 */
void aiwr_anim_request_fence(void);

/**
 * Called from within a step to request one tree_render() after every
 * animation has stepped, rather than one per animation.
 *
 */
void aiwr_anim_request_render(void);
