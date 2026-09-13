#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: window open and close animations.
 *
 * X cannot scale a window's contents, so instead of scaling anything we
 * animate only the FRAME's geometry: the child stays at its final size and
 * the frame reveals it. No ConfigureNotify reaches the client, so a terminal
 * does not reflow sixty times a second.
 *
 * Closing is different, because the window is already gone by the time we
 * could draw it. The frame's pixmap is copied into one we own and animated as
 * a ghost window.
 *
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

struct Con;

typedef struct window_animation_config {
    bool enabled;
    int duration_ms; /* 0 disables the open animation */
    int start_scale; /* percentage of the final size on the first frame */
    int fps;         /* shared with the other animations */
    char *curve;     /* NULL for ease-out-cubic */

    bool close_enabled;
    int close_duration_ms;
    int close_scale; /* percentage of the original size on the last frame */
    char *close_curve;

    bool opacity;       /* fade alongside the scale; needs a compositor */
    int start_opacity;  /* percentage on the first frame of an open */
    int close_opacity;  /* percentage on the last frame of a close */
} window_animation_config_t;

/**
 * Called from main.c at startup, before the first window. If the module
 * initialised itself on the first map instead, the startup quiet period would
 * swallow that very window.
 *
 */
void window_animation_init(void);

/**
 * Called from main.c after the tree is built. An in-place restart remaps
 * every window, and without this the whole session would animate at once.
 *
 */
void window_animation_seed_existing(void);

/**
 * Called from x.c, just after xcb_map_window() on the frame. Only the first
 * map of each frame is an open: a workspace switch remaps windows and must
 * not animate.
 *
 */
void window_animation_on_map(struct Con *con);

/**
 * Called from the first line of _x_con_kill(), before anything is freed.
 *
 */
void window_animation_on_close(struct Con *con);

/**
 * Called from x_con_kill(), alongside aiwr_stale_forget().
 *
 */
void window_animation_forget(xcb_window_t frame);

bool window_animation_running(void);

extern window_animation_config_t window_animation_config;
