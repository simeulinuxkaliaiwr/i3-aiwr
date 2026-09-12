/*
 * i3-aiwr — Overview niri-like. See overview.h.
 *
 *  • Entrada: a workspace atual "afasta" (zoom-out) até virar uma thumbnail
 *    numa coluna vertical com as demais; a saída faz o caminho inverso para a
 *    workspace escolhida. Frame 0 e frame final são pixel-idênticos à tela.
 *  • Conteúdo: pixmaps nomeados (COMPOSITE) de cada frame. Eles continuam
 *    válidos depois que a workspace sai de cena, então as thumbnails das
 *    workspaces ocultas mostram a última imagem real de cada janela — e não
 *    uma cópia da root (que com picom é só o wallpaper).
 *  • Drag & drop: arraste uma janela de uma thumbnail para outra (ou para o
 *    slot "+" no fim) para movê-la de workspace. Clique numa janela foca ela.
 */
#include "all.h"
#include "i3/aiwr_capture.h"
#include <xcb/xcb_keysyms.h>
#include <xcb/composite.h>
#include <X11/keysym.h>
#include <cairo/cairo-xcb.h>
#include <math.h>
#include <time.h>

#define OVLOG(fmt, ...) LOG("[i3-aiwr] Overview: " fmt, ##__VA_ARGS__)
#define THUMB_RADIUS 10.0
#define SCROLL_ANIM_MS 220.0
#define DRAG_THRESHOLD 6.0
#define MAX_PARTICLES 256
#define FRAME_BUDGET_MS 12.0
#define PARTICLE_SLOW_FRAMES 45
#define OVERVIEW_SPILL_ALPHA 0.78

overview_config_t overview_config = {
    .enabled = true,
    .thumbnail_scale = 58,
    .spacing = 28,
    .animation_duration_ms = 240,
    .show_workspace_names = true,
    .background_opacity = 70,
    .live_previews = true,
    .particles = 40,
    .fps = 60,
    .border_start = {.red = 0.388, .green = 0.400, .blue = 0.945, .alpha = 1.0}, /* #6366f1 */
    .border_end = {.red = 0.925, .green = 0.282, .blue = 0.600, .alpha = 1.0},   /* #ec4899 */
    .border_inactive = {.red = 1.0, .green = 1.0, .blue = 1.0, .alpha = 0.18},
    .border_width = 3,
    .border_speed = 45,
    .thumbnail_blur = 0,
};

overview_state_t overview_state;

static void overview_destroy(void);
static void overview_start_animation(double target);
static void overview_rebuild(Con *keep_selected);
static void box_blur_pass(uint32_t *src, uint32_t *dst, int W, int H, int radius, bool vertical);
static void blur_surface(cairo_surface_t *dst, int W, int H, int amount);
static int draw_workspace(cairo_t *cr, Con *ws, Rect out, double x, double y, double sx, double sy, double alpha, Con *skip, bool placeholders);
static cairo_pattern_t *overview_border_pattern(double x, double y, double w, double h, double alpha);
static void thumb_rect(int i, double p, double *x, double *y, double *w, double *h);
static bool overview_ws_alive(Con *ws);
static aiwr_layer_t *live_for_con(Con *con);
static bool ws_is_scrolling(Con *ws);
typedef struct drop_slot drop_slot_t;
static bool ws_is_scrolling(Con *ws);
static void overview_set_selected(int idx);
static Con *ws_scrolling_con(Con *ws);
static Con *ws_window_at(Con *ws, double ox, double oy);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static double ease_out_cubic(double t) {
    t = 1.0 - clamp01(t);
    return 1.0 - t * t * t;
}

/* Onde a janela cai, dado o ponteiro em coordenadas do output. */
static bool drop_slot_at(workspace_thumbnail_t *t, double ox, double oy, drop_slot_t *out) {
    if (t == NULL || out == NULL || !overview_ws_alive(t->workspace)) return false;
    memset(out, 0, sizeof(*out));
    out->scroll_index = -1;
    out->position = AFTER;

    /* --- rolagem: fatia entre colunas --- */
    Con *sc = ws_scrolling_con(t->workspace);
    if (sc != NULL) {
        const double colw = (scrolling_config.default_width / 100.0) * (double)t->out.width;
        double y = t->out.y, h = t->out.height;
        double edge = 0;
        int index = 0, i = 0;
        bool have = false;

        Con *c;
        TAILQ_FOREACH (c, &(sc->nodes_head), nodes) {
            const double cx = (double)(int32_t)c->rect.x;
            const double cw = (double)c->rect.width;
            if (!have) { y = (double)(int32_t)c->rect.y; h = (double)c->rect.height; have = true; }
            if (ox < cx + cw / 2.0) {
                out->target = c;
                out->position = BEFORE;
                out->scroll_index = index;
                out->rect = (Rect){(uint32_t)(int32_t)cx, (uint32_t)(int32_t)y,
                                   (uint32_t)colw, (uint32_t)h};
                return true;
            }
            edge = cx + cw;
            out->target = c;
            index = ++i;
        }
        out->position = AFTER;
        out->scroll_index = index;
        out->rect = (Rect){(uint32_t)(int32_t)(have ? edge : t->out.x), (uint32_t)(int32_t)y,
                           (uint32_t)colw, (uint32_t)h};
        return true;
    }

    /* --- demais layouts: antes/depois da folha sob o ponteiro --- */
    Con *leaf = ws_window_at(t->workspace, ox, oy);
    if (leaf == NULL) {
        out->target = NULL;
        out->whole = true;
        out->rect = t->workspace->rect;
        return true;
    }

    Con *parent = leaf->parent;
    if (parent == NULL) return false;

    if (parent->layout == L_STACKED || parent->layout == L_TABBED) {
        /* vira mais uma aba: não há fatia espacial que seja honesta */
        out->target = leaf;
        out->position = AFTER;
        out->whole = true;
        out->rect = parent->rect;
        return true;
    }

    const Rect r = leaf->rect;
    /* faixa de 30% da folha no eixo do split, como no tiling_drag */
    if (con_orientation(parent) == VERT) {
        const double mid = (double)(int32_t)r.y + r.height / 2.0;
        const uint32_t band = (uint32_t)fmax(logical_px(6), r.height * 0.3);
        if (oy < mid) {
            out->position = BEFORE;
            out->rect = (Rect){r.x, r.y, r.width, band};
        } else {
            out->position = AFTER;
            out->rect = (Rect){r.x, (uint32_t)(int32_t)((int32_t)r.y + (int32_t)r.height - (int32_t)band),
                               r.width, band};
        }
    } else {
        const double mid = (double)(int32_t)r.x + r.width / 2.0;
        const uint32_t band = (uint32_t)fmax(logical_px(6), r.width * 0.3);
        if (ox < mid) {
            out->position = BEFORE;
            out->rect = (Rect){r.x, r.y, band, r.height};
        } else {
            out->position = AFTER;
            out->rect = (Rect){(uint32_t)(int32_t)((int32_t)r.x + (int32_t)r.width - (int32_t)band),
                               r.y, band, r.height};
        }
    }
    out->target = leaf;
    return true;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void draw_thumbnail_spill(cairo_t *cr, int i, double p) {
    workspace_thumbnail_t *t = &overview_state.thumbnails[i];
    if (t->new_slot || !overview_ws_alive(t->workspace)) return;
    if (!ws_is_scrolling(t->workspace)) return;

    double x, y, w, h;
    thumb_rect(i, p, &x, &y, &w, &h);
    if (y + h < -50 || y > overview_state.screen_height + 50 || w <= 1 || h <= 1) return;

    cairo_save(cr);
    /* recorta só na vertical: a fila pode sair pelos lados, mas não pode
     * invadir a linha da workspace de cima ou de baixo */
    cairo_rectangle(cr, 0, y, overview_state.screen_width, h);
    cairo_clip(cr);
    cairo_push_group(cr);
    Con *skip = overview_state.dragging ? overview_state.drag_con : NULL;
    draw_workspace(cr, t->workspace, t->out, x, y,
                   w / t->out.width, h / t->out.height, 1.0, skip, false);
    cairo_pop_group_to_source(cr);

    cairo_pattern_t *mask = cairo_pattern_create_linear(0, 0, overview_state.screen_width, 0);
    const double a = OVERVIEW_SPILL_ALPHA * p;
    cairo_pattern_add_color_stop_rgba(mask, 0.00, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(mask, 0.18, 0, 0, 0, a);
    cairo_pattern_add_color_stop_rgba(mask, 0.82, 0, 0, 0, a);
    cairo_pattern_add_color_stop_rgba(mask, 1.00, 0, 0, 0, 0);
    cairo_mask(cr, mask);
    cairo_pattern_destroy(mask);

    cairo_restore(cr);
}

static void draw_drop_slot(cairo_t *cr, int i, double p) {
    if (!overview_state.dragging || overview_state.drop_index != i) return;
    workspace_thumbnail_t *t = &overview_state.thumbnails[i];
    if (t->new_slot || !overview_ws_alive(t->workspace)) return;

    double x, y, w, h;
    thumb_rect(i, p, &x, &y, &w, &h);
    if (w <= 1 || h <= 1) return;
    const double sx = w / t->out.width, sy = h / t->out.height;
    const double ox = t->out.x + (overview_state.drag_x - x) / sx;
    const double oy = t->out.y + (overview_state.drag_y - y) / sy;

    drop_slot_t slot;
    if (!drop_slot_at(t, ox, oy, &slot)) return;

    const double rx = x + ((double)(int32_t)slot.rect.x - (double)(int32_t)t->out.x) * sx;
    const double ry = y + ((double)(int32_t)slot.rect.y - (double)(int32_t)t->out.y) * sy;
    const double rw = slot.rect.width * sx, rh = slot.rect.height * sy;
    if (rw < 2 || rh < 2) return;
    const double radius = fmax(4.0, THUMB_RADIUS * p * 0.6);
    const double lw = overview_config.border_width > 0 ? overview_config.border_width : 2;

    cairo_save(cr);
    cairo_rectangle(cr, 0, y, overview_state.screen_width, h);
    cairo_clip(cr);

    color_t f = overview_config.border_start;
    rounded_rect(cr, rx, ry, rw, rh, radius);
    /* "container inteiro" é só destaque, não uma fatia: menos preenchimento
     * para não parecer que a janela vai ocupar tudo */
    cairo_set_source_rgba(cr, f.red, f.green, f.blue, (slot.whole ? 0.16 : 0.32) * p);
    cairo_fill_preserve(cr);

    cairo_pattern_t *pat = overview_border_pattern(rx, ry, rw, rh, p);
    cairo_set_source(cr, pat);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat);
    cairo_restore(cr);
}

static void draw_filter(cairo_t *cr, double p) {
    if (overview_state.filter_len == 0 || p < 0.05) return;

    char buf[80];
    snprintf(buf, sizeof(buf), "  %s", overview_state.filter);
    i3String *s = i3string_from_utf8(buf);
    const int tw = predict_text_width(s);
    const int th = config.font.height;
    const double bw = tw + 28, bh = th + 14;
    const double bx = overview_state.screen_width / 2.0 - bw / 2.0;
    const double by = overview_state.screen_height - bh - 40;

    rounded_rect(cr, bx, by, bw, bh, bh / 2);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.72 * p);
    cairo_fill_preserve(cr);
    cairo_pattern_t *pat = overview_border_pattern(bx, by, bw, bh, p);
    cairo_set_source(cr, pat);
    cairo_set_line_width(cr, overview_config.border_width > 0 ? overview_config.border_width : 2);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat);

    char hex[16];
    snprintf(hex, sizeof(hex), "#FFFFFF%02X", (int)lround(0.95 * p * 255));
    draw_util_text(s, &overview_state.back, draw_util_hex_to_color(hex),
                   draw_util_hex_to_color("#00000000"),
                   (int)(bx + 14), (int)(by + 7), tw + 4);
    i3string_free(s);
}

static void draw_label(const char *utf8, double x, double y, int align, double alpha) {
    if (alpha <= 0.01) return;
    i3String *s = i3string_from_utf8(utf8);
    int tw = predict_text_width(s);
    double lx = (align == 0) ? x - tw / 2.0 : x;
    char hex[16];
    snprintf(hex, sizeof(hex), "#FFFFFF%02X", (int)lround(clamp01(alpha) * 255));
    draw_util_text(s, &overview_state.back, draw_util_hex_to_color(hex),
                   draw_util_hex_to_color("#00000000"), (int)lx, (int)y, tw + 4);
    i3string_free(s);
}

static bool con_in_tree(Con *c, Con *needle) {
    if (c == needle) return true;
    Con *ch;
    TAILQ_FOREACH (ch, &(c->nodes_head), nodes) {
        if (con_in_tree(ch, needle)) return true;
    }
    TAILQ_FOREACH (ch, &(c->floating_head), floating_windows) {
        if (con_in_tree(ch, needle)) return true;
    }
    return false;
}

static bool con_alive(Con *c) {
    return c != NULL && croot != NULL && con_in_tree(croot, c);
}

static bool overview_ws_alive(Con *ws) {
    return con_alive(ws) && ws->type == CT_WORKSPACE;
}

static bool overview_check(xcb_void_cookie_t cookie, const char *what) {
    xcb_generic_error_t *e = xcb_request_check(conn, cookie);
    if (e == NULL) return true;
    ELOG("Overview: %s failed (error %d, major %d)\n", what, e->error_code, e->major_code);
    free(e);
    return false;
}

static aiwr_layer_t *live_find(xcb_window_t win) {
    for (int i = 0; i < overview_state.live.count; i++) {
        if (overview_state.live.items[i].window == win) return &overview_state.live.items[i];
    }
    return NULL;
}

static void live_release(aiwr_layer_t *l) {
    if (l->surface) { cairo_surface_destroy(l->surface); l->surface = NULL; }
    if (l->checked) { xcb_discard_reply(conn, l->cookie.sequence); l->checked = false; }
    if (l->pixmap != XCB_NONE) { xcb_free_pixmap(conn, l->pixmap); l->pixmap = XCB_NONE; }
    l->failed = false;
}

static bool live_enabled(void) {
    return aiwr_capture_available() && overview_config.live_previews;
}

static void live_name(Con *con, bool force) {
    if (con->frame.id == XCB_NONE) return;
    aiwr_layer_t *l = live_find(con->frame.id);
    if (!con->mapped) return;
    if (con->rect.width == 0 || con->rect.height == 0) return;
    if (l == NULL) {
        aiwr_layers_t *L = &overview_state.live;
        L->items = srealloc(L->items, sizeof(aiwr_layer_t) * (L->count + 1));
        l = &L->items[L->count++];
        memset(l, 0, sizeof(*l));
        l->window = con->frame.id;
    } else if (force || l->failed || l->pixmap == XCB_NONE ||
            l->rect.width != con->rect.width ||
            l->rect.height != con->rect.height) {
        live_release(l);
    }

    if (l->pixmap == XCB_NONE) {
        l->pixmap = xcb_generate_id(conn);
        l->cookie = xcb_composite_name_window_pixmap_checked(conn, con->frame.id, l->pixmap);
        l->checked = true;
        l->rect = con->rect;
        l->depth = con->depth;
        l->radius = rounded_corners_radius_for(con);
    }
}

static void live_name_tree(Con *con, bool force) {
    if (con->type != CT_WORKSPACE) live_name(con, force);
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) { live_name_tree(ch, force); }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) { live_name_tree(ch, force); }
}

static aiwr_layer_t *live_for_con(Con *con) {
    if (!live_enabled() || con == NULL || con->frame.id == XCB_NONE) return NULL;
    if (con->mapped) live_name(con, false);
    aiwr_layer_t *l = live_find(con->frame.id);
    if (l == NULL || l->failed || l->pixmap == XCB_NONE) return NULL;
    if (l->checked) {
        l->checked = false;
        xcb_generic_error_t *e = xcb_request_check(conn, l->cookie);
        if (e != NULL) {
            free(e);
            l->failed = true;
            l->pixmap = XCB_NONE;
            return NULL;
        }
    }
    if (l->surface == NULL) {
        xcb_visualtype_t *vt = aiwr_visualtype_for_depth(l->depth);
        if (vt == NULL) { l->failed = true; return NULL; }
        l->surface = cairo_xcb_surface_create(conn, l->pixmap, vt, l->rect.width, l->rect.height);
        if (cairo_surface_status(l->surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(l->surface);
            l->surface = NULL;
            l->failed = true;
            return NULL;
        }
    }
    if (con->mapped) cairo_surface_mark_dirty(l->surface);
    return l;
}

static void collect_frames(Con *con, xcb_window_t **ids, int *n) {
    if (con->frame.id != XCB_NONE) {
        *ids = srealloc(*ids, sizeof(xcb_window_t) * (*n + 1));
        (*ids)[(*n)++] = con->frame.id;
    }
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) { collect_frames(ch, ids, n); }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) { collect_frames(ch, ids, n); }
}

static void live_prune(void) {
    xcb_window_t *ids = NULL;
    int n = 0;
    if (croot) collect_frames(croot, &ids, &n);
    aiwr_layers_t *L = &overview_state.live;
    for (int i = L->count - 1; i >= 0; i--) {
        bool found = false;
        for (int k = 0; k < n && !found; k++) found = (ids[k] == L->items[i].window);
        if (!found) {
            live_release(&L->items[i]);
            L->items[i] = L->items[L->count - 1];
            L->count--;
        }
    }
    free(ids);
}

__attribute__((unused)) static void live_free_all(void) {
    for (int i = 0; i < overview_state.live.count; i++) live_release(&overview_state.live.items[i]);
    free(overview_state.live.items);
    overview_state.live.items = NULL;
    overview_state.live.count = 0;
}

static overview_snapshot_t *snapshot_find(Con *ws) {
    for (int i = 0; i < overview_state.num_snapshots; i++) {
        if (overview_state.snapshots[i].workspace == ws) return &overview_state.snapshots[i];
    }
    return NULL;
}

static void snapshot_release(overview_snapshot_t *s) {
    if (s->surface) { cairo_surface_destroy(s->surface); s->surface = NULL; }
    if (s->pixmap != XCB_NONE) { xcb_free_pixmap(conn, s->pixmap); s->pixmap = XCB_NONE; }
}

static void snapshot_remove_at(int i) {
    snapshot_release(&overview_state.snapshots[i]);
    overview_state.snapshots[i] = overview_state.snapshots[overview_state.num_snapshots - 1];
    overview_state.num_snapshots--;
}

static cairo_surface_t *snapshot_surface(overview_snapshot_t *s) {
    if (s->pixmap == XCB_NONE) return NULL;
    if (s->surface == NULL) {
        s->surface = cairo_xcb_surface_create(conn, s->pixmap, aiwr_find_visualtype(root_screen->root_visual),
                                              s->width, s->height);
        if (cairo_surface_status(s->surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(s->surface);
            s->surface = NULL;
        }
    }
    return s->surface;
}

static void snapshot_invalidate(Con *ws) {
    for (int i = 0;i < overview_state.num_snapshots; i++) {
        if (overview_state.snapshots[i].workspace == ws) {
            snapshot_remove_at(i);
            return;
        }
    }
}

void overview_forget_workspace(Con *ws) {
    for (int i = 0; i < overview_state.num_snapshots; i++) {
        if (overview_state.snapshots[i].workspace == ws) { snapshot_remove_at(i); break; }
    }
    for (int i = 0; i < overview_state.num_thumbnails; i++) {
        if (overview_state.thumbnails[i].workspace == ws) overview_state.thumbnails[i].workspace = NULL;
    }
}

void overview_snapshot_workspace(Con *ws) {
    if (!overview_state.initialized || !overview_config.enabled) return;
    if (overview_state.in_snapshot) return;
    if (ws == NULL || ws->type != CT_WORKSPACE || con_is_internal(ws) || ws->parent == NULL) return;
    if (!overview_ws_alive(ws) || !workspace_is_visible(ws)) return;

    overview_state.in_snapshot = true;
    if (live_enabled()) {
        live_name_tree(ws, true);
        overview_state.in_snapshot = false;
        return;
    }
    if (overview_state.active || overview_state.overlay_visible) {
        overview_state.in_snapshot = false;
        return;
    }

    Con *output = con_get_output(ws);
    if (output == NULL || output->rect.width == 0 || output->rect.height == 0) {
        overview_state.in_snapshot = false;
        return;
    }
    Rect r = output->rect;
    overview_snapshot_t *s = snapshot_find(ws);
    if (s == NULL) {
        overview_state.snapshots = srealloc(overview_state.snapshots,
                                            sizeof(overview_snapshot_t) * (overview_state.num_snapshots + 1));
        s = &overview_state.snapshots[overview_state.num_snapshots++];
        memset(s, 0, sizeof(*s));
        s->workspace = ws;
    }
    if (s->pixmap != XCB_NONE && (s->width != (int)r.width || s->height != (int)r.height)) snapshot_release(s);
    if (s->pixmap == XCB_NONE) {
        s->pixmap = xcb_generate_id(conn);
        xcb_create_pixmap(conn, root_screen->root_depth, s->pixmap, root, r.width, r.height);
        s->width = r.width;
        s->height = r.height;
    }
    xcb_copy_area(conn, root, s->pixmap, overview_state.copy_gc, r.x, r.y, 0, 0, r.width, r.height);
    overview_state.in_snapshot = false;
}

static void overview_prune_snapshots(void) {
    for (int i = overview_state.num_snapshots - 1; i >= 0; i--) {
        if (!overview_ws_alive(overview_state.snapshots[i].workspace)) snapshot_remove_at(i);
    }
}

// Tree draw
static Con *ws_scrolling_con(Con *ws) {
    if (ws == NULL) return NULL;
    if (ws->layout == L_SCROLLING) return ws;
    Con *c;
    TAILQ_FOREACH (c, &(ws->nodes_head), nodes) {
        Con *r = ws_scrolling_con(c);
        if (r != NULL) return r;
    }
    return NULL;
}

static bool ws_is_scrolling(Con *ws) {
    return ws_scrolling_con(ws) != NULL;
}

static int count_windows(Con *con) {
    int n = (con->window != NULL) ? 1 : 0;
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) { n += count_windows(ch); }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) { n += count_windows(ch); }
    return n;
}

static void draw_placeholder(cairo_t *cr, Con *con, Rect out, double x, double y, double sx, double sy, double alpha) {
    double rx = x + ((double)con->rect.x - (double)out.x) * sx;
    double ry = y + ((double)con->rect.y - (double)out.y) * sy;
    double rw = con->rect.width * sx, rh = con->rect.height * sy;
    rounded_rect(cr, rx + 1, ry + 1, rw - 2, rh - 2, 4);
    cairo_set_source_rgba(cr, 0.22, 0.24, 0.30, alpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.25 * alpha);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (con->window && con->window->name && rh > config.font.height + 8 && rw > 40) {
        int tw = predict_text_width(con->window->name);
        char hex[16];
        snprintf(hex, sizeof(hex), "#FFFFFF%02X", (int)lround(0.75 * alpha * 255));
        draw_util_text(con->window->name, &overview_state.back, draw_util_hex_to_color(hex),
                       draw_util_hex_to_color("#00000000"), (int)(rx + 6), (int)(ry + 4),
                       (int)fmin(tw + 4, rw - 12));
    }
}

static int draw_tree(cairo_t *cr, Con *con, Rect out, double x, double y, double sx, double sy,
                     double alpha, Con *skip, bool placeholders) {
    if (con == NULL || con == skip) return 0;
    int n = 0;
    if (con->type != CT_WORKSPACE && con->type != CT_FLOATING_CON && con->window != NULL) {
        aiwr_layer_t *l = live_for_con(con);
        if (l != NULL) {
            double rx = x + ((double)(int32_t)con->rect.x - (double)(int32_t)out.x) * sx;
            double ry = y + ((double)(int32_t)con->rect.y - (double)(int32_t)out.y) * sy;
            /* surface xcb guarda o que leu da última vez; sem isto a preview
             * congela no primeiro frame */
            cairo_surface_mark_dirty(l->surface);
            cairo_save(cr);
            cairo_translate(cr, rx, ry);
            cairo_scale(cr, sx * (double)con->rect.width / l->rect.width,
                        sy * (double)con->rect.height / l->rect.height);
            cairo_set_source_surface(cr, l->surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_rectangle(cr, 0, 0, l->rect.width, l->rect.height);
            cairo_clip(cr);
            if (alpha >= 0.999 && !aiwr_capture_external()) {
                /* sem compositor externo o servidor ignora o alpha das janelas
                 * ARGB e mostra o RGB pré-multiplicado. SOURCE reproduz isso. */
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_paint(cr);
            } else {
                cairo_paint_with_alpha(cr, alpha);
            }
            cairo_restore(cr);
            n++;
        } else if (placeholders) {
            draw_placeholder(cr, con, out, x, y, sx, sy, alpha);
            n++;
        }
        return n;
    }
    bool deco_parent = con->type == CT_FLOATING_CON ||
                       (con->type != CT_WORKSPACE && (con->layout == L_STACKED || con->layout == L_TABBED));
    if (deco_parent) {
        aiwr_layer_t *l = live_for_con(con);
        if (l != NULL) {
            cairo_surface_mark_dirty(l->surface);
            cairo_save(cr);
            cairo_translate(cr, x + ((double)(int32_t)con->rect.x - (double)(int32_t)out.x) * sx,
                           y + ((double)(int32_t)con->rect.y - (double)(int32_t)out.y) * sy);
            cairo_scale(cr, sx * (double)con->rect.width / l->rect.width,
                        sy * (double)con->rect.height / l->rect.height);
            cairo_set_source_surface(cr, l->surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_rectangle(cr, 0, 0, l->rect.width, l->rect.height);
            cairo_clip(cr);
            if (alpha >= 0.999 && !aiwr_capture_external()) {
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_paint(cr);
            } else {
                cairo_paint_with_alpha(cr, alpha);
            }
            cairo_restore(cr);
        }
    }
    if (con->type != CT_WORKSPACE && con->type != CT_FLOATING_CON && (con->layout == L_STACKED || con->layout == L_TABBED)) {
        Con *f = TAILQ_FIRST(&(con->focus_head));
        return n + (f ? draw_tree(cr, f, out, x, y, sx, sy, alpha, skip, placeholders) : 0);
    }
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) {
        n += draw_tree(cr, ch, out, x, y, sx, sy, alpha, skip, placeholders);
    }
    return n;
}

static int draw_workspace(cairo_t *cr, Con *ws, Rect out, double x, double y, double sx, double sy,
                          double alpha, Con *skip, bool placeholders) {
    Con *fs = con_get_fullscreen_con(ws, CF_OUTPUT);
    if (fs == NULL) fs = con_get_fullscreen_con(ws, CF_GLOBAL);
    int n = 0;
    if (fs != NULL) {
        n += draw_tree(cr, fs, out, x, y, sx, sy, alpha, skip, placeholders);
        return n;
    }
    n += draw_tree(cr, ws, out, x, y, sx, sy, alpha, skip, placeholders);
    Con *fl;
    TAILQ_FOREACH (fl, &(ws->floating_head), floating_windows) {
        n += draw_tree(cr, fl, out, x, y, sx, sy, alpha, skip, placeholders);
    }
    return n;
}

static int draw_workspace_blurred(cairo_t *cr, Con *ws, Rect out, double x, double y,
                                   double sx, double sy, double alpha, Con *skip, int amount) {
    if (amount <= 0) {
        return draw_workspace(cr, ws, out, x, y, sx, sy, alpha, skip, false);
    }
    const int k = 2 + amount / 12;
    const int sw = (int)fmax(1.0, (out.width * sx) / k);
    const int sh = (int)fmax(1.0, (out.height * sy) / k);

    cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t *c = cairo_create(small);
    const int n = draw_workspace(c, ws, out, 0, 0, sx / k, sy / k, 1.0, skip, false);
    cairo_destroy(c);
    draw_workspace(c, ws, out, 0, 0, sx / k, sy / k, 1.0, skip, false);
    cairo_destroy(c);
    cairo_surface_flush(small);

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, small, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_surface_destroy(small);
    return n;
}

static cairo_pattern_t *overview_border_pattern(double x, double y, double w, double h, double alpha) {
    color_t a = overview_config.border_start, b = overview_config.border_end;
    double ang = overview_state.border_phase * M_PI / 180.0;
    double dx = cos(ang), dy = sin(ang);
    double L = (fabs(dx) * w + fabs(dy) * h) / 2.0;
    double cx = x + w / 2.0, cy = y + h / 2.0;
    cairo_pattern_t *p = cairo_pattern_create_linear(cx - dx * L, cy - dy * L, cx + dx * L, cy + dy * L);
    cairo_pattern_add_color_stop_rgba(p, 0.0, a.red, a.green, a.blue, a.alpha * alpha);
    cairo_pattern_add_color_stop_rgba(p, 0.5, b.red, b.green, b.blue, b.alpha * alpha);
    cairo_pattern_add_color_stop_rgba(p, 1.0, a.red, a.green, a.blue, a.alpha * alpha);
    return p;
}

static double frand(void) { return rand() / (double)RAND_MAX; }

static void particles_init(void) {
    free(overview_state.particles);
    overview_state.particles = NULL;
    overview_state.num_particles = 0;
    overview_state.particles_off = false;
    overview_state.slow_frames = 0;
    int n = overview_config.particles;
    if (n <= 0) return;
    if (n > MAX_PARTICLES) n = MAX_PARTICLES;
    overview_state.particles = scalloc(n, sizeof(overview_particle_t));
    overview_state.num_particles = n;
    double W = overview_state.screen_width, H = overview_state.screen_height;
    for (int i = 0; i < n; i++) {
        overview_particle_t *q = &overview_state.particles[i];
        double u = frand();
        q->x = frand() * W;
        q->y = frand() * H;
        q->r = 1.0 + 2.2 * u;
        q->a = 0.12 + 0.30 * u;
        q->vx = (frand() - 0.5) * 16.0;
        q->vy = -(5.0 + 16.0 * u);
        q->ph = frand() * 2.0 * M_PI;
    }
}

static void particles_step(double dt) {
    if (overview_state.particles_off) return;
    float W = overview_state.screen_width, H = overview_state.screen_height;
    for (int i = 0; i < overview_state.num_particles; i++) {
        overview_particle_t *q = &overview_state.particles[i];
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->ph += dt * 1.6;
        if (q->y < -8) { q->y = H + 8; q->x = frand() * W; }
        if (q->x < -8) q->x = W + 8;
        else if (q->x > W + 8) q->x = -8;
    }
}

static void draw_particles(cairo_t *cr, double p) {
    if (overview_state.particles_off || overview_state.num_particles == 0 || p < 0.05) return;
    cairo_save(cr);
    for (int i = 0; i < overview_state.num_particles; i++) {
        overview_particle_t *q = &overview_state.particles[i];
        double al = q->a * (0.6 + 0.4 * sin(q->ph)) * p;
        cairo_set_source_rgba(cr, 1, 1, 1, al);
        cairo_arc(cr, q->x, q->y, q->r, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static Con *leaf_at(Con *con, double ox, double oy) {
    if (con == NULL) return NULL;
    if (con->type != CT_WORKSPACE && con->type != CT_FLOATING_CON && con->window != NULL) {
        Rect r = con->rect;
        const double rx = (double)(int32_t)r.x;
        const double ry = (double)(int32_t)r.y;
        bool in = (ox >= rx && ox < rx + (double)r.width &&
                   oy >= ry && oy < ry + (double)r.height);
        ELOG("[leaf] cmp ox=%.0f oy=%.0f vs %.0f,%.0f %ux%u -> %d\n",
             ox, oy, rx, ry, r.width, r.height, in);
        return in ? con : NULL;
    }
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) {
        Con *r = leaf_at(ch, ox, oy);
        if (r) return r;
    }
    return NULL;
}

static Con *ws_window_at(Con *ws, double ox, double oy) {
    Con *fs = con_get_fullscreen_con(ws, CF_OUTPUT);
    if (fs == NULL) fs = con_get_fullscreen_con(ws, CF_GLOBAL);
    if (fs != NULL) return leaf_at(fs, ox, oy);
    Con *fl;
    TAILQ_FOREACH_REVERSE (fl, &(ws->floating_head), floating_head, floating_windows) {
        Con *r = leaf_at(fl, ox, oy);
        if (r) return r;
    }
    return leaf_at(ws, ox, oy);
}

static Rect overview_focus_area(void) {
    Con *out = focused ? con_get_output(focused) : NULL;
    if (out) return out->rect;
    return (Rect){0, 0, overview_state.screen_width, overview_state.screen_height};
}

static void overview_calculate_thumbnail_positions(void) {
    int n = overview_state.num_thumbnails;
    if (n == 0) return;
    Rect area = overview_focus_area();
    double spacing = overview_config.spacing;
    double scale = overview_config.thumbnail_scale / 100.0;
    if (scale < 0.1) scale = 0.1;
    if (scale > 0.9) scale = 0.9;
    double label_w = overview_config.show_workspace_names ? config.font.height * 9.0 : 0.0;
    double center_x = area.x + area.width / 2.0 - label_w / 3.0;

    double y = 0;
    for (int i = 0; i < n; i++) {
        workspace_thumbnail_t *t = &overview_state.thumbnails[i];
        t->width = t->out.width * scale;
        t->height = t->out.height * scale;
        t->x = center_x - t->width / 2.0;
        t->y = y;
        y += t->height + spacing;
    }
}

static double scroll_for(int idx) {
    if (idx < 0 || idx >= overview_state.num_thumbnails) return 0;
    Rect area = overview_focus_area();
    workspace_thumbnail_t *t = &overview_state.thumbnails[idx];
    return t->y + t->height / 2.0 - (area.y + area.height / 2.0);
}

static void thumb_rect(int i, double p, double *x, double *y, double *w, double *h) {
    workspace_thumbnail_t *t = &overview_state.thumbnails[i];
    int org = overview_state.origin_index;
    if (org < 0 || org >= overview_state.num_thumbnails) org = 0;
    double lx = t->x, ly = t->y - overview_state.scroll, lw = t->width, lh = t->height;
    double gap0 = overview_config.spacing / (overview_config.thumbnail_scale / 100.0);
    double fx = t->out.x, fw = t->out.width, fh = t->out.height;
    double fy = t->out.y + (i - org) * (t->out.height + gap0);
    *x = lerp(fx, lx, p);
    *y = lerp(fy, ly, p);
    *w = lerp(fw, lw, p);
    *h = lerp(fh, lh, p);
}

static int overview_thumbnail_at(double px, double py) {
    double p = clamp01(overview_state.animation_progress);
    for (int i = 0; i < overview_state.num_thumbnails; i++) {
        double x, y, w, h;
        thumb_rect(i, p, &x, &y, &w, &h);
        if (py < y || py >= y + h) continue;
        if (px >= x && px < x + w) return i;
        /* a fila de uma workspace de rolagem passa da thumbnail: a faixa
         * inteira pertence a ela */
        if (!overview_state.thumbnails[i].new_slot &&
            ws_is_scrolling(overview_state.thumbnails[i].workspace)) {
            return i;
        }
    }
    return -1;
}

static Con *overview_window_at(int idx, double px, double py) {
    if (idx < 0 || idx >= overview_state.num_thumbnails) return NULL;
    workspace_thumbnail_t *t = &overview_state.thumbnails[idx];
    if (t->new_slot || !overview_ws_alive(t->workspace)) return NULL;
    double x, y, w, h;
    thumb_rect(idx, clamp01(overview_state.animation_progress), &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return NULL;
    double ox = t->out.x + (px - x) * (t->out.width / w);
    double oy = t->out.y + (py - y) * (t->out.height / h);
    Con *r = ws_window_at(t->workspace, ox, oy);
    ELOG("[hit] px=%.0f idx=%d thumb=%.0f..%.0f ox=%.0f oy=%.0f -> %s\n",
         px, idx, x, x + w, ox, oy, r ? "HIT" : "miss");
    return r;
}

static void overview_free_background(void) {
    if (overview_state.bg_full) { cairo_surface_destroy(overview_state.bg_full); overview_state.bg_full = NULL; }
    if (overview_state.wp_thumb) { cairo_surface_destroy(overview_state.wp_thumb); overview_state.wp_thumb = NULL; }
    if (overview_state.bg_pixmap != XCB_NONE) { xcb_free_pixmap(conn, overview_state.bg_pixmap); overview_state.bg_pixmap = XCB_NONE; }
    if (overview_state.wp_thumb_pixmap != XCB_NONE) { xcb_free_pixmap(conn, overview_state.wp_thumb_pixmap); overview_state.wp_thumb_pixmap = XCB_NONE; }
    overview_state.wp_thumb_w = overview_state.wp_thumb_h = 0;
}

static void overview_bake_background(void) {
    overview_free_background();
    int W = overview_state.screen_width, H = overview_state.screen_height;
    xcb_visualtype_t *vt = aiwr_find_visualtype(root_screen->root_visual);
    if (vt == NULL || W <= 0 || H <= 0) return;

    overview_state.bg_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, root_screen->root_depth, overview_state.bg_pixmap, root, W, H);
    overview_state.bg_full = cairo_xcb_surface_create(conn, overview_state.bg_pixmap, vt, W, H);
    cairo_t *c = cairo_create(overview_state.bg_full);
    if (overview_state.wallpaper) {
        int iw = cairo_image_surface_get_width(overview_state.wallpaper);
        int ih = cairo_image_surface_get_height(overview_state.wallpaper);
        if (iw > 0 && ih > 0) {
            double k = fmax((double)W / iw, (double)H / ih);
            cairo_save(c);
            cairo_translate(c, (W - iw * k) / 2.0, (H -ih * k) / 2.0);
            cairo_scale(c, k, k);
            cairo_set_source_surface(c, overview_state.wallpaper, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_BILINEAR);
            cairo_paint(c);
            cairo_restore(c);
        }
        cairo_surface_flush(overview_state.bg_full);
        blur_surface(overview_state.bg_full, W, H, overview_config.background_blur);
    } else {
        cairo_set_source_rgb(c, 0.04, 0.04, 0.06);
        cairo_paint(c);
        cairo_surface_flush(overview_state.bg_full);
    }
    cairo_destroy(c);

    double s = overview_config.thumbnail_scale / 100.0;
    if (s < 0.1) s = 0.1;
    if (s > 0.9) s = 0.9;
    int tw = (int)ceil(W * s), th = (int)ceil(H * s);
    overview_state.wp_thumb_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, root_screen->root_depth, overview_state.wp_thumb_pixmap, root, tw, th);
    overview_state.wp_thumb = cairo_xcb_surface_create(conn, overview_state.wp_thumb_pixmap, vt, tw, th);
    overview_state.wp_thumb_w = tw;
    overview_state.wp_thumb_h = th;
    c = cairo_create(overview_state.wp_thumb);
    cairo_scale(c, (double)tw / W, (double)th / H);
    cairo_set_source_surface(c, overview_state.bg_full, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_BILINEAR);
    cairo_paint(c);
    cairo_surface_flush(overview_state.wp_thumb);
    cairo_destroy(c);
}

static cairo_surface_t *overview_config_wallpaper(void) {
    if (overview_config.wallpaper_path == NULL) return NULL;
    cairo_surface_t *s = cairo_image_surface_create_from_png(overview_config.wallpaper_path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        ELOG("Overview: could not load wallpaper '%s'\n", overview_config.wallpaper_path);
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

static void overview_park_overlay(void) {
    if (overview_state.overlay_window == XCB_NONE) return;
    uint32_t v[] = {(uint32_t)(int32_t)(-overview_state.ov_w - 64), (uint32_t)(int32_t)(-overview_state.ov_h - 64)};
    xcb_configure_window(conn, overview_state.overlay_window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    overview_state.overlay_visible = false;
}

static void overview_release_overlay(void) {
    if (overview_state.front.id != XCB_NONE) draw_util_surface_free(conn, &overview_state.front);
    if (overview_state.back.id != XCB_NONE) draw_util_surface_free(conn, &overview_state.back);
    memset(&overview_state.front, 0, sizeof(overview_state.front));
    memset(&overview_state.back, 0, sizeof(overview_state.back));
    if (overview_state.overlay_window != XCB_NONE) {
        xcb_destroy_window(conn, overview_state.overlay_window);
        overview_state.overlay_window = XCB_NONE;
    }
    if (overview_state.back_pixmap != XCB_NONE) { xcb_free_pixmap(conn, overview_state.back_pixmap); overview_state.back_pixmap = XCB_NONE; }
    if (overview_state.colormap != XCB_NONE) { xcb_free_colormap(conn, overview_state.colormap); overview_state.colormap = XCB_NONE; }
    overview_state.overlay_ready = false;
    overview_state.overlay_visible = false;
    overview_state.ov_w = overview_state.ov_h = 0;
}

static bool overview_ensure_overlay(void) {
    int W = overview_state.screen_width, H = overview_state.screen_height;
    if (overview_state.overlay_ready && (overview_state.ov_w != W || overview_state.ov_h != H)) overview_release_overlay();
    if (overview_state.overlay_ready) return true;

    uint8_t depth = 32;
    xcb_visualid_t visual = get_visualid_by_depth(32);
    if (visual == 0) {
        depth = root_screen->root_depth;
        visual = root_screen->root_visual;
    }
    xcb_visualtype_t *vt = aiwr_find_visualtype(visual);
    if (vt == NULL) { ELOG("Overview: visualtype 0x%x not found\n", visual); return false; }
    overview_state.depth = depth;
    overview_state.visual = visual;

    overview_state.colormap = xcb_generate_id(conn);
    if (!overview_check(xcb_create_colormap_checked(conn, XCB_COLORMAP_ALLOC_NONE, overview_state.colormap, root, visual),
                        "CreateColormap")) {
        overview_state.colormap = XCB_NONE;
        return false;
    }
    overview_state.back_pixmap = xcb_generate_id(conn);
    if (!overview_check(xcb_create_pixmap_checked(conn, depth, overview_state.back_pixmap, root, W, H), "CreatePixmap(back)")) {
        overview_state.back_pixmap = XCB_NONE;
        return false;
    }
    draw_util_surface_init(conn, &overview_state.back, overview_state.back_pixmap, vt, W, H);
    if (overview_state.back.cr == NULL || cairo_status(overview_state.back.cr) != CAIRO_STATUS_SUCCESS) {
        ELOG("Overview: cairo back surface failed\n");
        return false;
    }
    cairo_set_source_rgb(overview_state.back.cr, 0.04, 0.04, 0.06);
    cairo_paint(overview_state.back.cr);
    cairo_surface_flush(overview_state.back.surface);

    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                    XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values[] = {
        overview_state.back_pixmap, 0, 1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
        overview_state.colormap};
    overview_state.overlay_window = xcb_generate_id(conn);
    if (!overview_check(xcb_create_window_checked(conn, depth, overview_state.overlay_window, root,
                                                  (int16_t)(-W - 64), (int16_t)(-H - 64), W, H, 0,
                                                  XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, values),
                        "CreateWindow")) {
        overview_state.overlay_window = XCB_NONE;
        return false;
    }
    aiwr_set_overlay_hints(overview_state.overlay_window, "i3-aiwr overview");
    xcb_intern_atom_reply_t *op = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, strlen("_NET_WM_WINDOW_OPACITY"), "_NET_WM_WINDOW_OPACITY"), NULL);
    if (op != NULL) {
        uint32_t full = 0xffffffffu;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, overview_state.overlay_window, op->atom, XCB_ATOM_CARDINAL, 32, 1, &full);
        free(op);
    }
    draw_util_surface_init(conn, &overview_state.front, overview_state.overlay_window, vt, W, H);
    xcb_map_window(conn, overview_state.overlay_window);
    overview_state.overlay_ready = true;
    overview_state.overlay_visible = false;
    overview_state.ov_w = W;
    overview_state.ov_h = H;
    xcb_flush(conn);
    OVLOG("overlay created (%dx%d, depth %d)\n", W, H, depth);
    return true;
}

static bool overview_show_overlay(void) {
    if (!overview_ensure_overlay()) return false;
    overview_render();
    uint32_t v[] = {0, 0, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(conn, overview_state.overlay_window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, v);
    xcb_flush(conn);
    ELOG("[i3-aiwr] Overview: overlay 0x%08x moved to 0,0 + raised\n",
            overview_state.overlay_window);
    overview_state.overlay_visible = true;

    xcb_grab_keyboard_reply_t *grab = xcb_grab_keyboard_reply(
        conn, xcb_grab_keyboard(conn, 0, overview_state.overlay_window, XCB_CURRENT_TIME,
                                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    overview_state.keyboard_grabbed = (grab && grab->status == XCB_GRAB_STATUS_SUCCESS);
    free(grab);
    overview_state.focus_stolen = false;
    if (!overview_state.keyboard_grabbed) {
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, overview_state.overlay_window, XCB_CURRENT_TIME);
        overview_state.focus_stolen = true;
    }
    xcb_flush(conn);
    return true;
}

/* substring case-insensitive; needle vazio casa com tudo */
static bool str_has_ci(const char *hay, const char *needle) {
    if (hay == NULL) return false;
    if (needle == NULL || *needle == '\0') return true;
    const size_t nl = strlen(needle);
    for (const char *p = hay; *p != '\0'; p++) {
        if (strncasecmp(p, needle, nl) == 0) return true;
    }
    return false;
}

static bool con_matches_filter(Con *con, const char *q) {
    if (con == NULL) return false;
    if (con->window != NULL) {
        if (con->window->name != NULL &&
            str_has_ci(i3string_as_utf8(con->window->name), q)) return true;
        if (str_has_ci(con->window->class_class, q)) return true;
        if (str_has_ci(con->window->class_instance, q)) return true;
    }
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) {
        if (con_matches_filter(ch, q)) return true;
    }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) {
        if (con_matches_filter(ch, q)) return true;
    }
    return false;
}

static bool thumb_matches(int i) {
    if (overview_state.filter_len == 0) return true;
    if (i < 0 || i >= overview_state.num_thumbnails) return false;
    workspace_thumbnail_t *t = &overview_state.thumbnails[i];
    /* o slot "nova workspace" não tem o que casar */
    if (t->new_slot || !overview_ws_alive(t->workspace)) return false;
    if (str_has_ci(t->workspace->name, overview_state.filter)) return true;
    return con_matches_filter(t->workspace, overview_state.filter);
}

/* Anda para o próximo/anterior que casa com o filtro. */
static void overview_step_match(int dir) {
    const int n = overview_state.num_thumbnails;
    if (n <= 0) return;
    int i = overview_state.selected_index;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (thumb_matches(i)) {
            overview_set_selected(i);
            return;
        }
    }
}

static void overview_filter_changed(void) {
    if (overview_state.filter_len > 0 && !thumb_matches(overview_state.selected_index)) {
        for (int i = 0; i < overview_state.num_thumbnails; i++) {
            if (thumb_matches(i)) { overview_set_selected(i); break; }
        }
    }
    overview_render();
}

static void overview_filter_clear(void) {
    overview_state.filter[0] = '\0';
    overview_state.filter_len = 0;
}

static void draw_thumbnail(cairo_t *cr, int i, double p) {
    workspace_thumbnail_t *t = &overview_state.thumbnails[i];
    const bool selected = (i == overview_state.selected_index);
    bool hovered = (i == overview_state.hover_index);
    bool drop_target = overview_state.dragging && i == overview_state.drop_index;
    bool alive = !t->new_slot && overview_ws_alive(t->workspace);
    double x, y, w, h;
    thumb_rect(i, p, &x, &y, &w, &h);
    if (y + h < -50 || y > overview_state.screen_height + 50 || w <= 1 || h <= 1) return;
    double radius = THUMB_RADIUS * p;
    double ui = p;

    if (t->new_slot) {
        double dash[] = {8, 6};
        rounded_rect(cr, x, y, w, h, THUMB_RADIUS);
        cairo_set_source_rgba(cr, 1, 1, 1, (drop_target ? 0.10 : 0.04) * ui);
        cairo_fill_preserve(cr);
        cairo_set_dash(cr, dash, 2, 0);
        if (drop_target) {
            cairo_pattern_t *pat = overview_border_pattern(x, y, w, h, ui);
            cairo_set_source(cr, pat);
            cairo_set_line_width(cr, overview_config.border_width > 0 ? overview_config.border_width : 2);
            cairo_stroke(cr);
            cairo_pattern_destroy(pat);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.35 * ui);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
        cairo_set_dash(cr, NULL, 0, 0);
        double cx = x + w / 2, cy = y + h / 2, r = fmin(w, h) * 0.07;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.5 * ui);
        cairo_set_line_width(cr, 2.5);
        cairo_move_to(cr, cx - r, cy); cairo_line_to(cr, cx + r, cy);
        cairo_move_to(cr, cx, cy - r); cairo_line_to(cr, cx, cy + r);
        cairo_stroke(cr);
        if (overview_config.show_workspace_names) {
            draw_label(overview_state.dragging ? "Solte para criar workspace" : "Nova workspace",
                       x + w + 18, y + h / 2.0 - config.font.height / 2.0, 1, 0.45 * ui);
        }
        return;
    }

    /* sombra */
    for (int s = 3; s >= 1; s--) {
        rounded_rect(cr, x - s * 2, y + 6 + s * 2, w + s * 4, h + s * 2, radius + s * 2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12 * ui);
        cairo_fill(cr);
    }

    cairo_save(cr);
    rounded_rect(cr, x, y, w, h, radius);
    cairo_clip(cr);

    if (overview_state.bg_full) {
        cairo_surface_t *src = overview_state.bg_full;
        double k = 1.0;
        if (overview_state.wp_thumb && w <= overview_state.wp_thumb_w * 1.25) {
            src = overview_state.wp_thumb;
            k = overview_state.wp_thumb_w / (double)overview_state.screen_width;
        }
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, w / (t->out.width * k), h / (t->out.height * k));
        cairo_set_source_surface(cr, src, -(double)t->out.x * k, -(double)t->out.y * k);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0.10, 0.11, 0.15);
        cairo_paint(cr);
    }

    int drawn = 0;
    if (alive) {
        Con *skip = overview_state.dragging ? overview_state.drag_con : NULL;
        const int blur = selected ? 0 : overview_config.thumbnail_blur;
        drawn = draw_workspace_blurred(cr, t->workspace, t->out, x, y, w / t->out.width, h / t->out.height, 1.0, skip, false);
        if (drawn == 0 && count_windows(t->workspace) > 0) {
            overview_snapshot_t *snap = snapshot_find(t->workspace);
            cairo_surface_t *surf = snap ? snapshot_surface(snap) : NULL;
            if (surf) {
                cairo_save(cr);
                cairo_translate(cr, x, y);
                cairo_scale(cr, w / snap->width, h / snap->height);
                cairo_set_source_surface(cr, surf, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
                drawn = 1;
            } else {
                drawn = draw_workspace(cr, t->workspace, t->out, x, y, w / t->out.width, h / t->out.height, 1.0, skip, true);
            }
        }
    }
    if (overview_state.filter_len > 0 && !thumb_matches(i)) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.65 * ui);
    } else if (!selected) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.22 * ui);
        cairo_paint(cr);
    }

    cairo_restore(cr);

    double bwid = overview_config.border_width > 0 ? overview_config.border_width : 1;
    if (drop_target || selected) {
        double lw = drop_target ? bwid + 1 : bwid;
        rounded_rect(cr, x - lw / 2, y - lw / 2, w + lw, h + lw, radius + lw / 2);
        cairo_pattern_t *pat = overview_border_pattern(x - lw, y - lw, w + 2 * lw, h + 2 * lw, ui);
        cairo_set_source(cr, pat);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
        cairo_pattern_destroy(pat);
        if (drop_target) {
            color_t hc = overview_config.border_end;
            rounded_rect(cr, x - lw - 3, y - lw - 3, w + 2 * lw + 6, h + 2 * lw + 6, radius + lw + 3);
            cairo_set_source_rgba(cr, hc.red, hc.green, hc.blue, 0.35 * ui);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        }
    } else {
        color_t ic = overview_config.border_inactive;
        double al = ic.alpha * (hovered ? 2.5 : 1.0) * ui;
        rounded_rect(cr, x - 0.5, y - 0.5, w + 1, h + 1, radius + 0.5);
        cairo_set_source_rgba(cr, ic.red, ic.green, ic.blue, al > 1 ? 1 : al);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }

    if (alive && t->workspace->urgent) {
        rounded_rect(cr, x - 2, y - 2, w + 4, h + 4, radius + 2);
        cairo_set_source_rgba(cr, 1.0, 0.45, 0.2, 0.7 * ui);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }

    if (i < 9 && ui > 0.05) {
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        i3String *s = i3string_from_utf8(num);
        int tw = predict_text_width(s);
        int th = config.font.height;
        double bw = fmax(th + 8, tw + 14), bh = th + 6;
        double bx = x + 10, by = y + 10;
        rounded_rect(cr, bx, by, bw, bh, bh / 2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * ui);
        cairo_fill(cr);
        char hex[16];
        snprintf(hex, sizeof(hex), "#FFFFFF%02X", (int)lround(0.9 * ui * 255));
        draw_util_text(s, &overview_state.back, draw_util_hex_to_color(hex), draw_util_hex_to_color("#00000000"),
                       (int)(bx + (bw - tw) / 2), (int)(by + 3), tw + 4);
        i3string_free(s);
    }

    /* nome à direita */
    if (overview_config.show_workspace_names && alive && ui > 0.05) {
        int nwin = count_windows(t->workspace);
        char info[64];
        snprintf(info, sizeof(info), nwin == 1 ? "%d janela" : "%d janelas", nwin);
        double lx = x + w + 18;
        double ly = y + h / 2.0 - config.font.height;
        draw_label(t->workspace->name, lx, ly, 1, (selected ? 0.95 : 0.6) * ui);
        draw_label(info, lx, ly + config.font.height + 4, 1, 0.4 * ui);
    }
}

static void draw_drag_ghost(cairo_t *cr) {
    if (!overview_state.dragging || !con_alive(overview_state.drag_con)) return;
    Con *con = overview_state.drag_con;
    int src = overview_state.drag_src_index;
    if (src < 0 || src >= overview_state.num_thumbnails) return;
    workspace_thumbnail_t *t = &overview_state.thumbnails[src];
    double sx = t->width / t->out.width, sy = t->height / t->out.height;
    double gw = con->rect.width * sx * 1.05, gh = con->rect.height * sy * 1.05;
    double gx = overview_state.drag_x - overview_state.grab_fx * gw;
    double gy = overview_state.drag_y - overview_state.grab_fy * gh;

    for (int s = 3; s >= 1; s--) {
        rounded_rect(cr, gx - s * 3, gy + 8 + s * 3, gw + s * 6, gh + s * 3, 6 + s * 3);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.14);
        cairo_fill(cr);
    }
    cairo_save(cr);
    rounded_rect(cr, gx, gy, gw, gh, 6);
    cairo_clip(cr);
    aiwr_layer_t *l = live_for_con(con);
    if (l != NULL) {
        cairo_save(cr);
        cairo_translate(cr, gx, gy);
        cairo_scale(cr, gw / l->rect.width, gh / l->rect.height);
        cairo_set_source_surface(cr, l->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint_with_alpha(cr, 0.92);
        cairo_restore(cr);
    } else {
        Rect fake = {con->rect.x, con->rect.y, con->rect.width, con->rect.height};
        draw_placeholder(cr, con, fake, gx, gy, gw / con->rect.width, gh / con->rect.height, 0.92);
    }
    cairo_restore(cr);
    double lw = overview_config.border_width > 0 ? overview_config.border_width : 2;
    rounded_rect(cr, gx - lw / 2, gy - lw / 2, gw + lw, gh + lw, 6 + lw / 2);
    cairo_pattern_t *pat = overview_border_pattern(gx - lw, gy - lw, gw + 2 * lw, gh + 2 * lw, 1.0);
    cairo_set_source(cr, pat);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
    cairo_pattern_destroy(pat);
}

void overview_render(void) {
    if (!overview_state.active || overview_state.back.cr == NULL) return;
    cairo_t *cr = overview_state.back.cr;
    double p = clamp01(overview_state.animation_progress);
    double W = overview_state.screen_width, H = overview_state.screen_height;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (overview_state.bg_full) {
        cairo_set_source_surface(cr, overview_state.bg_full, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgb(cr, 0.04, 0.04, 0.06);
        cairo_paint(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.04, (overview_config.background_opacity / 100.0) * p);
    cairo_paint(cr);
    cairo_restore(cr);
    draw_particles(cr, p);

    Rect area = overview_focus_area();
    double acx = area.x + area.width / 2.0;
    (void)W; (void)H;

    int sel = overview_state.selected_index;
    for (int i = 0;i < overview_state.num_thumbnails;i++) {
        draw_thumbnail_spill(cr, i, p);
    }
    for (int i = 0; i < overview_state.num_thumbnails; i++) {
        if (i != sel) draw_thumbnail(cr, i, p);
    }
    if (sel >= 0 && sel < overview_state.num_thumbnails) draw_thumbnail(cr, sel, p);

    for (int i = 0;i < overview_state.num_thumbnails; i++) {
        draw_drop_slot(cr, i, p);
    }

    draw_label(overview_state.dragging ? "Solte sobre uma workspace para mover a janela"
                                       : "↑ ↓  navegar    Enter  abrir    arraste janelas para mover    Esc  sair",
               acx, area.y + area.height - config.font.height * 2.5 + (1.0 - p) * 12.0, 0, 0.4 * p);

    draw_drag_ghost(cr);
    draw_filter(cr, p);

    if (overview_state.front.id != XCB_NONE) {
        draw_util_copy_surface(&overview_state.back, &overview_state.front, 0, 0, 0, 0,
                               overview_state.screen_width, overview_state.screen_height);
        if (overview_state.overlay_visible) {
            uint32_t stack_value[] = {XCB_STACK_MODE_ABOVE};
            xcb_configure_window(conn, overview_state.overlay_window, XCB_CONFIG_WINDOW_STACK_MODE, stack_value);
        }
        xcb_flush(conn);
        xcb_get_input_focus_reply_t *fence = xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL);
        free(fence);
    }
}

static double overview_frame_period_ms(void) {
    int fps = overview_config.fps;
    if (fps < 15) fps = 15;
    if (fps > 240) fps = 240;
    return 1000.0 / fps;
}

static void overview_tick(EV_P_ ev_timer *w, int revents) {
    if (!overview_state.active) { ev_timer_stop(EV_A_ w); return; }
    double now = now_ms();
    double dt = (now - overview_state.last_tick_ms) / 1000.0;
    if (dt < 0 || dt > 0.25) dt = overview_frame_period_ms() / 1000.0;
    overview_state.last_tick_ms = now;

    if (overview_config.border_speed > 0) {
        overview_state.border_phase = fmod(overview_state.border_phase + overview_config.border_speed * dt, 360.0);
    }
    particles_step(dt);

    if (overview_state.animating) {
        double dur = overview_config.animation_duration_ms;
        double t = dur > 0 ? (now - overview_state.animation_start_ms) / dur : 1.0;
        if (t >= 1.0) { t = 1.0; overview_state.animating = false; }
        overview_state.animation_progress = lerp(overview_state.animation_from, overview_state.animation_to, ease_out_cubic(t));
    }
    if (overview_state.scroll_animating) {
        double t = (now - overview_state.scroll_start_ms) / SCROLL_ANIM_MS;
        if (t >= 1.0) { t = 1.0; overview_state.scroll_animating = false; }
        overview_state.scroll = lerp(overview_state.scroll_from, overview_state.scroll_to, ease_out_cubic(t));
    }
    if (!overview_state.animating && overview_state.exiting) {
        overview_render();
        overview_destroy();
        return;
    }
    overview_render();

    double period = overview_frame_period_ms();
    double spent = now_ms() - now;
    overview_state.frame_ema_ms = overview_state.frame_ema_ms <= 0 ? spent : overview_state.frame_ema_ms * 0.9 + spent * 0.1;
    if (!overview_state.particles_off && overview_state.num_particles > 0) {
        if (spent > FRAME_BUDGET_MS) {
            if (++overview_state.slow_frames >= PARTICLE_SLOW_FRAMES) {
                overview_state.particles_off = true;
                OVLOG("particles disabled for this session (frame avg %.1f ms)\n", overview_state.frame_ema_ms);
            }
        } else if (overview_state.slow_frames > 0) {
            overview_state.slow_frames--;
        }
    }
    double wait = period - spent;
    if (wait < 0.5) wait = 0.5;
    w->repeat = wait / 1000.0;
    ev_timer_again(EV_A_ w);
}

static void overview_start_timer(void) {
    if (ev_is_active(&overview_state.timer)) return;
    overview_state.last_tick_ms = now_ms();
    double per = overview_frame_period_ms() / 1000.0;
    ev_timer_set(&overview_state.timer, per, per);
    ev_timer_start(main_loop, &overview_state.timer);
}

static void overview_start_animation(double target) {
    overview_state.animation_from = overview_state.animation_progress;
    overview_state.animation_to = target;
    overview_state.animation_start_ms = now_ms();
    overview_state.animating = (overview_config.animation_duration_ms > 0);
    if (!overview_state.animating) overview_state.animation_progress = target;
    overview_start_timer();
}

static void overview_scroll_to(int idx, bool animate) {
    double target = scroll_for(idx);
    if (!animate || overview_config.animation_duration_ms <= 0) {
        overview_state.scroll = target;
        overview_state.scroll_animating = false;
        return;
    }
    overview_state.scroll_from = overview_state.scroll;
    overview_state.scroll_to = target;
    overview_state.scroll_start_ms = now_ms();
    overview_state.scroll_animating = true;
}

static void overview_set_selected(int idx) {
    if (idx < 0 || idx >= overview_state.num_thumbnails || idx == overview_state.selected_index) return;
    overview_state.selected_index = idx;
    overview_scroll_to(idx, true);
}

static int overview_collect_workspaces(bool store) {
    int count = 0;
    Output *output;
    TAILQ_FOREACH (output, &outputs, outputs) {
        if (!output->active || output->con == NULL) continue;
        Con *content = output_get_content(output->con);
        if (content == NULL) continue;
        Con *ws;
        TAILQ_FOREACH (ws, &(content->nodes_head), nodes) {
            if (ws->type != CT_WORKSPACE || con_is_internal(ws)) continue;
            if (store) {
                workspace_thumbnail_t *t = &overview_state.thumbnails[count];
                memset(t, 0, sizeof(*t));
                t->workspace = ws;
                t->output = output->con;
                t->out = output->con->rect;
            }
            count++;
        }
    }
    if (overview_config.dynamic_workspaces) {
        Con *focus_out = focused ? con_get_output(focused) : NULL;
        if (focus_out == NULL && !TAILQ_EMPTY(&outputs)) focus_out = TAILQ_FIRST(&outputs)->con;
        if (focus_out != NULL) {
            if (store) {
                workspace_thumbnail_t *t = &overview_state.thumbnails[count];
                memset(t, 0, sizeof(*t));
                t->new_slot = true;
                t->output = focus_out;
                t->out = focus_out->rect;
            }
            count++;
        }
    }
    return count;
}

static void overview_rebuild(Con *keep_selected) {
    int count = overview_collect_workspaces(false);
    free(overview_state.thumbnails);
    overview_state.thumbnails = scalloc(count > 0 ? count : 1, sizeof(workspace_thumbnail_t));
    overview_state.num_thumbnails = count;
    overview_collect_workspaces(true);
    overview_calculate_thumbnail_positions();

    int sel = -1, org = -1;
    Con *origin_ws = NULL;
    Output *output;
    TAILQ_FOREACH (output, &outputs, outputs) {
        if (!output->active || output->con == NULL) continue;
        Con *content = output_get_content(output->con);
        if (content && (focused == NULL || con_get_output(focused) == output->con)) {
            origin_ws = con_get_fullscreen_con(content, CF_OUTPUT);
            break;
        }
    }
    for (int i = 0; i < count; i++) {
        if (overview_state.thumbnails[i].workspace == keep_selected) sel = i;
        if (overview_state.thumbnails[i].workspace == origin_ws) org = i;
    }
    if (sel < 0) sel = (org >= 0) ? org : 0;
    if (org < 0) org = sel;
    overview_state.selected_index = sel;
    overview_state.origin_index = org;
    overview_state.hover_index = -1;
    overview_state.drop_index = -1;
    overview_scroll_to(sel, overview_state.active && overview_state.overlay_visible);
}

static void box_blur_pass(uint32_t *src, uint32_t *dst, int W, int H, int radius, bool vertical) {
    int len = vertical ? H : W;
    int lines = vertical ? W : H;
    int stride = vertical ? W : 1;
    int step = vertical ? 1 : W;
    double inv = 1.0 / (radius * 2 + 1);

    for (int line = 0; line < lines; line++) {
        uint32_t *s = src + line * step;
        uint32_t *d = dst + line * step;
        int64_t r = 0, g = 0, b = 0;

        /* janela inicial, com as bordas repetidas */
        for (int i = -radius; i <= radius; i++) {
            int k = i < 0 ? 0 : (i >= len ? len - 1 : i);
            uint32_t px = s[k * stride];
            r += (px >> 16) & 0xff;
            g += (px >> 8) & 0xff;
            b += px & 0xff;
        }
        for (int i = 0; i < len; i++) {
            d[i * stride] = 0xff000000u |
                            ((uint32_t)(r * inv) << 16) |
                            ((uint32_t)(g * inv) << 8) |
                            (uint32_t)(b * inv);
            int add = i + radius + 1, sub = i - radius;
            add = add >= len ? len - 1 : add;
            sub = sub < 0 ? 0 : sub;
            uint32_t pa = s[add * stride], ps = s[sub * stride];
            r += (int)((pa >> 16) & 0xff) - (int)((ps >> 16) & 0xff);
            g += (int)((pa >> 8) & 0xff) - (int)((ps >> 8) & 0xff);
            b += (int)(pa & 0xff) - (int)(ps & 0xff);
        }
    }
}

static void blur_surface(cairo_surface_t *dst, int W, int H, int amount) {
    if (amount <= 0 || W <= 0 || H <= 0) return;
    int radius = (int)(amount * 0.6);
    if (radius < 1) return;
    if (radius > 128) radius = 128;

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t *c = cairo_create(img);
    cairo_set_source_surface(c, dst, 0, 0);
    cairo_paint(c);
    cairo_destroy(c);
    cairo_surface_flush(img);

    int stride = cairo_image_surface_get_stride(img);
    if (stride != W * 4) {
        cairo_surface_destroy(img);
        ELOG("Overview: blur skipped (stride %d != %d)\n", stride, W * 4);
        return;
    }
    uint32_t *buf = (uint32_t *)cairo_image_surface_get_data(img);
    uint32_t *tmp = smalloc(sizeof(uint32_t) * W * H);

    for (int pass = 0; pass < 3;pass++) {
        box_blur_pass(buf, tmp, W, H, radius, false);
        box_blur_pass(tmp, buf, W, H, radius, true);
    }
    cairo_surface_mark_dirty(img);
    free(tmp);

    c = cairo_create(dst);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(c, img, 0, 0);
    cairo_paint(c);
    cairo_destroy(c);
    cairo_surface_flush(dst);
    cairo_surface_destroy(img);
}

static Con *overview_target_workspace(int idx) {
    if (idx < 0 || idx >= overview_state.num_thumbnails) return NULL;
    workspace_thumbnail_t *t = &overview_state.thumbnails[idx];
    if (!t->new_slot) return overview_ws_alive(t->workspace) ? t->workspace : NULL;
    Output *out = get_output_for_con(t->output);
    Con *content = output_get_content(t->output);
    if (out == NULL || content == NULL) return NULL;
    Con *ws = create_workspace_on_output(out, content);
    OVLOG("created workspace %s\n", ws ? ws->name : "(null)");
    return ws;
}

// Drag and Drop
//
static void overview_drag_reset(void) {
    overview_state.drag_pending = false;
    overview_state.dragging = false;
    overview_state.drag_con = NULL;
    overview_state.drag_src_index = -1;
    overview_state.drop_index = -1;
}

static void overview_drop(int target_idx) {
    Con *con = overview_state.drag_con;
    if (!con_alive(con)) { overview_drag_reset(); return; }
    Con *src_ws = con_get_workspace(con);
    Con *target_ws = overview_target_workspace(target_idx);
    if (target_ws != NULL) {
        if (target_ws != src_ws) {
            OVLOG("moving '%s' to workspace %s\n", con->window && con->window->name ? i3string_as_utf8(con->window->name) : "?", target_ws->name);
            con_move_to_workspace(con, target_ws, true, true, true);
        }

                workspace_thumbnail_t *t = &overview_state.thumbnails[target_idx];
        double tx, ty, tw, th;
        thumb_rect(target_idx, clamp01(overview_state.animation_progress), &tx, &ty, &tw, &th);

        drop_slot_t slot;
        bool have_slot = false;
        if (tw > 1 && th > 1) {
            const double ox = t->out.x + (overview_state.drag_x - tx) * (t->out.width / tw);
            const double oy = t->out.y + (overview_state.drag_y - ty) * (t->out.height / th);
            have_slot = drop_slot_at(t, ox, oy, &slot);
        }

        Con *sc = ws_scrolling_con(target_ws);
        if (sc != NULL) {
            /* con_attach() embrulha a janela num container novo por causa do
             * workspace_layout, então ela não entra no container de rolagem. */
            if (con->parent != sc) {
                Con *old_parent = con->parent;
                con_detach(con);
                con_attach(con, sc, true);
                if (old_parent != NULL && old_parent != sc) {
                    CALL(old_parent, on_remove_child);
                }
            }
            if (have_slot && slot.scroll_index >= 0) {
                TAILQ_REMOVE(&(sc->nodes_head), con, nodes);
                Con *before = NULL;
                int k = 0;
                Con *it;
                TAILQ_FOREACH (it, &(sc->nodes_head), nodes) {
                    if (k++ == slot.scroll_index) { before = it; break; }
                }
                if (before != NULL) {
                    TAILQ_INSERT_BEFORE(before, con, nodes);
                } else {
                    TAILQ_INSERT_TAIL(&(sc->nodes_head), con, nodes);
                }
            }
            if (con->percent <= 0.0) {
                con->percent = scrolling_config.default_width / 100.0;
            }
            if (have_slot) {
                con->rect = slot.rect;
            }
        } else {
            /* splith/splitv: insere antes/depois da folha sob o ponteiro. O
             * eixo veio da orientação do pai, então não há tree_split a fazer. */
            if (have_slot && !slot.whole && slot.target != NULL &&
                slot.target != con && con_exists(slot.target)) {
                insert_con_into(con, slot.target, slot.position);
            }
            /* i3 só renderiza a workspace visível: sem rect novo a janela
             * pode cair fora da thumbnail */
            if (!workspace_is_visible(target_ws)) {
                con->rect = have_slot ? slot.rect : target_ws->rect;
            }
        }

        if (con->percent <= 0.0) {
            con->percent = scrolling_config.default_width / 100.0;
        } else if (!workspace_is_visible(target_ws)) {
            con->rect = target_ws->rect;
        }

        tree_render();
        xcb_flush(conn);
        if (live_enabled()) {
            if (overview_ws_alive(target_ws) && workspace_is_visible(target_ws)) live_name_tree(target_ws, true);
            if (overview_ws_alive(src_ws) && workspace_is_visible(src_ws)) live_name_tree(src_ws, true);
        }
        snapshot_invalidate(target_ws);
        snapshot_invalidate(src_ws);
        int sel = overview_state.selected_index;
        Con *keep = (sel >= 0 && sel < overview_state.num_thumbnails && !overview_state.thumbnails[sel].new_slot)
                        ? overview_state.thumbnails[sel].workspace : NULL;
        if (!overview_ws_alive(keep)) keep = overview_ws_alive(target_ws) ? target_ws : NULL;
        overview_drag_reset();
        overview_rebuild(keep);
        overview_render();
        return;
    }
    overview_drag_reset();
}

void overview_enter(void) {
    if (overview_state.active) {
        if (overview_state.exiting) {
            overview_state.exiting = false;
            overview_start_animation(1.0);
        }
        return;
    }
    if (!overview_state.initialized) overview_init();
    if (!overview_state.initialized) return;
    if (!overview_config.enabled) { OVLOG("disabled\n"); return; }
    if (main_loop == NULL || root_screen == NULL || keysyms == NULL) {
        ELOG("Overview: called before i3 finished initializing\n");
        return;
    }
    OVLOG("Entering overview mode\n");

    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL);
    if (!geom) { ELOG("Overview: Could not get root geometry\n"); return; }
    overview_state.screen_width = geom->width;
    overview_state.screen_height = geom->height;
    free(geom);

    aiwr_capture_ensure();
    overview_prune_snapshots();
    live_prune();
    overview_filter_clear();

    Output *output;
    TAILQ_FOREACH (output, &outputs, outputs) {
        if (!output->active || output->con == NULL) continue;
        Con *content = output_get_content(output->con);
        Con *vis = content ? con_get_fullscreen_con(content, CF_OUTPUT) : NULL;
        if (vis == NULL) continue;
        if (live_enabled()) live_name_tree(vis, true);
        else overview_snapshot_workspace(vis);
    }
    overview_state.wallpaper = overview_config_wallpaper();

    if (overview_state.wallpaper == NULL) {
        overview_state.wallpaper = aiwr_wallpaper_surface(&overview_state.wp_w, &overview_state.wp_h);
    }

    OVLOG("wallpaper=%p\n", (void *)overview_state.wallpaper);

    overview_drag_reset();
    overview_rebuild(focused ? con_get_workspace(focused) : NULL);
    if (overview_state.num_thumbnails == 0) {
        ELOG("Overview: No workspaces found\n");
        overview_destroy();
        return;
    }

    overview_state.active = true;
    overview_state.exiting = false;
    overview_state.animation_progress = 0.0;
    overview_state.animating = false;
    overview_state.frame_ema_ms = 0;
    if (!overview_ensure_overlay()) {
        ELOG("Overview: Failed to create overlay window\n");
        overview_release_overlay();
        overview_destroy();
        return;
    }
    ELOG("[i3-aiwr] Overview: overlay window 0x%08x\n", overview_state.overlay_window);
    overview_bake_background();
    particles_init();
    if (!overview_show_overlay()) {
        overview_destroy();
        return;
    }
    overview_start_animation(1.0);
    OVLOG("open: %d thumbnails, %d live layers, composite=%d, particles=%d, fps=%d\n",
          overview_state.num_thumbnails, overview_state.live.count, aiwr_capture_available(),
          overview_state.num_particles, overview_config.fps);
}

static void overview_destroy(void) {
    ev_timer_stop(main_loop, &overview_state.timer);
    if (overview_state.keyboard_grabbed) {
        xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
        overview_state.keyboard_grabbed = false;
    }
    overview_park_overlay();
    overview_free_background();
    free(overview_state.particles);
    overview_state.particles = NULL;
    overview_state.num_particles = 0;
    if (overview_state.wallpaper) { cairo_surface_destroy(overview_state.wallpaper); overview_state.wallpaper = NULL; }
    for (int i = 0; i < overview_state.live.count; i++) {
        aiwr_layer_t *l = &overview_state.live.items[i];
        if (l->surface) { cairo_surface_destroy(l->surface); l->surface = NULL; }
    }
    for (int i = 0; i < overview_state.num_snapshots; i++) {
        overview_snapshot_t *s = &overview_state.snapshots[i];
        if (s->surface) { cairo_surface_destroy(s->surface); s->surface = NULL; }
    }
    free(overview_state.thumbnails);
    overview_state.thumbnails = NULL;
    overview_state.num_thumbnails = 0;
    overview_drag_reset();
    overview_state.active = false;
    overview_state.exiting = false;
    overview_state.animating = false;
    overview_state.scroll_animating = false;

    if (overview_state.focus_stolen) {
        xcb_window_t to = (focused && focused->window) ? focused->window->id : root;
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, to, XCB_CURRENT_TIME);
        overview_state.focus_stolen = false;
    }
    xcb_flush(conn);
    OVLOG("closed\n");
}

void overview_exit(void) {
    if (!overview_state.active || overview_state.exiting) return;
    overview_state.exiting = true;
    overview_drag_reset();
    overview_start_animation(0.0);
    overview_filter_clear();
}

void overview_next(void) {
    if (!overview_state.active || overview_state.exiting || overview_state.num_thumbnails == 0) return;
    overview_set_selected((overview_state.selected_index + 1) % overview_state.num_thumbnails);
}

void overview_prev(void) {
    if (!overview_state.active || overview_state.exiting || overview_state.num_thumbnails == 0) return;
    overview_set_selected((overview_state.selected_index - 1 + overview_state.num_thumbnails) % overview_state.num_thumbnails);
}

static void overview_go_to(Con *ws, Con *focus_con) {
    if (ws == NULL || !overview_ws_alive(ws)) { overview_exit(); return; }
    OVLOG("Selecting workspace %s\n", ws->name);
    workspace_show(ws);
    if (focus_con != NULL && con_alive(focus_con)) con_focus(focus_con);
    tree_render();
    if (live_enabled()) live_name_tree(ws, true);
    for (int i = 0; i < overview_state.num_thumbnails; i++) {
        if (overview_state.thumbnails[i].workspace == ws) {
            overview_state.selected_index = i;
            overview_state.origin_index = i;
        }
    }
    overview_scroll_to(overview_state.selected_index, false);
    overview_exit();
}

void overview_select(void) {
    if (!overview_state.active || overview_state.exiting) return;
    Con *ws = overview_target_workspace(overview_state.selected_index);
    if (ws != NULL && overview_state.thumbnails[overview_state.selected_index].new_slot) {
        overview_rebuild(ws);
    }
    overview_go_to(ws, NULL);
}

void overview_cancel(void) {
    if (!overview_state.active) return;
    overview_exit();
}

void overview_toggle(void) {
    if (overview_state.active && !overview_state.exiting) overview_exit();
    else overview_enter();
}

bool overview_is_active(void) {
    return overview_state.active;
}

bool overview_handle_event(xcb_generic_event_t *event) {
    if (!overview_state.active || overview_state.overlay_window == XCB_NONE) return false;

    switch (event->response_type & 0x7F) {
        case XCB_EXPOSE: {
            xcb_expose_event_t *e = (xcb_expose_event_t *)event;
            if (e->window != overview_state.overlay_window) return false;
            if (e->count == 0) overview_render();
            return true;
        }
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)event;
            if (e->event != overview_state.overlay_window) return false;
            if (overview_state.exiting) return true;
            double px = e->event_x, py = e->event_y;
            if (overview_state.drag_pending && !overview_state.dragging) {
                if (hypot(px - overview_state.press_x, py - overview_state.press_y) > DRAG_THRESHOLD) {
                    if (con_alive(overview_state.drag_con)) {
                        overview_state.dragging = true;
                        OVLOG("drag start\n");
                    } else {
                        overview_drag_reset();
                    }
                }
            }
            if (overview_state.dragging) {
                overview_state.drag_x = px;
                overview_state.drag_y = py;
                overview_state.drop_index = overview_thumbnail_at(px, py);
                overview_state.hover_index = -1;
            } else {
                overview_state.hover_index = overview_thumbnail_at(px, py);
            }
            return true;
        }
        case XCB_BUTTON_PRESS: {
            xcb_button_press_event_t *e = (xcb_button_press_event_t *)event;
            if (e->event != overview_state.overlay_window) return false;
            if (overview_state.exiting) return true;
            if (e->detail == XCB_BUTTON_INDEX_4) { overview_prev(); return true; }
            if (e->detail == XCB_BUTTON_INDEX_5) { overview_next(); return true; }
            if (e->detail == XCB_BUTTON_INDEX_3) { overview_cancel(); return true; }
            if (e->detail != XCB_BUTTON_INDEX_1) return true;
            double px = e->event_x, py = e->event_y;
            int idx = overview_thumbnail_at(px, py);
            overview_drag_reset();
            overview_state.press_x = px;
            overview_state.press_y = py;
            Con *win = overview_window_at(idx, px, py);
            if (win != NULL) {
                overview_state.drag_pending = true;
                overview_state.drag_con = win;
                overview_state.drag_src_index = idx;
                double x, y, w, h;
                thumb_rect(idx, clamp01(overview_state.animation_progress), &x, &y, &w, &h);
                workspace_thumbnail_t *t = &overview_state.thumbnails[idx];
                double sx = w / t->out.width, sy = h / t->out.height;
                double wx = x + ((double)win->rect.x - (double)t->out.x) * sx;
                double wy = y + ((double)win->rect.y - (double)t->out.y) * sy;
                overview_state.grab_fx = clamp01((px - wx) / (win->rect.width * sx));
                overview_state.grab_fy = clamp01((py - wy) / (win->rect.height * sy));
                overview_state.drag_x = px;
                overview_state.drag_y = py;
            }
            return true;
        }
        case XCB_BUTTON_RELEASE: {
            xcb_button_release_event_t *e = (xcb_button_release_event_t *)event;
            if (e->event != overview_state.overlay_window) return false;
            if (overview_state.exiting || e->detail != XCB_BUTTON_INDEX_1) return true;
            double px = e->event_x, py = e->event_y;
            int idx = overview_thumbnail_at(px, py);
            if (overview_state.dragging) {
                overview_drop(idx);
                return true;
            }
            if (overview_state.drag_pending) {
                Con *win = overview_state.drag_con;
                overview_drag_reset();
                if (con_alive(win)) overview_go_to(con_get_workspace(win), win);
                return true;
            }
            if (idx >= 0) {
                overview_state.selected_index = idx;
                overview_select();
            } else {
                overview_cancel();
            }
            return true;
        }
        case XCB_KEY_PRESS: {
            xcb_key_press_event_t *e = (xcb_key_press_event_t *)event;
            if (e->event != overview_state.overlay_window) return false;
            if (overview_state.exiting) return true;
            xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, e->detail, 0);
            const bool filtering = (overview_state.filter_len > 0);

            switch (sym) {
                case XK_Escape:
                    /* primeiro Esc limpa o filtro, o segundo fecha */
                    if (filtering) {
                        overview_filter_clear();
                        overview_render();
                    } else {
                        overview_cancel();
                    }
                    return true;
                case XK_BackSpace:
                    if (filtering) {
                        overview_state.filter[--overview_state.filter_len] = '\0';
                        overview_filter_changed();
                    }
                    return true;
                case XK_Up: case XK_Left: case XK_ISO_Left_Tab:
                    if (filtering) overview_step_match(-1); else overview_prev();
                    return true;
                case XK_Down: case XK_Right: case XK_Tab:
                    if (filtering) overview_step_match(1); else overview_next();
                    return true;
                case XK_Home: overview_set_selected(0); return true;
                case XK_End: overview_set_selected(overview_state.num_thumbnails - 1); return true;
                case XK_Return: case XK_KP_Enter:
                    overview_select();
                    return true;
                default:
                    break;
            }

            /* hjkl navegam enquanto não há filtro; depois viram texto */
            if (!filtering) {
                switch (sym) {
                    case XK_k: case XK_h: overview_prev(); return true;
                    case XK_j: case XK_l: overview_next(); return true;
                    case XK_space: overview_select(); return true;
                    default: break;
                }
                if (sym >= XK_1 && sym <= XK_9) {
                    int idx = sym - XK_1;
                    if (idx < overview_state.num_thumbnails) {
                        overview_state.selected_index = idx;
                        overview_select();
                    }
                    return true;
                }
            }

            if (sym >= 0x20 && sym <= 0x7e &&
                overview_state.filter_len < (int)sizeof(overview_state.filter) - 1) {
                overview_state.filter[overview_state.filter_len++] = (char)sym;
                overview_state.filter[overview_state.filter_len] = '\0';
                overview_filter_changed();
            }
            return true;
        }
        default:
            return false;
    }
}

void overview_init(void) {
    if (overview_state.initialized) return;
    if (conn == NULL || root_screen == NULL) {
        ELOG("Overview: init called before X connection\n");
        return;
    }
    memset(&overview_state, 0, sizeof(overview_state));
    overview_state.selected_index = -1;
    overview_state.origin_index = -1;
    overview_state.hover_index = -1;
    overview_state.drop_index = -1;
    overview_state.drag_src_index = -1;
    overview_state.copy_gc = xcb_generate_id(conn);
    uint32_t gcv[] = {XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS, 0};
    xcb_create_gc(conn, overview_state.copy_gc, root, XCB_GC_SUBWINDOW_MODE | XCB_GC_GRAPHICS_EXPOSURES, gcv);
    ev_timer_init(&overview_state.timer, overview_tick, 1.0 / 60.0, 1.0 / 60.0);
    srand((unsigned)time(NULL));
    overview_state.initialized = true;
    OVLOG("initialized (composite=%d, live=%d)\n", aiwr_capture_available(), overview_config.live_previews);
}
