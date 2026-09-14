/*
 * i3-aiwr — window open/close animations. See window_animation.h.
 */
#include "all.h"
#include "i3/window_animation.h"
#include "i3/aiwr_anim.h"
#include "i3/rounded_corners.h"
#include <xcb/composite.h>
#include <xcb/shape.h>
#include <math.h>
#include <time.h>

#define WALOG(fmt, ...) DLOG("[i3-aiwr] WindowAnim: " fmt, ##__VA_ARGS__)

#define WA_STARTUP_QUIET_MS 1200.0

window_animation_config_t window_animation_config = {
    .enabled = true,
    .duration_ms = 160,
    .start_scale = 88,
    .fps = 60,
    .curve = NULL,
    .close_enabled = true,
    .close_duration_ms = 140,
    .close_scale = 60,
    .close_curve = NULL,
    .opacity = true,
    .start_opacity = 0,
    .close_opacity = 0,
};

typedef struct wa_ghost {
    xcb_window_t window;
    xcb_pixmap_t pixmap;
    xcb_colormap_t colormap;
    Rect rect;
} wa_ghost_t;

typedef struct wa_item {
    xcb_window_t frame;
    int anim_id;
} wa_item_t;

static struct {
    bool initialized;
    double init_ms;

    xcb_window_t *seen;
    int num_seen;

    wa_item_t *items;
    int count;
} wa;

static xcb_atom_t wa_opacity_atom(void) {
    static xcb_atom_t atom = XCB_NONE;
    static bool tried = false;
    if (tried) return atom;
    tried = true;
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(
        conn, xcb_intern_atom(conn, 0, strlen("_NET_WM_WINDOW_OPACITY"), "_NET_WM_WINDOW_OPACITY"), NULL);
    atom = r ? r->atom : XCB_NONE;
    free(r);
    return atom;
}

static void wa_set_opacity(xcb_window_t win, double a) {
    if (!window_animation_config.opacity) return;
    aiwr_capture_ensure();
    if (!aiwr_capture_external()) return;
    xcb_atom_t atom = wa_opacity_atom();
    if (atom == XCB_NONE) return;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    uint32_t v = (uint32_t)(a * 0xffffffffu);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom, XCB_ATOM_CARDINAL, 32, 1, &v);
}

static void wa_clear_opacity(xcb_window_t win) {
    xcb_atom_t atom = wa_opacity_atom();
    if (atom == XCB_NONE) return;
    xcb_delete_property(conn, win, atom);
}

static xcb_pixmap_t wa_snapshot_frame(Con *con) {
    xcb_pixmap_t named = xcb_generate_id(conn);
    xcb_void_cookie_t ck = xcb_composite_name_window_pixmap_checked(conn, con->frame.id, named);
    xcb_generic_error_t *e = xcb_request_check(conn, ck);
    if (e != NULL) {
        WALOG("close: NameWindowPixmap(0x%08x) failed: %d\n", con->frame.id, e->error_code);
        free(e);
        return XCB_NONE;
    }

    xcb_pixmap_t copy = xcb_generate_id(conn);
    xcb_create_pixmap(conn, con->depth, copy, con->frame.id, con->rect.width, con->rect.height);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t gcv[] = {0};
    xcb_create_gc(conn, gc, copy, XCB_GC_GRAPHICS_EXPOSURES, gcv);
    xcb_copy_area(conn, named, copy, gc, 0, 0, 0, 0, con->rect.width, con->rect.height);
    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, named);
    return copy;
}

static void wa_ghost_apply(wa_ghost_t *g, double f) {
    uint32_t w = (uint32_t)fmax(1.0, g->rect.width * f);
    uint32_t h = (uint32_t)fmax(1.0, g->rect.height * f);
    uint32_t vals[5] = {
        (uint32_t)(int32_t)(g->rect.x + ((int32_t)g->rect.width - (int32_t)w) / 2),
        (uint32_t)(int32_t)(g->rect.y + ((int32_t)g->rect.height - (int32_t)h) / 2),
        w, h, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(conn, g->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_STACK_MODE,
                         vals);
}

static void wa_ghost_step(double p, double e, void *data) {
    wa_ghost_t *g = data;
    double f1 = window_animation_config.close_scale / 100.0;
    wa_ghost_apply(g, 1.0 + (f1 - 1.0) * e);
    double a1 = window_animation_config.close_opacity / 100.0;
    wa_set_opacity(g->window, 1.0 + (a1 - 1.0) * e);
}

static void wa_ghost_done(void *data) {
    wa_ghost_t *g = data;
    if (g->window != XCB_NONE) xcb_destroy_window(conn, g->window);
    if (g->colormap != XCB_NONE) xcb_free_colormap(conn, g->colormap);
    if (g->pixmap != XCB_NONE) xcb_free_pixmap(conn, g->pixmap);
    free(g);
    xcb_flush(conn);
}

void window_animation_on_close(Con *con) {
    if (con == NULL || con->frame.id == XCB_NONE) return;
    if (!wa.initialized) return;
    if (!window_animation_config.enabled) return;
    if (!window_animation_config.close_enabled) return;
    if (window_animation_config.close_duration_ms <= 0) return;
    if (window_animation_config.close_scale >= 100) return;
    if (!con->mapped) return; /* já estava escondida: ninguém veria */
    if (con->rect.width == 0 || con->rect.height == 0) return;
    if (!aiwr_capture_available()) return;
    if (overview_is_active() || workspace_transition_active()) return;

    xcb_pixmap_t copy = wa_snapshot_frame(con);
    if (copy == XCB_NONE) return;

    uint8_t ghost_depth = (con->depth == 32) ? 32 : root_screen->root_depth;
    xcb_visualtype_t *vt = aiwr_visualtype_for_depth(ghost_depth);

    if (vt == NULL) {
        xcb_free_pixmap(conn, copy);
        return;
    }

    wa_ghost_t *g = scalloc(1, sizeof(wa_ghost_t));
    g->pixmap = copy;
    g->rect = con->rect;
    g->colormap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, g->colormap, root, vt->visual_id);

    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                    XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values[] = {g->pixmap, 0, 1, XCB_EVENT_MASK_NO_EVENT, g->colormap};
    g->window = xcb_generate_id(conn);
    xcb_create_window(conn, con->depth, g->window, root,
                      (int16_t)g->rect.x, (int16_t)g->rect.y,
                      (uint16_t)g->rect.width, (uint16_t)g->rect.height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, vt->visual_id, mask, values);
    aiwr_set_overlay_hints(g->window, "i3-aiwr closing");
    /* região de input vazia: o fantasma não rouba cliques */
    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         g->window, 0, 0, 0, NULL);
    xcb_map_window(conn, g->window);
    wa_ghost_apply(g, 1.0);
    xcb_flush(conn);

    aiwr_anim_set_fps(window_animation_config.fps);
    if (aiwr_anim_start(window_animation_config.close_duration_ms,
                        window_animation_config.close_curve,
                        wa_ghost_step, wa_ghost_done, g) == 0) {
        wa_ghost_done(g); /* duração 0 */
        return;
    }
    WALOG("close 0x%08x (%dx%d)\n", con->frame.id, con->rect.width, con->rect.height);
}

static double wa_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static bool wa_seen(xcb_window_t frame) {
    for (int i = 0; i < wa.num_seen; i++) {
        if (wa.seen[i] == frame) return true;
    }
    return false;
}

static void wa_mark_seen(xcb_window_t frame) {
    if (wa_seen(frame)) return;
    wa.seen = srealloc(wa.seen, sizeof(xcb_window_t) * (wa.num_seen + 1));
    wa.seen[wa.num_seen++] = frame;
}

static wa_item_t *wa_find(xcb_window_t frame) {
    for (int i = 0; i < wa.count; i++) {
        if (wa.items[i].frame == frame) return &wa.items[i];
    }
    return NULL;
}

static void wa_drop(xcb_window_t frame) {
    for (int i = wa.count - 1; i >= 0; i--) {
        if (wa.items[i].frame != frame) continue;
        wa.items[i] = wa.items[wa.count - 1];
        wa.count--;
    }
}

static void wa_apply(Con *con, double f) {
    Rect t = con->rect;
    uint32_t w = (uint32_t)fmax(1.0, t.width * f);
    uint32_t h = (uint32_t)fmax(1.0, t.height * f);
    uint32_t vals[4] = {
        (uint32_t)(int32_t)(t.x + ((int32_t)t.width - (int32_t)w) / 2),
        (uint32_t)(int32_t)(t.y + ((int32_t)t.height - (int32_t)h) / 2),
        w, h};
    xcb_configure_window(conn, con->frame.id,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         vals);
}

static void wa_step(double p, double e, void *data) {
    xcb_window_t frame = (xcb_window_t)(uintptr_t)data;
    Con *con = con_by_frame_id(frame);
    if (con == NULL) return;
    double f0 = window_animation_config.start_scale / 100.0;
    wa_apply(con, f0 + (1.0 - f0) * e);
    double a0 = window_animation_config.start_opacity / 100.0;
    wa_set_opacity(frame, a0 + (1.0 - a0) * e);
}

static void wa_finished(void *data) {
    xcb_window_t frame = (xcb_window_t)(uintptr_t)data;
    Con *con = con_by_frame_id(frame);
    if (con != NULL) {
        wa_clear_opacity(frame);
        wa_apply(con, 1.0);
        rounded_corners_apply(con);
    }
    wa_drop(frame);
}

void window_animation_init(void) {
    if (wa.initialized) return;
    if (conn == NULL || main_loop == NULL) return;
    memset(&wa, 0, sizeof(wa));
    wa.init_ms = wa_now_ms();
    aiwr_anim_init();
    wa.initialized = true;
}

static void wa_seed_rec(Con *con) {
    if (con == NULL) return;
    if (con->frame.id != XCB_NONE) wa_mark_seen(con->frame.id);
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) { wa_seed_rec(ch); }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) { wa_seed_rec(ch); }
}

void window_animation_seed_existing(void) {
    if (!wa.initialized) window_animation_init();
    if (!wa.initialized || croot == NULL) return;
    wa_seed_rec(croot);
    WALOG("seeded %d existing frames\n", wa.num_seen);
}

void window_animation_on_map(Con *con) {
    if (con == NULL || con->frame.id == XCB_NONE) return;
    if (!wa.initialized) window_animation_init();
    if (!wa.initialized) return;

    bool first_map = !wa_seen(con->frame.id);
    wa_mark_seen(con->frame.id);
    if (!first_map) return;

    if (!window_animation_config.enabled) return;
    if (window_animation_config.duration_ms <= 0) return;
    if (window_animation_config.start_scale >= 100) return;
    if (wa_now_ms() - wa.init_ms < WA_STARTUP_QUIET_MS) return;
    if (con->rect.width == 0 || con->rect.height == 0) return;
    if (overview_is_active() || workspace_transition_active()) return;
    if (wa_find(con->frame.id) != NULL) return;

    wa_apply(con, window_animation_config.start_scale / 100.0);
    xcb_flush(conn);

    aiwr_anim_set_fps(window_animation_config.fps);
    int id = aiwr_anim_start(window_animation_config.duration_ms,
                             window_animation_config.curve,
                             wa_step, wa_finished,
                             (void *)(uintptr_t)con->frame.id);
    if (id == 0) return;

    wa.items = srealloc(wa.items, sizeof(wa_item_t) * (wa.count + 1));
    wa.items[wa.count].frame = con->frame.id;
    wa.items[wa.count].anim_id = 0;
    wa.count++;


    if (id == 0) return; /* Already in the final state */
    wa_item_t *added = wa_find(con->frame.id);
    if (added != NULL) added->anim_id = id;

    WALOG("open 0x%08x (%dx%d)\n", con->frame.id, con->rect.width, con->rect.height);
}

void window_animation_forget(xcb_window_t frame) {
    for (int i = 0; i < wa.num_seen; i++) {
        if (wa.seen[i] != frame) continue;
        wa.seen[i] = wa.seen[wa.num_seen - 1];
        wa.num_seen--;
        break;
    }
    wa_item_t *it = wa_find(frame);
    if (it != NULL) {
        aiwr_anim_cancel(it->anim_id);
        wa_clear_opacity(frame);
        wa_drop(frame);
    }
}

bool window_animation_running(void) {
    return wa.count > 0;
}
