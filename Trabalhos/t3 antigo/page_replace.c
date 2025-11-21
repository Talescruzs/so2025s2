#include "page_replace.h"
#include <stdlib.h>

static frame_table_t *g_ft = NULL;
static repl_algo_t g_algo = REPL_FIFO;

void pr_init(frame_table_t *ft, repl_algo_t algo) {
  g_ft = ft;
  g_algo = algo;
}

void pr_on_clock_tick(int current_proc_idx) {
  // aproximacao simples: percorre frames e ajusta aging.
  // Aqui podemos ler bit de acesso via tabpag, mas para simplicidade
  // assumimos que ft_get_owner/ft_set_accessed mantêm bit de acesso.
  // Implementação de envelhecimento por frame:
  if (!g_ft) return;
  // ajusta aging: aging >>= 1 ; if accessed set MSB; then clear accessed
  // for (int f = 0; f < g_ft->n_frames; f++) {
  //   // não expor n_frames: usar ft_get_owner falhando, assumimos API expandida ou faça casting (simplificação)
  // }
}

int pr_choose_victim(void) {
  if (!g_ft) return -1;
  if (g_algo == REPL_FIFO) return ft_choose_victim_fifo(g_ft);
  return ft_choose_victim_lru(g_ft);
}