#ifndef METRICA_H
#define METRICA_H

#include <stdbool.h>
#include <stdlib.h>
#include "config.h"  // Inclui definições compartilhadas
#include "es.h"
#include "irq.h"
#include "processo.h"


typedef struct metricas {
    int n_processos_criados;
    int tempo_total_execucao;
    int tempo_total_ocioso;
    bool esta_ocioso;
    int tempo_inicio_ocioso;
    int n_interrupcoes_tipo[7];
    int n_preempcao;
    int tempo_retorno[MAX_PROCESSOS];
    int n_preempcao_processo[MAX_PROCESSOS];
    int n_entradas_estado[MAX_PROCESSOS][3]; // 3 estados: pronto, bloqueado, executando
    int tempo_estado[MAX_PROCESSOS][3];
    int tempo_inicio_estado[MAX_PROCESSOS][3];
    int tempo_medio_resposta[MAX_PROCESSOS];
} metricas;

metricas *cria_metrica();

void mostra_metricas(metricas *m);

void marca_preempcao(metricas *metri, es_t *relogio, int n_processo, int tempo_retorno);

void verifica_ocioso(metricas *metri, processo *tabela_processos, es_t *relogio);

#endif