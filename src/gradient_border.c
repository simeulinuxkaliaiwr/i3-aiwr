/*
 * i3-aiwr — borders with animated gradient colors. See gradient_border.h.
 */
#include "all.h"
#include "i3/gradient_border.h"
#include <math.h>
#include <time.h>

aiwr_gradient_t aiwr_gradient = {
    .enabled = false,
    .active_start = {.red = 0.2, .green = 0.6, .blue = 1.0, .alpha = 1.0},
    .active_end = {.red = 0.8, .green = 0.3, .blue = 0.9, .alpha = 1.0},
    .inactive_set = false,
    .angle = 45,
    .speed = 60,
    .fps = 30,
    .animate_inactive = true,
};

static struct {
    bool initialized;
    ev_timer timer;
    double last_ms;
    double phase;
} gb;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

cairo_pattern_t *gradient_border_pattern(double w, double h, bool active) {
    aiwr_gradient_t *c = &aiwr_gradient;
    if (!c->enabled || w <= 0 || h <= 0) return NULL;
    if (!active && !c->inactive_set) return NULL;

    color_t a = active ? c->active_start : c->inactive_start;
    color_t b = active ? c->active_end : c->inactive_end;
    double deg = c->angle + ((active || c->animate_inactive) ? gb.phase : 0.0);
    double ang = deg * M_PI / 180.0;
    double dx = cos(ang), dy = sin(ang);
    double L = (fabs(dx) * w + fabs(dy) * h) / 2.0;
    double cx = w / 2.0, cy = h / 2.0;

    cairo_pattern_t *p = cairo_pattern_create_linear(cx - dx * L, cy - dy * L, cx + dx * L, cy + dy * L);
    cairo_pattern_add_color_stop_rgba(p, 0.0, a.red, a.green, a.blue, a.alpha);
    cairo_pattern_add_color_stop_rgba(p, 1.0, b.red, b.green, b.blue, b.alpha);
    return p;
}

static void repaint_tree(Con *con, bool focused_ws) {
    if (con == NULL) return;
    if (con->window != NULL && con_is_leaf(con)) {
        bool active = (con == focused || con_inside_focused(con));
        if (active || (aiwr_gradient.animate_inactive && aiwr_gradient.inactive_set)) {
            x_gradient_border_repaint(con);
        }
        return;
    }
    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        repaint_tree(child, focused_ws);
    }
    TAILQ_FOREACH (child, &(con->floating_head), floating_windows) {
        repaint_tree(child, focused_ws);
    }
}

static void tick(EV_P_ ev_timer *w, int revents) {
    aiwr_gradient_t *c = &aiwr_gradient;
    double now = now_ms();
    double dt = (now - gb.last_ms) / 1000.0;
    gb.last_ms = now;

    if (!c->enabled || c->speed == 0) {
        w->repeat = 1.0;
        ev_timer_again(EV_A_ w);
        return;
    }
    if (dt < 0 || dt > 0.5) dt = 1.0 / (c->fps > 0 ? c->fps : 30);
    gb.phase = fmod(gb.phase + c->speed * dt, 360.0);

    if (!overview_is_active()) {
        Output *output;
        TAILQ_FOREACH (output, &outputs, outputs) {
            if (!output->active || output->con == NULL) continue;
            Con *content = output_get_content(output->con);
            if (content == NULL) continue;
            Con *ws = con_get_fullscreen_con(content, CF_OUTPUT);
            if (ws != NULL) repaint_tree(ws, true);
        }
        xcb_flush(conn);
    }

    w->repeat = 1.0 / (c->fps > 0 ? c->fps : 30);
    ev_timer_again(EV_A_ w);
}

void gradient_border_init(void) {
    if (gb.initialized) return;
    gb.initialized = true;
    gb.last_ms = now_ms();
    ev_timer_init(&gb.timer, tick, 1.0, 1.0);
    ev_timer_start(main_loop, &gb.timer);
    LOG("[i3-aiwr] Gradient border initialized (enabled=%d, speed=%d, fps=%d)\n",
        aiwr_gradient.enabled, aiwr_gradient.speed, aiwr_gradient.fps);
}
