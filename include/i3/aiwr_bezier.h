#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: animation curves, of two kinds.
 *
 * A bezier is a cubic-bezier in the CSS and Hyprland convention: P0 is (0,0),
 * P3 is (1,1), and only the two control points matter. y may exceed 1, which
 * is where overshoot comes from.
 *
 * A spring is a damped harmonic oscillator taking niri's parameters —
 * damping-ratio, stiffness, epsilon and mass. A spring has no duration: it
 * runs until it settles within epsilon. That settle time is computed when the
 * curve is defined and becomes the animation's duration, so the duration set
 * in the config is ignored for springs.
 *
 */
#include <stdbool.h>

typedef enum {
    AIWR_CURVE_BEZIER = 0,
    AIWR_CURVE_SPRING,
} aiwr_curve_type_t;

typedef struct aiwr_curve {
    char *name;
    aiwr_curve_type_t type;

    /* bezier */
    double x1, y1, x2, y2;

    /* spring */
    double damping_ratio;
    double stiffness;
    double mass;
    double epsilon;
    double settle_ms; /* computed in aiwr_curve_define_spring */
} aiwr_curve_t;

void aiwr_curves_init(void);
int aiwr_curve_count(void);
const char *aiwr_curve_name_at(int i);

bool aiwr_curve_define(const char *name, double x1, double y1, double x2, double y2);
bool aiwr_curve_define_spring(const char *name, double damping_ratio, double stiffness,
                              double mass, double epsilon, double speed);

/**
 * Parses the bezier directive: "name 0.05 0.7 0.1 1".
 *
 */
bool aiwr_curve_define_spec(const char *spec);

/**
 * Parses the spring directive:
 * "name damping-ratio=1.0 stiffness=1000 epsilon=0.0001 [mass=] [speed=]".
 * The keys may appear in any order.
 *
 */
bool aiwr_curve_define_spring_spec(const char *spec);

const aiwr_curve_t *aiwr_curve_get(const char *name);

/**
 * Evaluates the curve at p, which runs from 0 to 1. With no curve, falls back
 * to ease-out-cubic.
 *
 */
double aiwr_curve_eval(const aiwr_curve_t *c, double p);
double aiwr_curve_eval_named(const char *name, double p);

/**
 * Returns the curve's natural duration in milliseconds, or 0 for a curve that
 * has none.
 *
 */
double aiwr_curve_natural_duration_ms(const aiwr_curve_t *c);
