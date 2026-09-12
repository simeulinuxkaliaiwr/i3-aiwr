#pragma once
/*
 * i3-aiwr — transição de workspace: as duas workspaces deslizam sobre um
 * overlay enquanto a troca real acontece embaixo.
 *
 * Nunca copia a root: a workspace que sai é capturada frame a frame via
 * COMPOSITE (ainda mapeada), e a que entra vem do cache "stale" — os clientes
 * recém-mapeados levam alguns frames para repintar.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "libi3.h"
#include "i3/aiwr_capture.h"

struct Con;

typedef enum {
    WT_SLIDE = 0,
    WT_FADE,
    WT_ZOOM,
} wt_type_t;

typedef enum {
    WT_HORIZONTAL = 0,
    WT_VERTICAL,
} wt_direction_t;

typedef struct workspace_transition_config {
    bool enabled;
    int duration_ms; /* 0 desliga */
    wt_direction_t direction;
    int fps;         /* compartilhado com as demais animações */
    wt_type_t type;
    char *curve;     /* NULL = ease-out-cubic */
} workspace_transition_config_t;

typedef struct workspace_transition_state {
    bool initialized;
    bool active;

    xcb_window_t overlay_window;
    xcb_colormap_t colormap;
    xcb_pixmap_t back_pixmap;
    xcb_visualid_t visual;
    uint8_t depth;
    surface_t front;
    surface_t back;
    bool overlay_ready;
    bool overlay_visible;
    int ov_w, ov_h;

    Rect out;     /* rect do output em que a troca acontece */
    double sign;  /* +1: a nova entra pela direita/baixo; -1: pelo lado oposto */

    aiwr_layers_t old_layers;
    aiwr_layers_t new_layers;

    double progress;
    int anim_id; /* no aiwr_anim */
} workspace_transition_state_t;

void workspace_transition_init(void);
/* workspace.c, dentro de workspace_show(): 'from' ainda está mapeada. */
void workspace_transition_begin(struct Con *from, struct Con *to);
/* Corta a animação em curso E solta o overlay (randr, shutdown). */
void workspace_transition_abort(void);
bool workspace_transition_active(void);
/* config reload: o wallpaper é decodificado uma vez e guardado. */
void workspace_transition_invalidate_wallpaper(void);

extern workspace_transition_config_t workspace_transition_config;
extern workspace_transition_state_t workspace_transition_state;
