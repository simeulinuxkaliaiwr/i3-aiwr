#pragma once
/*
 * i3-aiwr — rounded corners.
 */
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/shape.h>

struct Con;

void rounded_corners_apply(struct Con *con);
void rounded_corners_remove(struct Con *con);
bool rounded_corners_should_apply(struct Con *con);
int rounded_corners_radius_for(struct Con *con);
int rounded_corners_inner_radius(struct Con *con, int radius);
bool rounded_corners_owns_window(xcb_window_t win);
void rounded_corners_forget(struct Con *con);

int generate_rounded_rectangles(xcb_rectangle_t *out, int max_rects,
                                int width, int height, int radius);
