// swap.c
// memória secundária (swap)
// simulador de computador
// so25b

#include "swap.h"
#include "console.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Estrutura para controlar alocação de páginas na swap
typedef struct alocacao_t {
    int processo;       // PID do processo dono
    int end_inicio;     // Endereço inicial na swap
    int n_paginas;      // Número de páginas alocadas
    struct alocacao_t *prox;
} alocacao_t;

struct swap_t {
    int *dados;         // Dados da memória secundária
    int capacidade;     // Capacidade em palavras
    int tam_pagina;     // Tamanho de cada página
    int n_paginas;      // Número total de páginas
    int prox_livre;     // Próximo endereço livre (alocação sequencial)
    relogio_t *relogio; // Relógio para controle de tempo
    int disco_livre_em; // Momento em que o disco estará livre
    alocacao_t *alocacoes; // Lista de alocações
};

// UTILIZA A FILA DE ALOCACAO

alocacao_t *cria_alocacao(int processo, int end_inicio, int n_paginas) {
    console_printf("SWAP: criando alocação proc=%d end=%d pags=%d", processo, end_inicio, n_paginas);
    alocacao_t *aloc = malloc(sizeof(alocacao_t));
    assert(aloc != NULL);
    aloc->processo = processo;
    aloc->end_inicio = end_inicio;
    aloc->n_paginas = n_paginas;
    aloc->prox = NULL;
    console_printf("SWAP: alocação criada proc=%d end=%d pags=%d", processo, end_inicio, n_paginas);
    return aloc;
}

void destroi_alocacao(alocacao_t *aloc) {
    free(aloc);
}

void insere_final_alocacao(swap_t *self, alocacao_t *aloc) {
    console_printf("SWAP: inserindo alocação proc=%d end=%d pags=%d", aloc->processo, aloc->end_inicio, aloc->n_paginas);
    if(self->alocacoes == NULL) {
        self->alocacoes = aloc;
        return;
    }
    alocacao_t *atual = self->alocacoes;
    while(atual->prox != NULL) {
        atual = atual->prox;
    }
    atual->prox = aloc;
    console_printf("SWAP: alocação inserida proc=%d end=%d pags=%d", aloc->processo, aloc->end_inicio, aloc->n_paginas);
}

swap_t *swap_cria(int n_paginas, int tam_pagina, relogio_t *relogio)
{
    console_printf("SWAP: criando swap com %d páginas de %d palavras", n_paginas, tam_pagina);
    swap_t *self = malloc(sizeof(*self));
    assert(self != NULL);
    
    self->n_paginas = n_paginas;
    self->tam_pagina = tam_pagina;
    self->capacidade = n_paginas * tam_pagina;
    self->dados = malloc(self->capacidade * sizeof(int));
    assert(self->dados != NULL);
    
    // Inicializa com zeros
    memset(self->dados, 0, self->capacidade * sizeof(int));
    
    self->prox_livre = 0;
    self->relogio = relogio;
    self->disco_livre_em = 0;
    self->alocacoes = NULL;
    console_printf("SWAP: swap criada com sucesso");
    return self;
}

void swap_destroi(swap_t *self)
{
    if (self != NULL) {
        if (self->dados != NULL) {
            free(self->dados);
        }
        // Libera lista de alocações
        alocacao_t *atual = self->alocacoes;
        while (atual != NULL) {
            alocacao_t *prox = atual->prox;
            free(atual);
            atual = prox;
        }
        free(self);
    }
}

int swap_aloca(swap_t *self, int n_paginas, int processo)
{
    console_printf("SWAP: solicitada alocação proc=%d pags=%d", processo, n_paginas);
    if (self->prox_livre + n_paginas > self->n_paginas) {
        console_printf("SWAP: sem espaço para alocar %d páginas", n_paginas);
        return -1;
    }
    
    int end_inicio = self->prox_livre;
    self->prox_livre += n_paginas;
    
    // Registra alocação
    alocacao_t *aloc = cria_alocacao(processo, end_inicio, n_paginas);
    
    insere_final_alocacao(self, aloc);
    
    console_printf("SWAP: alocado proc=%d pags=%d end=%d", processo, n_paginas, end_inicio);
    return end_inicio;
}

void swap_libera_processo(swap_t *self, int processo)
{
    alocacao_t **pp = &self->alocacoes;
    while (*pp != NULL) {
        if ((*pp)->processo == processo) {
            alocacao_t *temp = *pp;
            *pp = (*pp)->prox;
            console_printf("SWAP: liberado proc=%d", processo);
            free(temp);
        } else {
            pp = &(*pp)->prox;
        }
    }
}

err_t swap_escreve_pagina(swap_t *self, int end_swap, int *dados, int tam, int *tempo_bloqueio)
{
    if (end_swap < 0 || end_swap >= self->n_paginas) {
        console_printf("SWAP: endereço inválido %d", end_swap);
        return ERR_END_INV;
    }
    
    int end_byte = end_swap * self->tam_pagina;
    
    // Copia dados para a swap
    for (int i = 0; i < tam && i < self->tam_pagina; i++) {
        self->dados[end_byte + i] = dados[i];
    }
    
    // Calcula tempo de bloqueio
    int agora;
    relogio_leitura(self->relogio, 0, &agora);
    
    if (agora >= self->disco_livre_em) {
        self->disco_livre_em = agora + TEMPO_ACESSO_DISCO;
    } else {
        self->disco_livre_em += TEMPO_ACESSO_DISCO;
    }
    
    *tempo_bloqueio = self->disco_livre_em;
    
    console_printf("SWAP: escrita end=%d tam=%d bloq_ate=%d", end_swap, tam, *tempo_bloqueio);
    return ERR_OK;
}

err_t swap_le_pagina(swap_t *self, int end_swap, int *dados, int tam, int *tempo_bloqueio)
{
    if (end_swap < 0 || end_swap >= self->n_paginas) {
        console_printf("SWAP: endereço inválido %d", end_swap);
        return ERR_END_INV;
    }
    
    int end_byte = end_swap * self->tam_pagina;
    
    // Copia dados da swap
    for (int i = 0; i < tam && i < self->tam_pagina; i++) {
        dados[i] = self->dados[end_byte + i];
    }
    
    // Calcula tempo de bloqueio
    int agora;
    relogio_leitura(self->relogio, 0, &agora);
    
    if (agora >= self->disco_livre_em) {
        self->disco_livre_em = agora + TEMPO_ACESSO_DISCO;
    } else {
        self->disco_livre_em += TEMPO_ACESSO_DISCO;
    }
    
    *tempo_bloqueio = self->disco_livre_em;
    
    console_printf("SWAP: leitura end=%d tam=%d bloq_ate=%d", end_swap, tam, *tempo_bloqueio);
    return ERR_OK;
}

int swap_endereco_pagina(swap_t *self, int processo, int pagina)
{
    // Procura alocação do processo
    alocacao_t *aloc = self->alocacoes;
    while (aloc != NULL) {
        if (aloc->processo == processo) {
            if (pagina < aloc->n_paginas) {
                return aloc->end_inicio + pagina;
            }
            return -1;
        }
        aloc = aloc->prox;
    }
    return -1;
}
