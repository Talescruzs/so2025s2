#ifndef SWAP_H
#define SWAP_H

#include "memoria.h"

typedef struct swap_t swap_t;

swap_t *swap_create(int total_words, int page_size);
void swap_destroy(swap_t *s);

/* Aloca um bloco contíguo de num_pages páginas; retorna base (página) ou -1 */
int swap_alloc(swap_t *s, int num_pages);
void swap_free(swap_t *s, int base_page, int num_pages);

/* Escreve/Le página (page_size palavras) da/para buffer */
void swap_write_page(swap_t *s, int base_page, int page_idx, int *buffer, int page_size);
void swap_read_page(swap_t *s, int base_page, int page_idx, int *buffer, int page_size);

/* Agenda I/O: retorna instante (relogio) até quando o disco ficará ocupado após transferir page_count páginas.
   'now' é o instante atual (instruções). 'io_time_per_page' tempo por página. */
int swap_schedule_io(swap_t *s, int now, int page_count, int io_time_per_page);

#endif // SWAP_H