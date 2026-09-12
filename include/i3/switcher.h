#pragma once
/*
 * i3-aiwr — alternador de janelas (Alt+Tab).
 *
 * MRU, não espacial: o overview responde "onde está tudo", este responde
 * "me leva de volta ao que eu estava fazendo". Por isso a ordem é de foco
 * recente e atravessa workspaces — justamente o caso em que o overview é
 * pior, porque você teria que procurar com os olhos.
 *
 * A ordem sai de graça do i3: focus_head já está em ordem de foco recente.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include "libi3.h"
#include "i3/aiwr_capture.h"

struct Con;

typedef struct switcher_config {
    bool enabled;
    int max_items;   /* além disso, use o overview */
    int cell_width;  /* px, antes de logical_px */
    int cell_height;
    bool show_preview;
} switcher_config_t;

void switcher_init(void);
/* commands.c: 'switcher next' / 'switcher prev' */
void switcher_open(bool backwards);
bool switcher_is_active(void);
/* handlers.c, antes do overview_handle_event() */
bool switcher_handle_event(xcb_generic_event_t *event);
/* randr / shutdown */
void switcher_abort(void);

extern switcher_config_t switcher_config;
