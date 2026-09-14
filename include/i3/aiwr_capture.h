#pragma once
/*
 * i3-aiwr — window captures by COMPOSITE (NameWindowPixmap).
 */

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "data.h"

typedef struct aiwr_layer {
    xcb_window_t window;       /* i3wm's frame */
    xcb_pixmap_t pixmap;
    xcb_void_cookie_t cookie;
    bool checked;
    cairo_surface_t *surface;
    Rect rect;
    uint16_t depth;
    bool failed;
    int radius;
    bool borrowed;
} aiwr_layer_t;

typedef struct aiwr_layers {
    aiwr_layer_t *items;
    int count;
} aiwr_layers_t;

void aiwr_capture_init(void);
void aiwr_capture_ensure(void);
bool aiwr_capture_available(void);
bool aiwr_capture_external(void);

void aiwr_stale_store(struct Con *ws);
void aiwr_stale_forget(xcb_window_t frame);

xcb_visualtype_t *aiwr_find_visualtype(xcb_visualid_t id);
xcb_visualtype_t *aiwr_visualtype_for_depth(uint16_t depth);

void aiwr_layers_collect(aiwr_layers_t *L, Con *ws);
void aiwr_layers_refresh(aiwr_layers_t *L, Con *ws);
bool aiwr_layers_ensure_surfaces(aiwr_layers_t *L);
int aiwr_layers_draw(cairo_t *cr, aiwr_layers_t *L, Rect out, double x, double y,
                     double sx, double sy, double alpha);
void aiwr_layers_free(aiwr_layers_t *L);

bool aiwr_layer_draw_fitted(cairo_t *cr, aiwr_layers_t *L, xcb_window_t frame,
                            double x, double y, double w, double h);

int aiwr_layers_collect_stale_owned(aiwr_layers_t *L, struct Con *ws);

cairo_surface_t *aiwr_wallpaper_surface(int *w, int *h);

void aiwr_set_overlay_hints(xcb_window_t win, const char *name);
