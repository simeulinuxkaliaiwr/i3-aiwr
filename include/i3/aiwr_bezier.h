#pragma once
/*
 * i3-aiwr — curvas de animação.
 *
 * Dois tipos:
 *  - bezier: cubic-bezier, convenção do CSS e do Hyprland (P0 = (0,0),
 *    P3 = (1,1), só os dois pontos de controle importam). y pode passar de 1:
 *    é daí que vem o overshoot.
 *  - spring: oscilador harmônico amortecido, mesmos parâmetros do niri
 *    (damping-ratio, stiffness, epsilon, mass). Uma mola não tem duração:
 *    roda até assentar dentro de epsilon. O tempo de assentamento é calculado
 *    quando a curva é definida e vira a duração da animação — a duração da
 *    config é ignorada para molas.
 */
#include <stdbool.h>

typedef enum {
    AIWR_CURVE_BEZIER = 0,
    AIWR_CURVE_SPRING,
} aiwr_curve_type_t;

typedef struct aiwr_curve {
    char *name;
    aiwr_curve_type_t type;

    /* bezier */
    double x1, y1, x2, y2;

    /* spring */
    double damping_ratio;
    double stiffness;
    double mass;
    double epsilon;
    double settle_ms; /* calculado em aiwr_curve_define_spring */
} aiwr_curve_t;

void aiwr_curves_init(void);
bool aiwr_curve_define(const char *name, double x1, double y1, double x2, double y2);
bool aiwr_curve_define_spring(const char *name, double damping_ratio, double stiffness,
                              double mass, double epsilon, double speed);
/* "nome 0.05 0.7 0.1 1" — formato da diretiva bezier. */
bool aiwr_curve_define_spec(const char *spec);
/* "nome damping-ratio=1.0 stiffness=1000 epsilon=0.0001 [mass=1.0]" —
 * formato da diretiva spring. Chaves em qualquer ordem. */
bool aiwr_curve_define_spring_spec(const char *spec);

const aiwr_curve_t *aiwr_curve_get(const char *name);
/* Avalia em p (0..1). Sem curva, cai em ease-out-cubic. */
double aiwr_curve_eval(const aiwr_curve_t *c, double p);
double aiwr_curve_eval_named(const char *name, double p);
/* Duração natural em ms, ou 0 se a curva não tem uma (bezier). */
double aiwr_curve_natural_duration_ms(const aiwr_curve_t *c);
