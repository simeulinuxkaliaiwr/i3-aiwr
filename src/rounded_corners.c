/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: rounded window corners, applied with the SHAPE extension.
 * See rounded_corners.h.
 *
 */
#include "all.h"
#include <math.h>

typedef struct {
    xcb_window_t client, frame;
    uint16_t fw, fh, ww, wh;
    int radius, inner;
    bool shaped_by_us;
    bool prop_checked, prop_owned;
} rc_entry_t;

/*
 * One entry per shaped client. The cached geometry lets rounded_corners_apply
 * skip the shape requests when nothing changed, which matters because it is
 * called on every render.
 *
 */
static rc_entry_t *entries = NULL;
static int n_entries = 0;
static xcb_atom_t owned_atom = XCB_NONE;

/*
 * _I3_AIWR_ROUNDED marks a client whose shape we set ourselves. Some clients
 * ship their own shape, and we must not clobber it — but after a restart we
 * have no memory of which shapes are ours, so the property is how we
 * recognise our own work.
 *
 */
static xcb_atom_t rc_atom(void) {
    if (owned_atom == XCB_NONE) {
        const char *n = "_I3_AIWR_ROUNDED";
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, strlen(n), n), NULL);
        if (r != NULL) {
            owned_atom = r->atom;
            free(r);
        }
    }
    return owned_atom;
}

static rc_entry_t *rc_find(xcb_window_t client) {
    for (int i = 0; i < n_entries; i++) {
        if (entries[i].client == client) return &entries[i];
    }
    return NULL;
}

static rc_entry_t *rc_get(xcb_window_t client) {
    rc_entry_t *e = rc_find(client);
    if (e != NULL) return e;
    entries = srealloc(entries, sizeof(rc_entry_t) * (n_entries + 1));
    e = &entries[n_entries++];
    memset(e, 0, sizeof(*e));
    e->client = client;
    return e;
}

static void rc_del(xcb_window_t client) {
    for (int i = 0; i < n_entries; i++) {
        if (entries[i].client != client) continue;
        memmove(&entries[i], &entries[i + 1], sizeof(rc_entry_t) * (n_entries - i - 1));
        n_entries--;
        return;
    }
}

static bool rc_prop_owned(rc_entry_t *e) {
    if (e->prop_checked) return e->prop_owned;
    e->prop_checked = true;
    xcb_atom_t a = rc_atom();
    if (a == XCB_NONE) return false;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        conn, xcb_get_property(conn, 0, e->client, a, XCB_ATOM_CARDINAL, 0, 1), NULL);
    e->prop_owned = (r != NULL && r->type == XCB_ATOM_CARDINAL && xcb_get_property_value_length(r) >= 4);
    free(r);
    return e->prop_owned;
}

/*
 * Approximates a rounded rectangle as a list of horizontal spans: one span
 * per scanline through each corner arc, plus one large rectangle for the
 * straight middle section. SHAPE takes rectangles, not paths, so the corners
 * have to be rasterised here.
 *
 * Returns the number of rectangles written to out.
 *
 */
int generate_rounded_rectangles(xcb_rectangle_t *out, int max_rects,
                                int width, int height, int radius) {
    int count = 0;
    if (width <= 0 || height <= 0 || max_rects <= 0) return 0;
    if (radius <= 0) {
        out[0] = (xcb_rectangle_t){0, 0, (uint16_t)width, (uint16_t)height};
        return 1;
    }
    if (radius > width / 2) radius = width / 2;
    if (radius > height / 2) radius = height / 2;

    for (int y = 0; y < radius && count < max_rects; y++) {
        int dy = radius - y;
        int dx = (int)(0.5 + sqrt((double)(radius * radius - dy * dy)));
        if (dx > radius) dx = radius;
        int w = width - 2 * (radius - dx);
        if (w <= 0) continue;
        out[count++] = (xcb_rectangle_t){(int16_t)(radius - dx), (int16_t)y, (uint16_t)w, 1};
    }
    if (height > 2 * radius && count < max_rects) {
        out[count++] = (xcb_rectangle_t){0, (int16_t)radius, (uint16_t)width, (uint16_t)(height - 2 * radius)};
    }
    for (int y = 0; y < radius && count < max_rects; y++) {
        int dy = y + 1;
        int dx = (int)(0.5 + sqrt((double)(radius * radius - dy * dy)));
        if (dx > radius) dx = radius;
        int w = width - 2 * (radius - dx);
        if (w <= 0) continue;
        out[count++] = (xcb_rectangle_t){(int16_t)(radius - dx), (int16_t)(height - radius + y), (uint16_t)w, 1};
    }
    return count;
}

bool rounded_corners_should_apply(Con *con) {
    if (con == NULL || con->window == NULL || con->frame.id == XCB_NONE) return false;
    if (!config.rounded_corners.enabled || !shape_supported) return false;
    if (config.rounded_corners.radius <= 0) return false;
    if (con->parent != NULL && con->parent->type == CT_DOCKAREA) return false;
    if (con->fullscreen_mode != CF_NONE) return false;
    if (con_is_floating(con)) {
        if (!config.rounded_corners.apply_to_floating) return false;
    } else if (!config.rounded_corners.apply_to_tiling) return false;

    if (con->window->shaped) {
        rc_entry_t *e = rc_find(con->window->id);
        if (e != NULL && e->shaped_by_us) return true;
        if (!rc_prop_owned(rc_get(con->window->id))) return false;
    }
    return true;
}

int rounded_corners_radius_for(Con *con) {
    if (!rounded_corners_should_apply(con)) return 0;
    int r = config.rounded_corners.radius;
    int m = (int)(con->rect.width < con->rect.height ? con->rect.width : con->rect.height) / 2;
    return r > m ? m : r;
}

int rounded_corners_inner_radius(Con *con, int radius) {
    Rect *w = &(con->window_rect);
    int sides[4] = {
        (int)w->x,
        (int)w->y,
        (int)con->rect.width - (int)(w->x + w->width),
        (int)con->rect.height - (int)(w->y + w->height),
    };
    int bw = -1;
    for (int i = 0; i < 4; i++) {
        if (sides[i] > 0 && (bw < 0 || sides[i] < bw)) bw = sides[i];
    }
    if (bw < 0) bw = 0;
    int inner = radius - bw;
    return inner < 0 ? 0 : inner;
}

bool rounded_corners_owns_window(xcb_window_t win) {
    rc_entry_t *e = rc_find(win);
    return e != NULL && e->shaped_by_us;
}

static void shape_rects(xcb_window_t win, int w, int h, int radius, bool input_too) {
    int max_rects = 2 * radius + 10;
    xcb_rectangle_t *rects = scalloc(max_rects, sizeof(xcb_rectangle_t));
    int n = generate_rounded_rectangles(rects, max_rects, w, h, radius);
    if (n > 0) {
        xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                             win, 0, 0, n, rects);
        if (input_too) {
            xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                                 win, 0, 0, n, rects);
        }
    }
    free(rects);
}

void rounded_corners_apply(Con *con) {
    if (con == NULL || con->window == NULL) return;
    if (!rounded_corners_should_apply(con)) {
        rounded_corners_remove(con);
        return;
    }
    int radius = rounded_corners_radius_for(con);
    int inner = rounded_corners_inner_radius(con, radius);
    uint16_t fw = con->rect.width, fh = con->rect.height;
    uint16_t ww = con->window_rect.width, wh = con->window_rect.height;
    if (fw == 0 || fh == 0 || radius <= 0) return;

    rc_entry_t *e = rc_get(con->window->id);
    if (e->shaped_by_us && e->frame == con->frame.id && e->fw == fw && e->fh == fh &&
        e->ww == ww && e->wh == wh && e->radius == radius && e->inner == inner) {
        return;
    }

    shape_rects(con->frame.id, fw, fh, radius, true);
    if (inner > 0 && ww > 0 && wh > 0) {
        shape_rects(con->window->id, ww, wh, inner, false);
    } else {
        xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, con->window->id, 0, 0, XCB_PIXMAP_NONE);
    }
    if (!e->prop_owned) {
        uint32_t one = 1;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, con->window->id, rc_atom(), XCB_ATOM_CARDINAL, 32, 1, &one);
        e->prop_owned = true;
        e->prop_checked = true;
    }
    e->frame = con->frame.id;
    e->fw = fw;
    e->fh = fh;
    e->ww = ww;
    e->wh = wh;
    e->radius = radius;
    e->inner = inner;
    e->shaped_by_us = true;
}

void rounded_corners_remove(Con *con) {
    if (con == NULL || con->window == NULL || !shape_supported) return;
    rc_entry_t *e = rc_find(con->window->id);
    if (e == NULL || !e->shaped_by_us) return;
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, con->frame.id, 0, 0, XCB_PIXMAP_NONE);
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, con->frame.id, 0, 0, XCB_PIXMAP_NONE);
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, con->window->id, 0, 0, XCB_PIXMAP_NONE);
    xcb_delete_property(conn, con->window->id, rc_atom());
    e->shaped_by_us = false;
    e->prop_owned = false;
}

void rounded_corners_forget(Con *con) {
    if (con == NULL || con->window == NULL) return;
    rc_entry_t *e = rc_find(con->window->id);
    if (e == NULL) return;
    if (e->shaped_by_us && shape_supported) {
        xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, con->window->id, 0, 0, XCB_PIXMAP_NONE);
        xcb_delete_property(conn, con->window->id, rc_atom());
    }
    rc_del(con->window->id);
}
