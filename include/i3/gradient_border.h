#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: window borders painted with an animated gradient, in the spirit of
 * Hyprland's borderangle loop.
 *
 * Config:
 *   gradient_border enabled|disabled
 *   gradient_border color_start #RRGGBB[AA]     (focused window)
 *   gradient_border color_end   #RRGGBB[AA]
 *   gradient_border inactive_start #RRGGBB[AA]  (optional; without it,
 *   gradient_border inactive_end   #RRGGBB[AA]   unfocused windows keep
 *                                                i3's solid colour)
 *   gradient_border angle 45                    (degrees)
 *   gradient_border direction horizontal|vertical|diagonal
 *                                               (shorthand for angle 0|90|45)
 *   gradient_border speed 60                    (degrees per second, 0 for
 *                                                static)
 *   gradient_border fps 30
 *
 */
#include <stdbool.h>
#include <cairo/cairo.h>
#include "libi3.h"

struct Con;

typedef struct aiwr_gradient {
    bool enabled;
    color_t active_start, active_end;
    color_t inactive_start, inactive_end;
    bool inactive_set; /* false until inactive colours are configured */
    int angle;
    int speed;
    int fps;
    bool animate_inactive;
} aiwr_gradient_t;

extern aiwr_gradient_t aiwr_gradient;

void gradient_border_init(void);

/**
 * Returns the cairo pattern for the border of a con measuring w by h, or NULL
 * when i3's solid colour should be used instead.
 *
 */
cairo_pattern_t *gradient_border_pattern(double w, double h, bool active);

/**
 * Defined in x.c: repaints only the con's border rectangles and copies them
 * to the frame.
 *
 */
void x_gradient_border_repaint(struct Con *con);
