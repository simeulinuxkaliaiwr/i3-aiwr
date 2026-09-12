#pragma once
/*
 * i3-aiwr — captura de janelas via COMPOSITE (NameWindowPixmap).
 * Base compartilhada do overview e da transição de workspace.
 *
 * Por que não copiar a root window: com um compositor (picom) as janelas são
 * redirecionadas e a root só contém o wallpaper. Sem compositor, a cópia só
 * vale para o que está visível naquele instante. NameWindowPixmap dá o
 * conteúdo real de cada frame, e continua válido depois do unmap.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "data.h"

typedef struct aiwr_layer {
    xcb_window_t window;       /* frame do i3 */
    xcb_pixmap_t pixmap;       /* pixmap nomeado (nosso, liberar) */
    xcb_void_cookie_t cookie;  /* NameWindowPixmap checked */
    bool checked;              /* cookie ainda não verificado */
    cairo_surface_t *surface;
    Rect rect;                 /* geometria absoluta no momento da captura */
    uint16_t depth;
    bool failed;
    int radius;      /* cantos arredondados do con (0 = quadrado) */
    bool borrowed;   /* pixmap pertence ao cache "stale": não liberar */
} aiwr_layer_t;

typedef struct aiwr_layers {
    aiwr_layer_t *items;
    int count;
} aiwr_layers_t;

void aiwr_capture_init(void);
/* Re-checa compositor externo / redirecionamento (barato; 1 round-trip). */
void aiwr_capture_ensure(void);
bool aiwr_capture_available(void);
/* true se há compositor externo (picom): as janelas ARGB são compostas com
 * alpha. false: o servidor mostra o RGB pré-multiplicado, opaco. */
bool aiwr_capture_external(void);

/* Cache "stale": última imagem de cada frame, sobrevive ao unmap. Guardar a
 * workspace que está SAINDO (ainda mapeada) e usar para a que ENTRA — os
 * clientes recém-mapeados levam alguns frames para repintar (preto). */
void aiwr_stale_store(struct Con *ws);
void aiwr_stale_forget(xcb_window_t frame);
/* Acrescenta a L as camadas da workspace (mesmo desmapeada) que têm imagem
 * guardada com o mesmo tamanho. Retorna quantas. */

xcb_visualtype_t *aiwr_find_visualtype(xcb_visualid_t id);
xcb_visualtype_t *aiwr_visualtype_for_depth(uint16_t depth);

/* Nomeia pixmaps de todos os frames mapeados da workspace (tiling, floating,
 * fullscreen por cima). Só requests assíncronos: seguro dentro de workspace_show(). */
void aiwr_layers_collect(aiwr_layers_t *L, Con *ws);
/* Adiciona frames que ainda não estão na lista (janelas mapeadas depois). */
void aiwr_layers_refresh(aiwr_layers_t *L, Con *ws);
/* Cria as surfaces cairo (faz round-trips). true se há ao menos uma camada. */
bool aiwr_layers_ensure_surfaces(aiwr_layers_t *L);
/* Desenha as camadas: ponto (rect.x,rect.y) do output vira (x,y), escala sx/sy. */
int aiwr_layers_draw(cairo_t *cr, aiwr_layers_t *L, Rect out, double x, double y,
                     double sx, double sy, double alpha);
void aiwr_layers_free(aiwr_layers_t *L);

/* Desenha a camada deste frame preenchendo (x,y,w,h) com proporção mantida.
 * false se não há imagem para ele. */
bool aiwr_layer_draw_fitted(cairo_t *cr, aiwr_layers_t *L, xcb_window_t frame,
                            double x, double y, double w, double h);

/* Como aiwr_layers_collect_stale, mas copia cada pixmap para um nosso. Use
 * quando as camadas vão sobreviver a eventos (o switcher segura o teclado por
 * segundos): as camadas emprestadas apontam para pixmaps do cache, que podem
 * ser liberados no meio. Retorna quantas foram acrescentadas. */
int aiwr_layers_collect_stale_owned(aiwr_layers_t *L, struct Con *ws);

/* Surface do wallpaper (_XROOTPMAP_ID / ESETROOT_PMAP_ID) ou NULL. Caller destrói a surface. */
cairo_surface_t *aiwr_wallpaper_surface(int *w, int *h);

/* WM_CLASS "i3-aiwr", _NET_WM_BYPASS_COMPOSITOR=2 (nunca unredirect), nome. */
void aiwr_set_overlay_hints(xcb_window_t win, const char *name);
