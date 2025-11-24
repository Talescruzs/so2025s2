#ifndef MEMORIA_QUADROS_H
#define MEMORIA_QUADROS_H

typedef struct mem_quadros_t mem_quadros_t;

typedef enum {
    MEM_Q_FIFO,
    MEM_Q_SC
} mem_q_tipo_t;

mem_quadros_t *mem_quadros_cria(int cap, int quadro_livre, mem_q_tipo_t tipo);
void mem_quadros_manda_fim_fila(mem_quadros_t *self);
int mem_quadros_tem_livre(mem_quadros_t *self);
void mem_quadros_muda_estado(mem_quadros_t *self, int indice, bool livre, int dono, int pagina);
int mem_quadros_libera_quadro_fifo(mem_quadros_t *self);
int mem_quadros_pega_dono(mem_quadros_t *self, int indice);
int mem_quadros_pega_pagina(mem_quadros_t *self, int indice);
void mem_quadros_remove_processo(mem_quadros_t *self, int pid);
int mem_quadros_pega_tam(mem_quadros_t *self);
void mem_quadros_lista_fila(mem_quadros_t *self);

#endif
