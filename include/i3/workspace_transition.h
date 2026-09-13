#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the workspace transition. The two workspaces slide across an
 * overlay while the real switch happens underneath.
 *
 * The root window is never copied. The outgoing workspace is captured via
 * COMPOSITE while it is still mapped, and the incoming one comes from the
 * stale cache: freshly mapped clients take a few frames to repaint, and would
 * otherwise show as black.
 *
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "libi3.h"
#include "i3/aiwr_capture.h"

struct Con;

typedef enum {
    WT_SLIDE = 0,
    WT_FADE,
    WT_ZOOM,
} wt_type_t;

typedef enum {
    WT_HORIZONTAL = 0,
    WT_VERTICAL,
} wt_direction_t;

typedef struct workspace_transition_config {
    bool enabled;
    int duration_ms; /* 0 disables the transition */
    wt_direction_t direction;
    int fps; /* shared with the other animations */
    wt_type_t type;
    char *curve; /* NULL for ease-out-cubic */
} workspace_transition_config_t;

typedef struct workspace_transition_state {
    bool initialized;
    bool active;

    xcb_window_t overlay_window;
    xcb_colormap_t colormap;
    xcb_pixmap_t back_pixmap;
    xcb_visualid_t visual;
    uint8_t depth;
    surface_t front;
    surface_t back;
    bool overlay_ready;
    bool overlay_visible;
    int ov_w, ov_h;

    Rect out;    /* rect of the output the switch happens on */
    double sign; /* +1 when the new workspace enters from the right or
                  * bottom, -1 from the opposite side */

    aiwr_layers_t old_layers;
    aiwr_layers_t new_layers;

    double progress;
    int anim_id; /* in aiwr_anim */
} workspace_transition_state_t;

void workspace_transition_init(void);

/**
 * Called from within workspace_show(), while 'from' is still mapped.
 *
 */
void workspace_transition_begin(struct Con *from, struct Con *to);

/**
 * Cuts the running transition and releases the overlay. Called on randr
 * changes and at shutdown.
 *
 */
void workspace_transition_abort(void);

bool workspace_transition_active(void);

/**
 * Called on config reload: the wallpaper is decoded once and cached.
 *
 */
void workspace_transition_invalidate_wallpaper(void);

extern workspace_transition_config_t workspace_transition_config;
extern workspace_transition_state_t workspace_transition_state;
