/*
 * i3-aiwr — transição de workspace. Ver workspace_transition.h.
 */
#include "all.h"
#include "i3/workspace_transition.h"
#include "i3/aiwr_capture.h"
#include "i3/aiwr_anim.h"
#include <xcb/shape.h>
#include <cairo/cairo-xcb.h>
#include <math.h>

#define WTLOG(fmt, ...) LOG("[i3-aiwr] Transition: " fmt, ##__VA_ARGS__)

workspace_transition_config_t workspace_transition_config = {
    .enabled = true,
    .duration_ms = 220,
    .direction = WT_HORIZONTAL,
    .fps = 60,
    .type = WT_SLIDE,
    .curve = NULL,
};

workspace_transition_state_t workspace_transition_state;

/* Decodificar o PNG a cada troca de workspace custava mais que a animação
 * inteira. Carrega uma vez; a config invalida. */
static cairo_surface_t *wallpaper_cache;
static bool wallpaper_cached;

/* ------------------------------------------------------------------ utils */

/* Posição de ws na lista do output (para saber de que lado a nova entra). */
static int wt_ws_index(Con *ws) {
    if (ws == NULL || ws->parent == NULL) return -1;
    int i = 0;
    Con *child;
    TAILQ_FOREACH (child, &(ws->parent->nodes_head), nodes) {
        if (child == ws) return i;
        i++;
    }
    return -1;
}

/* ---------------------------------------------------------------- overlay */

static void wt_park_overlay(void) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (s->overlay_window == XCB_NONE) return;
    uint32_t v[] = {(uint32_t)(int32_t)(-s->ov_w - 64), (uint32_t)(int32_t)(-s->ov_h - 64)};
    xcb_configure_window(conn, s->overlay_window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    s->overlay_visible = false;
}

static void wt_release_overlay(void) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (s->front.id != XCB_NONE) draw_util_surface_free(conn, &s->front);
    if (s->back.id != XCB_NONE) draw_util_surface_free(conn, &s->back);
    memset(&s->front, 0, sizeof(s->front));
    memset(&s->back, 0, sizeof(s->back));
    if (s->overlay_window != XCB_NONE) {
        xcb_destroy_window(conn, s->overlay_window);
        s->overlay_window = XCB_NONE;
    }
    if (s->back_pixmap != XCB_NONE) { xcb_free_pixmap(conn, s->back_pixmap); s->back_pixmap = XCB_NONE; }
    if (s->colormap != XCB_NONE) { xcb_free_colormap(conn, s->colormap); s->colormap = XCB_NONE; }
    s->overlay_ready = false;
    s->overlay_visible = false;
    s->ov_w = s->ov_h = 0;
}

/* Overlay do tamanho do output (não da tela: outro monitor não pode apagar).
 * Persistente e estacionado fora da tela — map/unmap faria o picom aplicar fade. */
static bool wt_ensure_overlay(int W, int H) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (s->overlay_ready && (s->ov_w != W || s->ov_h != H)) wt_release_overlay();
    if (s->overlay_ready) return true;
    if (W <= 0 || H <= 0) return false;

    uint8_t depth = root_screen->root_depth;
    xcb_visualid_t visual = root_screen->root_visual;
    xcb_visualtype_t *vt = aiwr_find_visualtype(visual);
    if (vt == NULL) {
        ELOG("Transition: visualtype 0x%x not found\n", visual);
        return false;
    }
    s->depth = depth;
    s->visual = visual;

    s->colormap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, s->colormap, root, visual);
    s->back_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, depth, s->back_pixmap, root, W, H);
    draw_util_surface_init(conn, &s->back, s->back_pixmap, vt, W, H);
    if (s->back.cr == NULL || cairo_status(s->back.cr) != CAIRO_STATUS_SUCCESS) {
        ELOG("Transition: cairo back surface failed\n");
        wt_release_overlay();
        return false;
    }

    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                    XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values[] = {s->back_pixmap, 0, 1, XCB_EVENT_MASK_NO_EVENT, s->colormap};
    s->overlay_window = xcb_generate_id(conn);
    xcb_create_window(conn, depth, s->overlay_window, root,
                      (int16_t)(-W - 64), (int16_t)(-H - 64), W, H, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual, mask, values);
    aiwr_set_overlay_hints(s->overlay_window, "i3-aiwr transition");

    /* região de input vazia: cliques atravessam para as janelas reais */
    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         s->overlay_window, 0, 0, 0, NULL);

    draw_util_surface_init(conn, &s->front, s->overlay_window, vt, W, H);
    xcb_map_window(conn, s->overlay_window);
    s->overlay_ready = true;
    s->overlay_visible = false;
    s->ov_w = W;
    s->ov_h = H;
    xcb_flush(conn);
    WTLOG("overlay created (%dx%d, depth %d)\n", W, H, depth);
    return true;
}

/* -------------------------------------------------------------- wallpaper */

static cairo_surface_t *wt_wallpaper(void) {
    if (wallpaper_cached) return wallpaper_cache;
    wallpaper_cached = true;
    if (overview_config.wallpaper_path != NULL) {
        cairo_surface_t *img = cairo_image_surface_create_from_png(overview_config.wallpaper_path);
        if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
            wallpaper_cache = img;
            return wallpaper_cache;
        }
        cairo_surface_destroy(img);
    }
    int w, h;
    wallpaper_cache = aiwr_wallpaper_surface(&w, &h);
    return wallpaper_cache;
}

void workspace_transition_invalidate_wallpaper(void) {
    if (wallpaper_cache != NULL) cairo_surface_destroy(wallpaper_cache);
    wallpaper_cache = NULL;
    wallpaper_cached = false;
}

/* ----------------------------------------------------------------- render */

static void wt_draw_background(cairo_t *cr, int W, int H) {
    cairo_surface_t *wp = wt_wallpaper();
    int iw = wp ? cairo_image_surface_get_width(wp) : 0;
    int ih = wp ? cairo_image_surface_get_height(wp) : 0;
    if (wp == NULL || iw <= 0 || ih <= 0) {
        cairo_set_source_rgb(cr, 0.04, 0.04, 0.06);
        cairo_paint(cr);
        return;
    }
    double k = fmax((double)W / iw, (double)H / ih);
    cairo_save(cr);
    cairo_translate(cr, (W - iw * k) / 2.0, (H - ih * k) / 2.0);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, wp, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

/* Um grupo por workspace: camadas sobrepostas (frame + pai de stacked/tabbed)
 * resolvem em opacidade cheia, e só o resultado recebe o alpha. Sem isso o
 * fade mostra cada camada transparente por cima da outra. O grupo custa uma
 * surface do tamanho do overlay, então só é usado quando há alpha. */
static void wt_draw_workspace(cairo_t *cr, aiwr_layers_t *L, Rect out,
                              double dx, double dy, double sc, double alpha) {
    if (L->count == 0 || alpha <= 0.001) return;
    if (alpha >= 0.999) {
        aiwr_layers_draw(cr, L, out, dx, dy, sc, sc, 1.0);
        return;
    }
    cairo_push_group(cr);
    aiwr_layers_draw(cr, L, out, dx, dy, sc, sc, 1.0);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void wt_render(double e) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (!s->active || s->back.cr == NULL) return;
    cairo_t *cr = s->back.cr;
    int W = s->ov_w, H = s->ov_h;

    double odx = 0, ody = 0, ndx = 0, ndy = 0;
    double osc = 1.0, nsc = 1.0, oal = 1.0, nal = 1.0;

    switch (workspace_transition_config.type) {
        case WT_FADE:
            oal = 1.0 - e;
            break;
        case WT_ZOOM:
            nsc = 0.92 + 0.08 * e;
            osc = 1.0 + 0.08 * e;
            nal = e;
            oal = 1.0 - e;
            ndx = W * (1.0 - nsc) / 2.0;
            ndy = H * (1.0 - nsc) / 2.0;
            odx = W * (1.0 - osc) / 2.0;
            ody = H * (1.0 - osc) / 2.0;
            break;
        case WT_SLIDE:
        default: {
            bool vert = (workspace_transition_config.direction == WT_VERTICAL);
            double span = vert ? H : W;
            double old_off = -e * span * s->sign;
            double new_off = (1.0 - e) * span * s->sign;
            if (vert) { ody = old_off; ndy = new_off; }
            else      { odx = old_off; ndx = new_off; }
            break;
        }
    }

    /* limpa o frame anterior: sem isso os frames se acumulam */
    cairo_save(cr);
    cairo_reset_clip(cr);
    cairo_identity_matrix(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    wt_draw_background(cr, W, H);
    cairo_restore(cr);

    wt_draw_workspace(cr, &s->new_layers, s->out, ndx, ndy, nsc, nal);
    wt_draw_workspace(cr, &s->old_layers, s->out, odx, ody, osc, oal);

    if (s->front.id == XCB_NONE) return;
    cairo_surface_flush(s->back.surface);
    draw_util_copy_surface(&s->back, &s->front, 0, 0, 0, 0, W, H);
    if (s->overlay_visible) {
        uint32_t v[] = {XCB_STACK_MODE_ABOVE};
        xcb_configure_window(conn, s->overlay_window, XCB_CONFIG_WINDOW_STACK_MODE, v);
    }
    aiwr_anim_request_fence();
}

/* -------------------------------------------------------------- lifecycle */

static void wt_drop_layers(void) {
    workspace_transition_state_t *s = &workspace_transition_state;
    aiwr_layers_free(&s->old_layers);
    aiwr_layers_free(&s->new_layers);
}

static void wt_step(double p, double e, void *data) {
    workspace_transition_state.progress = p;
    wt_render(e);
}

static void wt_done(void *data) {
    workspace_transition_state_t *s = &workspace_transition_state;
    wt_park_overlay();
    wt_drop_layers();
    s->active = false;
    s->anim_id = 0;
    s->progress = 0;
}

/* Corta no estado final. release: também destrói o overlay (randr, shutdown). */
static void wt_stop(bool release) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (s->active) {
        int id = s->anim_id;
        s->anim_id = 0;
        s->active = false;
        if (id > 0) aiwr_anim_cancel(id);
        wt_park_overlay();
        wt_drop_layers();
        s->progress = 0;
    }
    if (release) wt_release_overlay();
    xcb_flush(conn);
}

void workspace_transition_abort(void) {
    wt_stop(true);
}

bool workspace_transition_active(void) {
    return workspace_transition_state.active;
}

void workspace_transition_init(void) {
    workspace_transition_state_t *s = &workspace_transition_state;
    if (s->initialized) return;
    if (conn == NULL || root_screen == NULL || main_loop == NULL) return;
    memset(s, 0, sizeof(*s));
    aiwr_anim_init();
    s->initialized = true;
}

void workspace_transition_begin(Con *from, Con *to) {
    workspace_transition_state_t *s = &workspace_transition_state;

    if (!workspace_transition_config.enabled) return;
    if (workspace_transition_config.duration_ms <= 0) return;
    if (from == NULL || to == NULL || from == to) return;
    if (con_is_internal(from) || con_is_internal(to)) return;
    /* o overview já desenha a troca no próprio overlay */
    if (overview_is_active()) return;
    if (!aiwr_capture_available()) return;
    if (conn == NULL || root_screen == NULL || main_loop == NULL) return;

    if (!s->initialized) workspace_transition_init();
    if (!s->initialized) return;

    /* troca durante uma troca: corta a anterior, sem soltar o overlay */
    if (s->active) wt_stop(false);

    Con *output = con_get_output(to);
    if (output == NULL || output->rect.width == 0 || output->rect.height == 0) return;
    /* só animamos troca dentro do mesmo output */
    if (con_get_output(from) != output) return;

    s->out = output->rect;
    int W = (int)s->out.width, H = (int)s->out.height;
    if (!wt_ensure_overlay(W, H)) return;

    int i_from = wt_ws_index(from), i_to = wt_ws_index(to);
    s->sign = (i_to >= 0 && i_from >= 0 && i_to < i_from) ? -1.0 : 1.0;

    /* a que sai ainda está mapeada: captura real agora */
    aiwr_layers_collect(&s->old_layers, from);
    aiwr_stale_store(from);
    /* a que entra só será mapeada depois do tree_render(): usa a última imagem */
    aiwr_layers_collect_stale_owned(&s->new_layers, to);

    bool any_old = aiwr_layers_ensure_surfaces(&s->old_layers);
    bool any_new = aiwr_layers_ensure_surfaces(&s->new_layers);
    if (!any_old && !any_new) {
        wt_drop_layers();
        return;
    }

    s->progress = 0.0;
    s->active = true;

    /* frame 0 (idêntico à tela atual) no front ANTES de o overlay entrar */
    wt_render(0.0);
    uint32_t v[] = {(uint32_t)s->out.x, (uint32_t)s->out.y, XCB_STACK_MODE_ABOVE};
    xcb_configure_window(conn, s->overlay_window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_STACK_MODE, v);
    s->overlay_visible = true;
    xcb_flush(conn);

    aiwr_anim_set_fps(workspace_transition_config.fps);
    s->anim_id = aiwr_anim_start(workspace_transition_config.duration_ms,
                                 workspace_transition_config.curve, wt_step, wt_done, NULL);

    WTLOG("%s -> %s (%d old, %d new layers, sign %+.0f)\n", from->name, to->name,
          s->old_layers.count, s->new_layers.count, s->sign);
}
