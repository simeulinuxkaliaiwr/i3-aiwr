#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the scrolling layout, in the style of niri.
 *
 * Columns sit side by side at their own widths and the strip continues past
 * the edge of the screen. The container holds a view offset; the renderer
 * subtracts it from each column's x, and X clips whatever falls outside.
 *
 * The key difference from L_SPLITH: in a split the percentages sum to 1 and
 * divide the available space, whereas here each percentage is that column's
 * own width as a fraction of the viewport, independent of its siblings. That
 * is why con_fix_percent() must leave these containers alone.
 *
 */
#include <stdbool.h>

struct Con;

typedef struct scrolling_config {
    int default_width; /* percentage of the viewport for a new column */
    int duration_ms;   /* scroll animation; 0 for instant */
    char *curve;
    bool center_focus; /* centre the focused column rather than just bringing
                        * it into view */
} scrolling_config_t;

/**
 * Returns the current view offset of a scrolling container, in pixels.
 *
 */
double scrolling_offset(struct Con *con);

/**
 * Scrolls so that the given column is visible. Animates when duration_ms > 0.
 *
 */
void scrolling_reveal(struct Con *column);

/**
 * Called from the end of con_focus().
 *
 */
void scrolling_on_focus(struct Con *con);

/**
 * Adjusts the width of a column by the given number of percentage points.
 *
 */
void scrolling_resize_column(struct Con *column, int delta_ppt);

/**
 * Adjusts the width of a column by a number of pixels, without scrolling the
 * view. Used during an interactive resize, where revealing would fight the
 * pointer.
 *
 */
void scrolling_resize_column_px(struct Con *column, int px);

/**
 * Handles the resize command for a scrolling column, where there is no
 * neighbour to take space from. Returns false for vertical resizes and for
 * anything that is not a column, so the caller falls back to i3's own path.
 *
 */
bool scrolling_resize_handled(struct Con *con, const char *direction, long px, long ppt);

/**
 * Called from the renderer before the columns are positioned, to re-clamp the
 * view offset.
 *
 */
void scrolling_prepare(struct Con *con);

/**
 * Toggles the focused column between its own width and the full viewport.
 *
 */
void scrolling_toggle_maximize(struct Con *con);

/**
 * Cycles the focused column through the preset widths.
 *
 */
void scrolling_cycle_width(struct Con *con, bool backwards);

/**
 * Scrolls the view by one column width, without moving focus.
 *
 */
void scrolling_scroll_by(struct Con *con, int dir);

/**
 * Switches the workspace between the scrolling layout and splith, preserving
 * each column's width across the round trip.
 *
 */
void scrolling_toggle_layout(struct Con *con);

/**
 * Called from con_free(). The scroll animation holds a raw Con pointer and
 * would write into freed memory if the container died mid-scroll.
 *
 */
void scrolling_forget(struct Con *con);

/**
 * Returns true while a scroll animation is running on this con's container.
 *
 */
bool scrolling_animating(struct Con *con);

/**
 * Returns the width of a column in pixels, given its container's viewport.
 *
 */
int scrolling_column_width(struct Con *con, struct Con *child);

int aiwr_count_scrolling_workspaces(void);

/**
 * Returns the nearest ancestor using the scrolling layout, or NULL.
 *
 */
struct Con *scrolling_container(struct Con *con);

extern scrolling_config_t scrolling_config;
