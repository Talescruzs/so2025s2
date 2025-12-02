// swap.h
// memória secundária (swap)
// simulador de computador
// so25b

#ifndef SWAP_H
#define SWAP_H

#include "err.h"
#include "relogio.h"

typedef struct swap_t swap_t;

// Tempo de transferência de uma página entre memória principal e secundária
#define TEMPO_ACESSO_DISCO 10

// Cria uma memória secundária com capacidade para 'n_paginas' páginas
swap_t *swap_cria(int n_paginas, int tam_pagina, relogio_t *relogio);

// Destrói a memória secundária
void swap_destroi(swap_t *self);

// Aloca espaço na memória secundária para um processo
// Retorna o endereço inicial alocado ou -1 em caso de erro
int swap_aloca(swap_t *self, int n_paginas, int processo);

// Libera todas as páginas alocadas para um processo
void swap_libera_processo(swap_t *self, int processo);

// Escreve uma página na memória secundária
// Retorna o tempo até o qual o processo deve ficar bloqueado
err_t swap_escreve_pagina(swap_t *self, int end_swap, int *dados, int tam, int *tempo_bloqueio);

// Lê uma página da memória secundária
// Retorna o tempo até o qual o processo deve ficar bloqueado
err_t swap_le_pagina(swap_t *self, int end_swap, int *dados, int tam, int *tempo_bloqueio);

// Retorna o endereço na memória secundária para uma página de um processo
int swap_endereco_pagina(swap_t *self, int processo, int pagina);

#endif // SWAP_H
