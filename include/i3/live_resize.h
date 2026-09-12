#pragma once
/*
 * i3-aiwr — resize ao vivo.
 *
 * O i3 original arrasta só uma barra de prévia e aplica o resize no final.
 * Aqui o resize é aplicado durante o arrasto, limitado a live_resize_fps
 * quadros por segundo: cada aplicação manda ConfigureNotify de verdade para
 * os clientes, então sem o limite um terminal pesado trava o arrasto.
 */
#include <stdbool.h>

typedef struct live_resize_config {
    bool enabled;
    int fps; /* limite de aplicações por segundo durante o arrasto */
} live_resize_config_t;

extern live_resize_config_t live_resize_config;
