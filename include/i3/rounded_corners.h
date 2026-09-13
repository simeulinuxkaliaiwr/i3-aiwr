#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: rounded window corners, applied with the SHAPE extension.
 *
 * Both the frame and the client window are shaped: the client sits inside the
 * frame's border and needs a smaller radius, or its square corners would poke
 * through the frame's rounded ones.
 *
 * Clients that ship their own shape are left alone, unless the shape is one
 * we applied — which is what the _I3_AIWR_ROUNDED property records, so that
 * our own work is still recognisable after a restart.
 *
 */
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/shape.h>

struct Con;

/**
 * Applies or updates the shape. Cheap to call repeatedly: it returns early
 * when nothing relevant has changed.
 *
 */
void rounded_corners_apply(struct Con *con);

/**
 * Removes the shape but keeps the bookkeeping, so a later apply can restore
 * it.
 *
 */
void rounded_corners_remove(struct Con *con);

/**
 * Drops the bookkeeping entirely. Called from x_con_kill().
 *
 */
void rounded_corners_forget(struct Con *con);

bool rounded_corners_should_apply(struct Con *con);

/**
 * Returns the radius to use for this con, clamped to half its smaller side,
 * or 0 when rounding does not apply.
 *
 */
int rounded_corners_radius_for(struct Con *con);

/**
 * Returns the radius for the client window inside the frame, reduced by the
 * border width.
 *
 */
int rounded_corners_inner_radius(struct Con *con, int radius);

bool rounded_corners_owns_window(xcb_window_t win);

/**
 * Rasterises a rounded rectangle into horizontal spans: one per scanline
 * through each corner arc, plus one rectangle for the straight middle. SHAPE
 * takes rectangles rather than paths, so the corners have to be rasterised.
 *
 * Returns the number of rectangles written to out.
 *
 */
int generate_rounded_rectangles(xcb_rectangle_t *out, int max_rects,
                                int width, int height, int radius);
