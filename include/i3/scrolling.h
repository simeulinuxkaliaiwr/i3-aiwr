#pragma once
/*
 * i3-aiwr — layout de rolagem (estilo niri).
 *
 * As colunas ficam lado a lado com largura própria e a fila continua além da
 * borda da tela. O container guarda um deslocamento de visão; o render
 * subtrai esse deslocamento do x de cada coluna, e o que sai da tela o
 * próprio X recorta.
 *
 * Diferença central para o L_SPLITH: em split os percent somam 1 e dividem o
 * espaço; aqui cada percent é a largura da coluna em fração da viewport,
 * independente das outras. Por isso con_fix_percent() não pode mexer nesses
 * containers.
 */
#include <stdbool.h>

struct Con;

typedef struct scrolling_config {
    int default_width;  /* % da viewport para uma coluna nova */
    int duration_ms;    /* animação da rolagem; 0 = instantâneo */
    char *curve;
    bool center_focus;  /* true: centraliza a coluna focada em vez de só trazê-la para dentro */
} scrolling_config_t;

/* Deslocamento atual (px) do container de rolagem. */
double scrolling_offset(struct Con *con);
/* Rola para deixar 'column' visível. Anima se duration_ms > 0. */
void scrolling_reveal(struct Con *column);
/* con.c, no fim de con_focus(). */
void scrolling_on_focus(struct Con *con);
/* Sobe/desce a largura da coluna focada em 'delta' pontos percentuais. */
void scrolling_resize_column(struct Con *column, int delta_ppt);
void scrolling_prepare(struct Con *con);
void scrolling_toggle_maximize(struct Con *con);
void scrolling_cycle_width(struct Con *con, bool backwards);
bool scrolling_resize_handled(struct Con *con, const char *direction, long px, long ppt);
void scrolling_scroll_by(struct Con *con, int dir);
void scrolling_resize_column_px(Con *column, int px);
void scrolling_forget(Con *con);
bool scrolling_animating(struct Con *con);
void scrolling_toggle_layout(struct Con *con);

int scrolling_column_width(struct Con *con, struct Con *child);
/* Ancestral mais próximo com layout de rolagem, ou NULL. */
struct Con *scrolling_container(struct Con *con);

extern scrolling_config_t scrolling_config;
