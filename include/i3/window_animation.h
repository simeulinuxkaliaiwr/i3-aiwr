#pragma once
/*
 * i3-aiwr — animação de abertura de janela.
 *
 * O X não escala o conteúdo de uma janela. Em vez disso animamos só a
 * geometria do FRAME: o filho fica no tamanho final e o frame o revela.
 * Nenhum ConfigureNotify chega ao cliente, então nada de reflow no terminal.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

struct Con;

typedef struct window_animation_config {
    bool enabled;
    int duration_ms;  /* 0 desliga */
    int start_scale;  /* % do tamanho final no frame 0 */
    int fps;          /* compartilhado com as demais animações */
    char *curve;      /* NULL = ease-out-cubic */
    bool close_enabled;
    int close_duration_ms;
    int close_scale;
    char *close_curve;
    bool opacity;
    int start_opacity;
    int close_opacity;
} window_animation_config_t;

/* main.c, no arranque — ANTES da primeira janela. Se o módulo se inicializar
 * sozinho no primeiro map, o silêncio de arranque engole essa janela. */
void window_animation_init(void);
/* main.c, depois da árvore montada (restart in-place). */
void window_animation_seed_existing(void);
/* x.c, logo depois do xcb_map_window(con->frame.id). Só anima o primeiro
 * map de cada frame — trocar de workspace remapeia e não deve animar. */
void window_animation_on_map(struct Con *con);
/* x.c, junto do aiwr_stale_forget() em x_con_kill(). */
void window_animation_forget(xcb_window_t frame);

void window_animation_on_close(struct Con *con);
bool window_animation_running(void);
bool window_animation_current_rect(xcb_window_t frame, Rect *out);

extern window_animation_config_t window_animation_config;
