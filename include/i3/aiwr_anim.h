#pragma once
/*
 * i3-aiwr — relógio único das animações.
 *
 * Um ev_timer para tudo: cada animação registrada recebe o progresso já
 * passado pela curva, e no fim do frame sai UM xcb_flush e (se alguém pediu)
 * UM round-trip de sincronia. Com um timer por módulo os frames caíam em
 * fases diferentes e cada um dava seu próprio flush — era daí que vinha boa
 * parte do tranco.
 */
#include <stdbool.h>

/* p: 0..1 linear. e: p passado pela curva (pode passar de 1 com overshoot). */
typedef void (*aiwr_anim_step_cb)(double p, double e, void *data);
typedef void (*aiwr_anim_done_cb)(void *data);

void aiwr_anim_init(void);
void aiwr_anim_set_fps(int fps);
int aiwr_anim_fps(void);

/* Devolve um id > 0, ou 0 se não deu. duration_ms <= 0 executa o passo final
 * imediatamente e não registra nada. A curva é copiada por valor: redefinir
 * a curva depois não afeta animações em curso. */
int aiwr_anim_start(double duration_ms, const char *curve,
                    aiwr_anim_step_cb step, aiwr_anim_done_cb done, void *data);
/* Para sem chamar done (o dono está limpando sozinho). */
void aiwr_anim_cancel(int id);
/* Pula para o fim: step(1,1) + done. */
void aiwr_anim_finish(int id);
bool aiwr_anim_alive(int id);
bool aiwr_anim_any(void);
/* Chamar de dentro de um step: pede o round-trip de sincronia deste frame.
 * Só quem desenha (overlay) precisa; quem só manda configure, não. */
void aiwr_anim_request_fence(void);
void aiwr_anim_request_render(void);
