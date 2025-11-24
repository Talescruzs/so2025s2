#include <stdlib.h>
#include <stdbool.h>

#include "memoria_quadros.h"
#include "console.h"

typedef struct quadro {
    bool livre; 
    int dono; 
    int pagina; 
} quadro;

typedef struct fila {
    int indice;
} fila;

struct mem_quadros_t {
    int cap;
    quadro *quadros;
    mem_q_tipo_t tipo;

    int f_ini;
    int f_tam;
    fila *f;
};

mem_quadros_t *mem_quadros_cria(int cap, int quadro_livre, mem_q_tipo_t tipo) {
    mem_quadros_t *mq = (mem_quadros_t*)malloc(sizeof(mem_quadros_t));
    mq->quadros = (quadro*)malloc(sizeof(quadro) * cap);
    mq->cap = cap;
    mq->tipo = tipo;
    for (int i = 0; i < cap; i++) {
        if (i < quadro_livre) {
            mq->quadros[i].livre = 0;
            mq->quadros[i].dono = -1; // restrito
            mq->quadros[i].pagina = 0;
        }
        else {
            mq->quadros[i].livre = 1;
            mq->quadros[i].dono = 0;
            mq->quadros[i].pagina = 0;
        }
    }

    mq->f_ini = 0;
    mq->f_tam = 0;
    mq->f = (fila*)malloc(sizeof(fila) * cap);
    return mq;
}

int mem_quadros_tem_livre(mem_quadros_t *self) {
    for (int i = 0; i < self->cap; i++) {
        if (self->quadros[i].livre) return i;
    }
    return -1;
}

int mem_quadros_adiciona_fila(mem_quadros_t *self, int indice) {
    if (self->f_tam >= self->cap) return -1;
    self->f[(self->f_ini + self->f_tam) % self->cap].indice = indice;
    self->f_tam++;
    return 0;
}

void mem_quadros_remove_fila(mem_quadros_t *self, int indice) {
    if (indice == -1) {
        self->f_ini = (self->f_ini + 1) % self->cap;
        self->f_tam--;
        return;
    }
    for (int i = self->f_ini; i < self->f_ini + self->f_tam; i++) {
        if (self->f[i % self->cap].indice == indice) {
            self->f_tam--;
            for (int j = i; j < (i + self->f_tam); j++) {
                self->f[j % self->cap] = self->f[(j + 1) % self->cap];
            }
            break;
        }
    }
}

void mem_quadros_manda_fim_fila(mem_quadros_t *self) {
    self->f[(self->f_ini + self->f_tam) % self->cap].indice = self->f[self->f_ini].indice;
    self->f_ini = (self->f_ini + 1) % self->cap;
}

void mem_quadros_muda_estado(mem_quadros_t *self, int indice, bool livre, int dono, int pagina) {
    self->quadros[indice].livre = livre;
    self->quadros[indice].dono = dono;
    self->quadros[indice].pagina = pagina;
    if (livre) {
        mem_quadros_remove_fila(self, indice);
    }
    else {
        mem_quadros_adiciona_fila(self, indice);
    }
}

int mem_quadros_libera_quadro_fifo(mem_quadros_t *self) {
    int indice = self->f[self->f_ini].indice;
    mem_quadros_remove_fila(self, -1);
    self->quadros[indice].livre = 1;
    return indice;
}

int mem_quadros_pega_dono(mem_quadros_t *self, int indice) {
    if (indice == -1) {
        return self->quadros[self->f[self->f_ini].indice].dono;
    }
    return self->quadros[indice].dono;
}

int mem_quadros_pega_pagina(mem_quadros_t *self, int indice) {
    if (indice == -1) {
        return self->quadros[self->f[self->f_ini].indice].pagina;
    }
    return self->quadros[indice].pagina;
}

void mem_quadros_remove_processo(mem_quadros_t *self, int pid) {
    for (int i = 0; i < self->cap; i++) {
        if (self->quadros[i].dono == pid) {
            self->quadros[i].livre = 1;
            mem_quadros_remove_fila(self, i);
        }
    }
}

int mem_quadros_pega_tam(mem_quadros_t *self) {
    return self->f_tam;
}

void mem_quadros_lista_fila(mem_quadros_t *self) {
    for (int i = self->f_ini; i < self->f_ini + self->f_tam; i++) {
        quadro q;
        q.dono = self->quadros[self->f[i % self->cap].indice].dono;
        q.pagina = self->quadros[self->f[i % self->cap].indice].pagina;
        console_printf("DONO: %d, PAGINA: %d", q.dono, q.pagina);
    }
}
