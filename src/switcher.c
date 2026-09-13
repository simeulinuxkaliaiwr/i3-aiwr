/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the window switcher. See switcher.h.
 *
 */
#include "all.h"
#include "i3/switcher.h"
#include "i3/aiwr_capture.h"
#include <xcb/shape.h>
#include <cairo/cairo-xcb.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#define XK_MISCELLANY
#define XK_LATIN1
#define XK_XKB_KEYS
#include <X11/keysymdef.h>

#define SWLOG(fmt, ...) DLOG("[i3-aiwr] Switcher: " fmt, ##__VA_ARGS__)

#define SW_PAD 18
#define SW_GAP 12
#define SW_RADIUS 14

static void sw_render(void);

switcher_config_t switcher_config = {
    .enabled = true,
    .max_items = 8,
    .cell_width = 220,
    .cell_height = 140,
    .show_preview = true,
};

typedef struct sw_item {
    Con *con;
    xcb_window_t frame;
} sw_item_t;

static struct {
    bool initialized;
    bool active;

    xcb_window_t win;
    xcb_pixmap_t back_pixmap;
    surface_t front;
    surface_t back;
    int w, h;
    bool grabbed;

    sw_item_t items[32];
    int count;
    int selected;

    /* Modifiers held when the switcher opened; releasing them all commits. */
    uint16_t hold_mods;

    aiwr_layers_t layers;
} sw;


static void sw_step(bool backwards) {
    if (!sw.active || sw.count < 1) return;
    sw.selected += backwards ? -1 : 1;
    if (sw.selected < 0) sw.selected = sw.count - 1;
    if (sw.selected >= sw.count) sw.selected = 0;
    sw_render();
}

static double sw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void sw_rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    if (r <= 0.5) {
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

static uint16_t sw_current_mods(void) {
    xcb_query_pointer_reply_t *r =
        xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    if (r == NULL) return 0;
    const uint16_t mask = r->mask & (XCB_MOD_MASK_1 | XCB_MOD_MASK_2 | XCB_MOD_MASK_3 |
                                     XCB_MOD_MASK_4 | XCB_MOD_MASK_5 | XCB_MOD_MASK_CONTROL);
    free(r);
    return mask;
}

static void sw_collect(Con *con, Con *skip_internal) {
    if (con == NULL || sw.count >= switcher_config.max_items) return;
    if (con->type == CT_WORKSPACE && con_is_internal(con)) return;

    if (con->window != NULL && con->window->id != XCB_NONE) {
        sw.items[sw.count].con = con;
        sw.items[sw.count].frame = con->frame.id;
        sw.count++;
        return;
    }
    Con *c;
    TAILQ_FOREACH (c, &(con->focus_head), focused) {
        sw_collect(c, skip_internal);
    }
    TAILQ_FOREACH (c, &(con->floating_head), floating_windows) {
        sw_collect(c, skip_internal);
    }
}

static void sw_release_overlay(void) {
    if (sw.front.id != XCB_NONE) draw_util_surface_free(conn, &sw.front);
    if (sw.back.id != XCB_NONE) draw_util_surface_free(conn, &sw.back);
    memset(&sw.front, 0, sizeof(sw.front));
    memset(&sw.back, 0, sizeof(sw.back));
    if (sw.win != XCB_NONE) {
        xcb_destroy_window(conn, sw.win);
        sw.win = XCB_NONE;
    }
    if (sw.back_pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, sw.back_pixmap);
        sw.back_pixmap = XCB_NONE;
    }
    sw.w = sw.h = 0;
}

static bool sw_ensure_overlay(int W, int H, int x, int y) {
    if (sw.win != XCB_NONE && (sw.w != W || sw.h != H)) sw_release_overlay();

    if (sw.win == XCB_NONE) {
        xcb_visualtype_t *vt = aiwr_find_visualtype(root_screen->root_visual);
        if (vt == NULL) return false;

        sw.back_pixmap = xcb_generate_id(conn);
        xcb_create_pixmap(conn, root_screen->root_depth, sw.back_pixmap, root, W, H);
        draw_util_surface_init(conn, &sw.back, sw.back_pixmap, vt, W, H);
        if (sw.back.cr == NULL) {
            sw_release_overlay();
            return false;
        }

        uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL |
                        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        uint32_t values[] = {sw.back_pixmap, 0, 1, XCB_EVENT_MASK_EXPOSURE};
        sw.win = xcb_generate_id(conn);
        xcb_create_window(conn, root_screen->root_depth, sw.win, root,
                          (int16_t)x, (int16_t)y, W, H, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, vt->visual_id, mask, values);
        aiwr_set_overlay_hints(sw.win, "i3-aiwr switcher");
        xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                             XCB_CLIP_ORDERING_UNSORTED, sw.win, 0, 0, 0, NULL);
        draw_util_surface_init(conn, &sw.front, sw.win, vt, W, H);
        sw.w = W;
        sw.h = H;
    }

    uint32_t v[] = {(uint32_t)(int32_t)x, (uint32_t)(int32_t)y, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(conn, sw.win,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, v);
    return true;
}

static void sw_class_color(const char *s, double *r, double *g, double *b) {
    uint32_t hash = 2166136261u;
    for (const char *p = s ? s : "?"; *p; p++) {
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    }
    const double hue = (hash % 360) / 360.0;
    const double h6 = hue * 6.0, f = h6 - floor(h6);
    const double v = 0.85, p2 = v * (1 - 0.55), q = v * (1 - 0.55 * f), t = v * (1 - 0.55 * (1 - f));
    switch ((int)floor(h6) % 6) {
        case 0: *r = v; *g = t; *b = p2; break;
        case 1: *r = q; *g = v; *b = p2; break;
        case 2: *r = p2; *g = v; *b = t; break;
        case 3: *r = p2; *g = q; *b = v; break;
        case 4: *r = t; *g = p2; *b = v; break;
        default: *r = v; *g = p2; *b = q; break;
    }
}

static void sw_draw_fallback(cairo_t *cr, Con *con, double x, double y, double w, double h) {
    const char *cls = (con->window && con->window->class_class) ? con->window->class_class : "?";
    double r, g, b;
    sw_class_color(cls, &r, &g, &b);

    const double side = fmin(w, h) * 0.42;
    const double bx = x + (w - side) / 2.0, by = y + (h - side) / 2.0 - h * 0.06;
    sw_rounded(cr, bx, by, side, side, side * 0.22);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill(cr);

    char initial[8];
    snprintf(initial, sizeof(initial), "%c", (char)toupper((unsigned char)cls[0]));
    i3String *s = i3string_from_utf8(initial);
    const int tw = predict_text_width(s);
    draw_util_text(s, &sw.back, draw_util_hex_to_color("#FFFFFFEE"),
                   draw_util_hex_to_color("#00000000"),
                   (int)(bx + (side - tw) / 2.0), (int)(by + (side - config.font.height) / 2.0),
                   tw + 4);
    i3string_free(s);
}

static void sw_render(void) {
    if (!sw.active || sw.back.cr == NULL) return;
    cairo_t *cr = sw.back.cr;
    const double cw = logical_px(switcher_config.cell_width);
    const double ch = logical_px(switcher_config.cell_height);
    const double pad = logical_px(SW_PAD);
    const double gap = logical_px(SW_GAP);
    const double labelh = config.font.height + logical_px(10);

    /* fundo */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    sw_rounded(cr, 0, 0, sw.w, sw.h, logical_px(SW_RADIUS) + 6);
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.09);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < sw.count; i++) {
        const double x = pad + i * (cw + gap);
        const double y = pad;
        const bool selected = (i == sw.selected);
        Con *con = sw.items[i].con;

        sw_rounded(cr, x, y, cw, ch, logical_px(SW_RADIUS));
        cairo_set_source_rgb(cr, 0.13, 0.13, 0.17);
        cairo_fill(cr);

        cairo_save(cr);
        sw_rounded(cr, x, y, cw, ch, logical_px(SW_RADIUS));
        cairo_clip(cr);
        bool drew = false;
        if (switcher_config.show_preview) {
            drew = aiwr_layer_draw_fitted(cr, &sw.layers, sw.items[i].frame, x, y, cw, ch);
        }
        if (!drew) {
            sw_draw_fallback(cr, con, x, y, cw, ch);
        }
        cairo_restore(cr);

        /* borda */
        if (selected) {
            const color_t a = overview_config.border_start;
            const color_t b = overview_config.border_end;
            const int speed = overview_config.border_speed > 0 ? overview_config.border_speed : 45;
            const double ph = fmod(sw_now_ms() * speed / 1000.0, 360.0) * M_PI / 180.0;
            const double mx = x + cw / 2, my = y + ch / 2, len = fmax(cw, ch);
            cairo_pattern_t *pat = cairo_pattern_create_linear(
                mx - cos(ph) * len, my - sin(ph) * len, mx + cos(ph) * len, my + sin(ph) * len);
            cairo_pattern_add_color_stop_rgb(pat, 0.0, a.red, a.green, a.blue);
            cairo_pattern_add_color_stop_rgb(pat, 0.5, b.red, b.green, b.blue);
            cairo_pattern_add_color_stop_rgb(pat, 1.0, a.red, a.green, a.blue);
            const double lw = overview_config.border_width > 0 ? overview_config.border_width : 3;
            sw_rounded(cr, x - lw / 2, y - lw / 2, cw + lw, ch + lw, logical_px(SW_RADIUS) + lw / 2);
            cairo_set_source(cr, pat);
            cairo_set_line_width(cr, lw);
            cairo_stroke(cr);
            cairo_pattern_destroy(pat);
        } else {
            sw_rounded(cr, x, y, cw, ch, logical_px(SW_RADIUS));
            cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }

        /* título */
        if (con->window != NULL && con->window->name != NULL) {
            i3String *nm = con->window->name;
            const int tw = predict_text_width(nm);
            const int maxw = (int)cw - 8;
            draw_util_text(nm, &sw.back,
                           draw_util_hex_to_color(selected ? "#FFFFFFF2" : "#FFFFFF99"),
                           draw_util_hex_to_color("#00000000"),
                           (int)(x + 4), (int)(y + ch + logical_px(6)),
                           tw < maxw ? tw + 4 : maxw);
        }
        (void)labelh;
    }

    cairo_surface_flush(sw.back.surface);
    draw_util_copy_surface(&sw.back, &sw.front, 0, 0, 0, 0, sw.w, sw.h);
    xcb_flush(conn);
}

static void sw_teardown(void) {
    if (sw.grabbed) {
        xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
        sw.grabbed = false;
    }
    aiwr_layers_free(&sw.layers);
    sw.active = false;
    sw.count = 0;
    sw.selected = 0;
    if (sw.win != XCB_NONE) {
        uint32_t v[] = {(uint32_t)(int32_t)(-sw.w - 64), (uint32_t)(int32_t)(-sw.h - 64)};
        xcb_configure_window(conn, sw.win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    }
    xcb_flush(conn);
}

static void sw_commit(void) {
    Con *target = (sw.selected >= 0 && sw.selected < sw.count) ? sw.items[sw.selected].con : NULL;
    sw_teardown();
    if (target != NULL && con_exists(target)) {
        workspace_show(con_get_workspace(target));
        con_activate(target);
        tree_render();
    }
}

void switcher_abort(void) {
    if (!sw.active) return;
    sw_teardown();
    sw_release_overlay();
}

bool switcher_is_active(void) {
    return sw.active;
}

void switcher_init(void) {
    if (sw.initialized) return;
    if (conn == NULL || root_screen == NULL) return;
    memset(&sw, 0, sizeof(sw));
    sw.initialized = true;
}

void switcher_open(bool backwards) {
    if (!switcher_config.enabled) return;
    if (!sw.initialized) switcher_init();
    if (!sw.initialized) return;
    if (overview_is_active()) return;

    if (sw.active) { /* already open */
        sw.selected += backwards ? -1 : 1;
        if (sw.selected < 0) sw.selected = sw.count - 1;
        if (sw.selected >= sw.count) sw.selected = 0;
        sw_render();
        return;
    }

    sw.count = 0;
    sw_collect(croot, NULL);
    if (sw.count < 2) return;

    memset(&sw.layers, 0, sizeof(sw.layers));
    if (switcher_config.show_preview && aiwr_capture_available()) {
        Con *output;
        TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
            if (con_is_internal(output)) continue;
            Con *content = output_get_content(output);
            if (content == NULL) continue;
            Con *ws;
            TAILQ_FOREACH (ws, &(content->nodes_head), nodes) {
                if (workspace_is_visible(ws)) {
                    aiwr_layers_collect(&sw.layers, ws);
                } else {
                    aiwr_layers_collect_stale_owned(&sw.layers, ws);
                }
            }
        }
        aiwr_layers_ensure_surfaces(&sw.layers);
    }

    const double cw = logical_px(switcher_config.cell_width);
    const double ch = logical_px(switcher_config.cell_height);
    const double pad = logical_px(SW_PAD);
    const double gap = logical_px(SW_GAP);
    const int W = (int)(pad * 2 + sw.count * cw + (sw.count - 1) * gap);
    const int H = (int)(pad * 2 + ch + config.font.height + logical_px(10));

    Con *out = con_get_output(focused ? focused : croot);
    Rect o = (out != NULL) ? out->rect : croot->rect;
    const int x = (int)(o.x + ((int)o.width - W) / 2);
    const int y = (int)(o.y + ((int)o.height - H) / 2);

    if (!sw_ensure_overlay(W, H, x, y)) {
        aiwr_layers_free(&sw.layers);
        return;
    }
    xcb_map_window(conn, sw.win);

    sw.hold_mods = sw_current_mods();
    xcb_grab_keyboard_reply_t *gk = xcb_grab_keyboard_reply(
        conn, xcb_grab_keyboard(conn, false, root, XCB_CURRENT_TIME,
                                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    sw.grabbed = (gk != NULL && gk->status == XCB_GRAB_STATUS_SUCCESS);
    free(gk);

    sw.active = true;
    sw.selected = backwards ? sw.count - 1 : 1;
    sw_render();
    SWLOG("open: %d items, hold_mods=0x%x, grabbed=%d\n", sw.count, sw.hold_mods, sw.grabbed);

    if (sw.hold_mods == 0) SWLOG("no modifier held; modal mode\n");
}

bool switcher_handle_event(xcb_generic_event_t *event) {
    if (!sw.active) return false;
    const uint8_t type = event->response_type & ~0x80;

    if (type == XCB_KEY_PRESS) {
        xcb_key_press_event_t *e = (xcb_key_press_event_t *)event;
        xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, e->detail, 0);
        switch (sym) {
            case XK_Escape:
                sw_teardown();
                return true;
            case XK_Return: case XK_KP_Enter:
                sw_commit();
                return true;
            case XK_Tab: case XK_Right: case XK_l:
                sw_step(false);
                return true;
            case XK_ISO_Left_Tab: case XK_Left: case XK_h:
                sw_step(true);
                return true;
            default:
                return true;
        }
    }

    if (type == XCB_KEY_RELEASE) {
        if (sw.hold_mods == 0) return true;
        if ((sw_current_mods() & sw.hold_mods) == 0) {
            sw_commit();
        }
        return true;
    }

    return false;
}
