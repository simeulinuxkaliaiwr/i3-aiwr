#pragma once
/*
 * i3-aiwr — bordas com gradiente animado (estilo Hyprland "borderangle loop").
 *
 * Config:
 *   gradient_border enabled|disabled
 *   gradient_border color_start #RRGGBB[AA]      (janela focada)
 *   gradient_border color_end   #RRGGBB[AA]
 *   gradient_border inactive_start #RRGGBB[AA]   (opcional; sem isso as inativas usam a cor sólida do i3)
 *   gradient_border inactive_end   #RRGGBB[AA]
 *   gradient_border angle 45                     (graus)
 *   gradient_border direction horizontal|vertical|diagonal   (atalho para angle 0|90|45)
 *   gradient_border speed 60                     (graus/segundo; 0 = estático)
 *   gradient_border fps 30
 */
#include <stdbool.h>
#include <cairo/cairo.h>
#include "libi3.h"

struct Con;

typedef struct aiwr_gradient {
    bool enabled;
    color_t active_start, active_end;
    color_t inactive_start, inactive_end;
    bool inactive_set;
    int angle;
    int speed;
    int fps;
    bool animate_inactive;
} aiwr_gradient_t;

extern aiwr_gradient_t aiwr_gradient;

void gradient_border_init(void);

/* Padrão cairo para a borda de um con de w x h. NULL = usar cor sólida do i3. */
cairo_pattern_t *gradient_border_pattern(double w, double h, bool active);

/* Definida em x.c: repinta só os retângulos de borda do con e copia para o frame. */
void x_gradient_border_repaint(struct Con *con);
