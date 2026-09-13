#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the window switcher, bound to Alt+Tab.
 *
 * Most-recently-used, not spatial. The overview answers "where is
 * everything"; this answers "take me back to what I was doing". So the order
 * is by recent focus and it crosses workspaces — which is exactly the case
 * the overview is worst at, since you would have to find the window by eye.
 *
 * i3 maintains that order already: focus_head is in most-recently-focused
 * order, so walking it depth-first from croot gives the list for free.
 *
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "libi3.h"
#include "i3/aiwr_capture.h"

struct Con;

typedef struct switcher_config {
    bool enabled;
    int max_items;  /* past this many windows, use the overview instead */
    int cell_width; /* px, before logical_px scaling */
    int cell_height;
    bool show_preview;
} switcher_config_t;

void switcher_init(void);

/**
 * Opens the switcher, or steps the selection if it is already open. Called
 * from the 'switcher next' and 'switcher prev' commands.
 *
 */
void switcher_open(bool backwards);

bool switcher_is_active(void);

/**
 * Called from handle_event(), before the overview hook. Returns true when the
 * event was consumed.
 *
 */
bool switcher_handle_event(xcb_generic_event_t *event);

/**
 * Tears down the switcher and releases the keyboard grab. Called on randr
 * changes and at shutdown.
 *
 */
void switcher_abort(void);

extern switcher_config_t switcher_config;
