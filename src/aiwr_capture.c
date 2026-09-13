/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: window capture via COMPOSITE. See aiwr_capture.h.
 *
 */
#include "all.h"
#include "i3/aiwr_capture.h"
#include <xcb/composite.h>
#include <cairo/cairo-xcb.h>
#include <math.h>

#define CAPLOG(fmt, ...) LOG("[i3-aiwr] Capture: " fmt, ##__VA_ARGS__)

static struct {
    bool initialized;
    bool available;  /* COMPOSITE >= 0.2 is present */
    bool external;   /* an external compositor was detected */
    bool redirected; /* we redirected the root in Automatic mode */
    xcb_atom_t bypass_atom, rootpmap_atom, esetroot_atom, cm_atom;
} cap;

static aiwr_layers_t stale; /* last image of each frame; see the header */
static aiwr_layer_t *layer_find(aiwr_layers_t *L, xcb_window_t win);

static xcb_atom_t intern_atom(const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static bool external_compositor_running(void) {
    if (cap.cm_atom == XCB_NONE) return false;
    xcb_get_selection_owner_reply_t *o =
        xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, cap.cm_atom), NULL);
    bool ext = (o != NULL && o->owner != XCB_NONE);
    free(o);
    return ext;
}

static void try_redirect(void) {
    if (cap.redirected || !cap.available) return;
    xcb_generic_error_t *err = xcb_request_check(
        conn, xcb_composite_redirect_subwindows_checked(conn, root, XCB_COMPOSITE_REDIRECT_AUTOMATIC));
    if (err != NULL) {
        CAPLOG("RedirectSubwindows(Automatic) failed (error %d) - assuming external compositor\n", err->error_code);
        free(err);
        cap.external = true;
    } else {
        cap.redirected = true;
        CAPLOG("root subwindows redirected (Automatic) - no external compositor\n");
    }
}

bool aiwr_layer_draw_fitted(cairo_t *cr, aiwr_layers_t *L, xcb_window_t frame,
                            double x, double y, double w, double h) {
    aiwr_layer_t *l = layer_find(L, frame);
    if (l == NULL || l->surface == NULL || l->failed) return false;
    if (l->rect.width == 0 || l->rect.height == 0) return false;

    cairo_surface_flush(l->surface);
    const double k = fmin(w / (double)l->rect.width, h / (double)l->rect.height);
    const double dw = l->rect.width * k, dh = l->rect.height * k;

    cairo_save(cr);
    cairo_translate(cr, x + (w - dw) / 2.0, y + (h - dh) / 2.0);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, l->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, l->rect.width, l->rect.height);
    cairo_clip(cr);
    cairo_set_operator(cr, cap.external ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
}

void aiwr_capture_init(void) {
    if (cap.initialized) return;
    cap.initialized = true;
    cap.bypass_atom = intern_atom("_NET_WM_BYPASS_COMPOSITOR");
    cap.rootpmap_atom = intern_atom("_XROOTPMAP_ID");
    cap.esetroot_atom = intern_atom("ESETROOT_PMAP_ID");

    int screen_no = 0;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; it.rem; xcb_screen_next(&it), i++) {
        if (it.data->root == root) { screen_no = i; break; }
    }
    char sel[32];
    snprintf(sel, sizeof(sel), "_NET_WM_CM_S%d", screen_no);
    cap.cm_atom = intern_atom(sel);

    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_composite_id);
    if (ext == NULL || !ext->present) {
        CAPLOG("COMPOSITE extension not present - live capture disabled\n");
        return;
    }
    xcb_composite_query_version_reply_t *v =
        xcb_composite_query_version_reply(conn, xcb_composite_query_version(conn, 0, 4), NULL);
    if (v == NULL) return;
    bool ok = (v->major_version > 0 || v->minor_version >= 2);
    CAPLOG("COMPOSITE %d.%d\n", v->major_version, v->minor_version);
    free(v);
    if (!ok) return;
    cap.available = true;

    if (external_compositor_running()) {
        cap.external = true;
        CAPLOG("external compositor owns %s - using its redirection\n", sel);
        return;
    }
    try_redirect();
}

void aiwr_capture_ensure(void) {
    if (!cap.available || cap.redirected) return;
    if (external_compositor_running()) { cap.external = true; return; }
    cap.external = false;
    try_redirect();
}

bool aiwr_capture_available(void) {
    return cap.available;
}

bool aiwr_capture_external(void) {
    return cap.external;
}

xcb_visualtype_t *aiwr_find_visualtype(xcb_visualid_t id) {
    xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(root_screen);
    for (; d.rem; xcb_depth_next(&d)) {
        xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data);
        for (; v.rem; xcb_visualtype_next(&v)) {
            if (v.data->visual_id == id) return v.data;
        }
    }
    return NULL;
}

xcb_visualtype_t *aiwr_visualtype_for_depth(uint16_t depth) {
    if (depth == root_screen->root_depth) return aiwr_find_visualtype(root_screen->root_visual);
    xcb_depth_iterator_t d = xcb_screen_allowed_depths_iterator(root_screen);
    for (; d.rem; xcb_depth_next(&d)) {
        if (d.data->depth != depth) continue;
        xcb_visualtype_iterator_t v = xcb_depth_visuals_iterator(d.data);
        for (; v.rem; xcb_visualtype_next(&v)) {
            if (v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) return v.data;
        }
    }
    return NULL;
}

typedef void (*walk_cb)(Con *con, void *arg);

static void walk_rec(Con *con, Con *skip, walk_cb cb, void *arg) {
    if (con == NULL || con == skip) return;
    if (con->type != CT_WORKSPACE) cb(con, arg);
    Con *ch;
    TAILQ_FOREACH (ch, &(con->nodes_head), nodes) {
        walk_rec(ch, skip, cb, arg);
    }
    TAILQ_FOREACH (ch, &(con->floating_head), floating_windows) {
        walk_rec(ch, skip, cb, arg);
    }
}

/* Stacking order: tiling, then floating, then any fullscreen window on top. */
static void walk_ws(Con *ws, walk_cb cb, void *arg) {
    Con *fs = con_get_fullscreen_con(ws, CF_OUTPUT);
    if (fs == NULL) fs = con_get_fullscreen_con(ws, CF_GLOBAL);
    walk_rec(ws, fs, cb, arg);
    if (fs != NULL) walk_rec(fs, NULL, cb, arg);
}

static aiwr_layer_t *layer_find(aiwr_layers_t *L, xcb_window_t win) {
    for (int i = 0; i < L->count; i++) {
        if (L->items[i].window == win) return &L->items[i];
    }
    return NULL;
}

static aiwr_layer_t *layer_append(aiwr_layers_t *L) {
    L->items = srealloc(L->items, sizeof(aiwr_layer_t) * (L->count + 1));
    aiwr_layer_t *l = &L->items[L->count++];
    memset(l, 0, sizeof(*l));
    return l;
}

static void layer_release(aiwr_layer_t *l) {
    if (l->surface != NULL) {
        cairo_surface_destroy(l->surface);
        l->surface = NULL;
    }
    if (l->checked) {
        xcb_discard_reply(conn, l->cookie.sequence);
        l->checked = false;
    }
    if (l->pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, l->pixmap);
        l->pixmap = XCB_NONE;
    }
}

/* Asynchronous requests only, so this is safe inside workspace_show(). */
static void layer_name(aiwr_layer_t *l, Con *con) {
    l->window = con->frame.id;
    l->rect = con->rect;
    l->depth = con->depth;
    l->radius = rounded_corners_radius_for(con);
    l->failed = false;
    l->pixmap = xcb_generate_id(conn);
    l->cookie = xcb_composite_name_window_pixmap_checked(conn, con->frame.id, l->pixmap);
    l->checked = true;
}

static bool con_capturable(Con *con) {
    return con->mapped && con->frame.id != XCB_NONE && con->rect.width > 0 && con->rect.height > 0;
}

/* Verifies the NameWindowPixmap request and creates the surface. Round-trips. */
static bool layer_ready(aiwr_layer_t *l) {
    if (l->failed || l->pixmap == XCB_NONE) return false;
    if (l->checked) {
        l->checked = false;
        xcb_generic_error_t *e = xcb_request_check(conn, l->cookie);
        if (e != NULL) {
            CAPLOG("NameWindowPixmap(0x%08x) failed: error %d\n", l->window, e->error_code);
            free(e);
            l->failed = true;
            l->pixmap = XCB_NONE;
            return false;
        }
    }
    if (l->surface == NULL) {
        xcb_visualtype_t *vt = aiwr_visualtype_for_depth(l->depth);
        if (vt == NULL) {
            l->failed = true;
            return false;
        }
        l->surface = cairo_xcb_surface_create(conn, l->pixmap, vt, l->rect.width, l->rect.height);
        if (cairo_surface_status(l->surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(l->surface);
            l->surface = NULL;
            l->failed = true;
            return false;
        }
    }
    return true;
}

static void collect_cb(Con *con, void *arg) {
    aiwr_layers_t *L = arg;
    if (!con_capturable(con) || layer_find(L, con->frame.id) != NULL) return;
    layer_name(layer_append(L), con);
}

void aiwr_layers_collect(aiwr_layers_t *L, Con *ws) {
    if (!cap.available || ws == NULL) return;
    walk_ws(ws, collect_cb, L);
}

void aiwr_layers_refresh(aiwr_layers_t *L, Con *ws) {
    aiwr_layers_collect(L, ws);
}

bool aiwr_layers_ensure_surfaces(aiwr_layers_t *L) {
    bool any = false;
    for (int i = 0; i < L->count; i++) {
        if (layer_ready(&L->items[i])) any = true;
    }
    return any;
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    if (r <= 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

int aiwr_layers_draw(cairo_t *cr, aiwr_layers_t *L, Rect out, double x, double y,
                     double sx, double sy, double alpha) {
    int n = 0;
    for (int i = 0; i < L->count; i++) {
        aiwr_layer_t *l = &L->items[i];
        if (l->surface == NULL || l->failed) continue;
        cairo_surface_mark_dirty(l->surface);
        cairo_save(cr);
        cairo_translate(cr, x + ((double)l->rect.x - (double)out.x) * sx,
                        y + ((double)l->rect.y - (double)out.y) * sy);
        cairo_scale(cr, sx, sy);
        /* Same silhouette as the frame's shape. */
        rounded_path(cr, 0, 0, l->rect.width, l->rect.height, l->radius);
        cairo_clip(cr);
        cairo_set_source_surface(cr, l->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), (sx < 0.999 || sy < 0.999) ? CAIRO_FILTER_GOOD : CAIRO_FILTER_FAST);
        if (alpha >= 0.999) {
            /* Without an external compositor the server ignores the alpha of
             * ARGB windows and shows the premultiplied RGB as opaque. SOURCE
             * reproduces that in our buffer; OVER reproduces what a
             * compositor does. Mixing the two produced translucent black
             * frames that did not match the real screen. */
            cairo_set_operator(cr, cap.external ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_SOURCE);
            cairo_paint(cr);
        } else {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_paint_with_alpha(cr, alpha);
        }
        cairo_restore(cr);
        n++;
    }
    return n;
}

void aiwr_layers_free(aiwr_layers_t *L) {
    for (int i = 0; i < L->count; i++) {
        layer_release(&L->items[i]);
    }
    free(L->items);
    L->items = NULL;
    L->count = 0;
}

static void stale_cb(Con *con, void *arg) {
    if (!con_capturable(con)) return;
    aiwr_layer_t *l = layer_find(&stale, con->frame.id);
    if (l == NULL) {
        l = layer_append(&stale);
    } else {
        layer_release(l);
    }
    layer_name(l, con);
}

void aiwr_stale_store(Con *ws) {
    if (!cap.available || ws == NULL) return;
    walk_ws(ws, stale_cb, NULL);
}

void aiwr_stale_forget(xcb_window_t frame) {
    for (int i = 0; i < stale.count; i++) {
        if (stale.items[i].window != frame) continue;
        layer_release(&stale.items[i]);
        memmove(&stale.items[i], &stale.items[i + 1], sizeof(aiwr_layer_t) * (stale.count - i - 1));
        stale.count--;
        return;
    }
}

typedef struct {
    aiwr_layers_t *L;
    int n;
} stale_collect_t;

/* Copies a cached layer's pixmap into one we own. XCB_NONE on failure. */

static xcb_pixmap_t stale_pixmap_copy(aiwr_layer_t *s) {
    if (s->pixmap == XCB_NONE || s->rect.width == 0 || s->rect.height == 0) {
        return XCB_NONE;
    }
    xcb_pixmap_t copy = xcb_generate_id(conn);
    xcb_create_pixmap(conn, s->depth, copy, root, s->rect.width, s->rect.height);

    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t gcv[] = {0};
    xcb_create_gc(conn, gc, copy, XCB_GC_GRAPHICS_EXPOSURES, gcv);
    xcb_void_cookie_t ck = xcb_copy_area_checked(conn, s->pixmap, copy, gc, 0, 0, 0, 0,
                                                 s->rect.width, s->rect.height);
    xcb_generic_error_t *e = xcb_request_check(conn, ck);
    xcb_free_gc(conn, gc);
    if (e != NULL) {
        CAPLOG("stale copy of 0x%08x failed: error %d\n", s->window, e->error_code);
        free(e);
        xcb_free_pixmap(conn, copy);
        return XCB_NONE;
    }
    return copy;
}

static void stale_owned_cb(Con *con, void *arg) {
    stale_collect_t *sc = arg;
    if (con->frame.id == XCB_NONE || con->rect.width == 0 || con->rect.height == 0) return;
    if (layer_find(sc->L, con->frame.id) != NULL) return;

    aiwr_layer_t *s = layer_find(&stale, con->frame.id);
    if (s == NULL) return;
    if (s->rect.width != con->rect.width || s->rect.height != con->rect.height) return;
    /* The layer has to be ready: the NameWindowPixmap cookie must have been
     * verified before we can copy from it. */
    if (!layer_ready(s)) return;

    xcb_pixmap_t copy = stale_pixmap_copy(s);
    if (copy == XCB_NONE) return;

    aiwr_layer_t *l = layer_append(sc->L);
    l->window = con->frame.id;
    l->pixmap = copy;
    l->checked = false;
    l->failed = false;
    l->depth = s->depth;
    l->rect = con->rect;
    l->radius = rounded_corners_radius_for(con);
    l->surface = NULL; /* aiwr_layers_ensure_surfaces builds it from our copy */
    sc->n++;
}

int aiwr_layers_collect_stale_owned(aiwr_layers_t *L, Con *ws) {
    if (!cap.available || ws == NULL) return 0;
    stale_collect_t sc = {L, 0};
    walk_ws(ws, stale_owned_cb, &sc);
    return sc.n;
}

cairo_surface_t *aiwr_wallpaper_surface(int *w, int *h) {
    xcb_atom_t atoms[2] = {cap.rootpmap_atom, cap.esetroot_atom};
    for (int i = 0; i < 2; i++) {
        if (atoms[i] == XCB_NONE) continue;
        xcb_get_property_reply_t *r = xcb_get_property_reply(
            conn, xcb_get_property(conn, 0, root, atoms[i], XCB_ATOM_PIXMAP, 0, 1), NULL);
        if (r == NULL) continue;
        xcb_pixmap_t pix = XCB_NONE;
        if (r->type == XCB_ATOM_PIXMAP && xcb_get_property_value_length(r) >= 4) {
            pix = *(xcb_pixmap_t *)xcb_get_property_value(r);
        }
        free(r);
        if (pix == XCB_NONE) continue;
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, pix), NULL);
        if (g == NULL) continue;
        if (g->depth != root_screen->root_depth || g->width == 0 || g->height == 0) { free(g); continue; }
        cairo_surface_t *s = cairo_xcb_surface_create(conn, pix, aiwr_find_visualtype(root_screen->root_visual),
                                                      g->width, g->height);
        if (w) *w = g->width;
        if (h) *h = g->height;
        free(g);
        if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
        cairo_surface_destroy(s);
    }
    return NULL;
}

void aiwr_set_overlay_hints(xcb_window_t win, const char *name) {
    static const char cls[] = "i3-aiwr\0i3-aiwr";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(cls), cls);
    if (name != NULL) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(name), name);
    }
    if (cap.bypass_atom != XCB_NONE) {
        uint32_t v = 2; /* 2 = never unredirect because of this window */
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, cap.bypass_atom, XCB_ATOM_CARDINAL, 32, 1, &v);
    }
}
