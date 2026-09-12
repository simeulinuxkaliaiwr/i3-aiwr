#pragma once
/*
 * i3-aiwr — Overview (niri-like): workspaces empilhadas, zoom-out animado,
 * janelas ao vivo (COMPOSITE) e drag & drop de janelas entre workspaces.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <ev.h>
#include "libi3.h" /* surface_t */
#include "i3/aiwr_capture.h"

struct Con;

typedef struct overview_config {
    bool enabled;
    int thumbnail_scale;       /* % do tamanho do output */
    int spacing;               /* px entre thumbnails */
    int animation_duration_ms; /* 0 desliga a animação */
    bool show_workspace_names;
    int background_opacity;    /* 0-100: escurecimento do wallpaper */
    bool live_previews;        /* COMPOSITE: janelas ao vivo */
    int particles;             /* partículas no fundo (0 = desliga; auto-desliga se derrubar o FPS) */
    int fps;                   /* alvo de frames/s da UI (padrão 60) */
    /* borda das thumbnails — seção "overview border_*" da config */
    color_t border_start;      /* gradiente (selecionada / alvo de drop / ghost do drag) */
    color_t border_end;
    color_t border_inactive;   /* cor sólida das demais */
    int border_width;          /* px */
    int border_speed;          /* graus/s de rotação do gradiente (0 = estático) */

    char *wallpaper_path;
    int background_blur;
    bool dynamic_workspaces;
    int thumbnail_blur;
} overview_config_t;

typedef struct overview_particle {
    float x, y, vx, vy, r, a, ph;
} overview_particle_t;

/* Fallback sem COMPOSITE: cópia da root da workspace quando ela saiu de cena. */
typedef struct overview_snapshot {
    struct Con *workspace;
    xcb_pixmap_t pixmap;
    cairo_surface_t *surface;
    int width, height;
} overview_snapshot_t;

typedef struct workspace_thumbnail {
    struct Con *workspace; /* NULL = slot "nova workspace" ou workspace morta */
    struct Con *output;
    Rect out;              /* rect do output */
    bool new_slot;
    double x, y, width, height; /* layout final, sem scroll */
} workspace_thumbnail_t;

typedef struct overview_state {
    bool initialized;
    bool active;
    bool exiting;
    bool in_snapshot;

    xcb_window_t overlay_window;
    xcb_colormap_t colormap;
    xcb_pixmap_t back_pixmap;
    uint8_t depth;
    xcb_visualid_t visual;
    surface_t front;
    surface_t back;
    xcb_gcontext_t copy_gc;
    bool keyboard_grabbed;
    bool focus_stolen;

    cairo_surface_t *wallpaper;
    int wp_w, wp_h;

    workspace_thumbnail_t *thumbnails;
    int num_thumbnails;
    int selected_index;
    int origin_index; /* workspace que estava/vai estar na tela (origem do zoom) */
    int hover_index;

    int screen_width;
    int screen_height;

    bool animating;
    double animation_progress; /* 0 = tela normal (zoom in), 1 = overview aberto */
    double animation_from, animation_to;
    double animation_start_ms;

    bool scroll_animating;
    double scroll, scroll_from, scroll_to;
    double scroll_start_ms;

    ev_timer timer;
    double last_tick_ms;

    overview_snapshot_t *snapshots;
    int num_snapshots;

    aiwr_layers_t live; /* pixmaps nomeados por frame (válidos mesmo com a janela unmapped) */

    /* overlay persistente (criado 1x, estacionado fora da tela quando fechado) */
    bool overlay_ready;
    bool overlay_visible;
    int ov_w, ov_h;
    /* wallpaper pré-escalado (nunca reescala o root pixmap por frame) */
    xcb_pixmap_t bg_pixmap, wp_thumb_pixmap;
    cairo_surface_t *bg_full;  /* tamanho da tela */
    cairo_surface_t *wp_thumb; /* tamanho da thumbnail */
    int wp_thumb_w, wp_thumb_h;
    /* borda gradiente / desempenho / partículas */
    double border_phase;
    double frame_ema_ms;
    int slow_frames;
    bool particles_off;
    overview_particle_t *particles;
    int num_particles;

    /* drag & drop */
    bool drag_pending;
    bool dragging;
    struct Con *drag_con;
    int drag_src_index;
    int drop_index;
    double press_x, press_y;
    double drag_x, drag_y;
    double grab_fx, grab_fy; /* ponto pego, em fração da janela */
    char filter[64];
    int filter_len;
} overview_state_t;

typedef struct drop_slot {
    Con *target;
    position_t position;
    int scroll_index;
    Rect rect;
    bool whole;
} drop_slot_t;

void overview_init(void);
void overview_enter(void);
void overview_exit(void);
void overview_toggle(void);
void overview_next(void);
void overview_prev(void);
void overview_select(void);
void overview_cancel(void);
void overview_render(void);
bool overview_is_active(void);

/* handlers.c, início de handle_event(): if (overview_handle_event(event)) return; */
bool overview_handle_event(xcb_generic_event_t *event);

/* workspace.c, início de workspace_show(): guarda a imagem da workspace que sai.
 * Só requests assíncronos — nada de cairo, nada de round-trip. */
void overview_snapshot_workspace(struct Con *workspace);

/* con.c, con_free() */
void overview_forget_workspace(struct Con *workspace);

extern overview_config_t overview_config;


extern overview_state_t overview_state;
