/*
 * i3-aiwr — relógio único das animações. Ver aiwr_anim.h.
 */
#include "all.h"
#include "i3/aiwr_anim.h"
#include "i3/aiwr_bezier.h"
#include <time.h>

#define ANIMLOG(fmt, ...) DLOG("[i3-aiwr] Anim: " fmt, ##__VA_ARGS__)

typedef struct anim_slot {
    int id;
    bool active;
    double start_ms;
    double duration_ms;
    aiwr_curve_t curve; /* cópia: imune a realloc/redefinição */
    bool has_curve;
    aiwr_anim_step_cb step;
    aiwr_anim_done_cb done;
    void *data;
} anim_slot_t;

static struct {
    bool initialized;
    anim_slot_t *slots;
    int count;
    int next_id;
    int fps;
    bool timer_running;
    bool fence_wanted;
    bool render_wanted;
    ev_timer timer;
} an;

static double anim_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static anim_slot_t *slot_by_id(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < an.count; i++) {
        if (an.slots[i].id == id && an.slots[i].active) return &an.slots[i];
    }
    return NULL;
}

static bool in_tick;

static void anim_compact(void) {
    if (in_tick) return;
    int w = 0;
    for (int i = 0; i < an.count; i++) {
        if (an.slots[i].active) an.slots[w++] = an.slots[i];
    }
    an.count = w;
}

static void anim_stop_timer_if_idle(void) {
    if (an.count > 0 || !an.timer_running) return;
    ev_timer_stop(main_loop, &an.timer);
    an.timer_running = false;
}

static void anim_tick(EV_P_ ev_timer *w, int revents) {
    double now = anim_now_ms();
    an.fence_wanted = false;

    /* o snapshot do count evita rodar, no mesmo frame, animações criadas
     * dentro de um step */
    in_tick = true;
    int n = an.count;
    for (int i = 0; i < n; i++) {
        anim_slot_t *s = &an.slots[i];
        if (!s->active) continue;
        double p = (s->duration_ms <= 0) ? 1.0 : (now - s->start_ms) / s->duration_ms;
        if (p < 0.0) p = 0.0;
        bool last = (p >= 1.0);
        if (last) p = 1.0;
        double e = s->has_curve ? aiwr_curve_eval(&s->curve, p) : aiwr_curve_eval(NULL, p);
        if (s->step != NULL) s->step(p, e, s->data);
        if (!last) continue;
        /* o dono pode ter cancelado dentro do step */
        if (!s->active) continue;
        s->active = false;
        if (s->done != NULL) s->done(s->data);
    }
    in_tick = false;

    anim_compact();

    if (an.render_wanted) {
        an.render_wanted = false;
        tree_render();
    }

    xcb_flush(conn);
    if (an.fence_wanted) {
        /* segura até o servidor consumir o frame: sem isso os requests
         * acumulam e a animação vira um filme atrasado */
        xcb_get_input_focus_reply_t *fence =
            xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL);
        free(fence);
    }
    anim_stop_timer_if_idle();
}

void aiwr_anim_init(void) {
    if (an.initialized) return;
    if (conn == NULL || main_loop == NULL) return;
    memset(&an, 0, sizeof(an));
    an.next_id = 1;
    an.fps = 60;
    ev_timer_init(&an.timer, anim_tick, 1.0 / 60.0, 1.0 / 60.0);
    an.initialized = true;
}

void aiwr_anim_set_fps(int fps) {
    if (fps < 15) fps = 15;
    if (fps > 240) fps = 240;
    if (fps == an.fps) return;
    an.fps = fps;
    if (an.timer_running) { /* reinicia no novo período */
        ev_timer_stop(main_loop, &an.timer);
        double per = 1.0 / an.fps;
        ev_timer_set(&an.timer, per, per);
        ev_timer_start(main_loop, &an.timer);
    }
}

int aiwr_anim_fps(void) {
    return an.fps > 0 ? an.fps : 60;
}

static void anim_start_timer(void) {
    if (an.timer_running) return;
    double per = 1.0 / aiwr_anim_fps();
    ev_timer_set(&an.timer, per, per);
    ev_timer_start(main_loop, &an.timer);
    an.timer_running = true;
}

int aiwr_anim_start(double duration_ms, const char *curve,
                    aiwr_anim_step_cb step, aiwr_anim_done_cb done, void *data) {
    if (!an.initialized) aiwr_anim_init();
    if (!an.initialized) return 0;

    const aiwr_curve_t *c = aiwr_curve_get(curve);
    double natural = aiwr_curve_natural_duration_ms(c);
    if (natural > 0.0) duration_ms = natural;

    if (duration_ms <= 0) { /* sem animação: vai direto ao estado final */
        if (step != NULL) step(1.0, 1.0, data);
        if (done != NULL) done(data);
        xcb_flush(conn);
        return 0;
    }

    an.slots = srealloc(an.slots, sizeof(anim_slot_t) * (an.count + 1));
    anim_slot_t *s = &an.slots[an.count++];
    memset(s, 0, sizeof(*s));
    s->id = an.next_id++;
    if (an.next_id <= 0) an.next_id = 1;
    s->active = true;
    s->start_ms = anim_now_ms();
    s->duration_ms = duration_ms;
    if (c != NULL) {
        s->curve = *c;
        s->curve.name = NULL; /* a cópia não é dona do nome */
        s->has_curve = true;
    }
    s->step = step;
    s->done = done;
    s->data = data;

    anim_start_timer();
    return s->id;
}

void aiwr_anim_cancel(int id) {
    anim_slot_t *s = slot_by_id(id);
    if (s == NULL) return;
    s->active = false;
    anim_compact();
    anim_stop_timer_if_idle();
}

void aiwr_anim_finish(int id) {
    anim_slot_t *s = slot_by_id(id);
    if (s == NULL) return;
    s->active = false;
    if (s->step != NULL) s->step(1.0, 1.0, s->data);
    if (s->done != NULL) s->done(s->data);
    anim_compact();
    xcb_flush(conn);
    anim_stop_timer_if_idle();
}

bool aiwr_anim_alive(int id) {
    return slot_by_id(id) != NULL;
}

bool aiwr_anim_any(void) {
    return an.count > 0;
}

void aiwr_anim_request_fence(void) {
    an.fence_wanted = true;
}

void aiwr_anim_request_render(void) {
    an.render_wanted = true;
}
