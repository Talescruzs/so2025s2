#include <stdbool.h>
#include <stdlib.h>
#include "metrica.h"

metricas *cria_metrica() {
    metricas *m = malloc(sizeof(metricas));
    if (m == NULL) {
        return NULL;
    }
    m->esta_ocioso = false;
    return m;
}

void mostra_metricas(metricas *m) {
    console_printf("Métricas do Sistema Operacional:");
    console_printf("processos criados: %d\n", m->n_processos_criados);
    console_printf("tempo total da execucao: %d\n", m->tempo_total_execucao);
    console_printf("tempo total da ocioso: %d\n", m->tempo_total_ocioso);
    console_printf("interrupcoes de reset: %d\n", m->n_interrupcoes_tipo[IRQ_RESET]);
    console_printf("interrupcoes de CPU: %d\n", m->n_interrupcoes_tipo[IRQ_ERR_CPU]);
    console_printf("interrupcoes de Relogio: %d\n", m->n_interrupcoes_tipo[IRQ_RELOGIO]);
    console_printf("interrupcoes de Sistema: %d\n", m->n_interrupcoes_tipo[IRQ_SISTEMA]);
    console_printf("interrupcoes de teclado: %d\n", m->n_interrupcoes_tipo[IRQ_TECLADO]);
    console_printf("interrupcoes de tela: %d\n", m->n_interrupcoes_tipo[IRQ_TELA]);
    console_printf("interrupcoes desconhecidas: %d\n", m->n_interrupcoes_tipo[6]);
    console_printf("numero de preempcoes: %d\n", m->n_preempcao);
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        console_printf("processo %d: tempo de retorno: %d, numero de preempcoes: %d\n", i, m->tempo_retorno[i], m->n_preempcao_processo[i]);
    }
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        console_printf("processo %d: entradas em estados - pronto: %d, bloqueado: %d, executando: %d\n", i, m->n_entradas_estado[i][PRONTO], m->n_entradas_estado[i][BLOQUEADO], m->n_entradas_estado[i][EXECUTANDO]);
    }
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        console_printf("processo %d: tempo em estados - pronto: %d, bloqueado: %d, executando: %d\n", i, m->tempo_estado[i][PRONTO], m->tempo_estado[i][BLOQUEADO], m->tempo_estado[i][EXECUTANDO]);
    }
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        int entradas = m->n_entradas_estado[i][BLOQUEADO];
        int tempo = m->tempo_estado[i][BLOQUEADO];
        int media = (entradas > 0) ? (tempo / entradas) : 0;
        console_printf("processo %d: tempo tempo medio de resposta: %d\n", i, media);
    }
}

void marca_preempcao(metricas *metri, es_t *relogio, int n_processo, int tempo_retorno) {
    int tempo_atual = 0, tempo_inicio = 0;
    
    metri->n_preempcao++;
    metri->n_preempcao_processo[n_processo]++;
    metri->tempo_retorno[n_processo] += tempo_retorno;

    es_le(relogio, D_RELOGIO_INSTRUCOES, &tempo_inicio);
    tempo_atual = tempo_inicio - metri->tempo_retorno[n_processo];

    metri->tempo_retorno[n_processo] += tempo_atual;
}

void entrou_ocioso(metricas *metri, es_t *relogio) {
    metri->esta_ocioso = true;
    es_le(relogio, D_RELOGIO_INSTRUCOES, &metri->tempo_inicio_ocioso);
}

void saiu_ocioso(metricas *metri, es_t *relogio) {
    metri->esta_ocioso = false;
    int tempo_atual = 0;
    es_le(relogio, D_RELOGIO_INSTRUCOES, &tempo_atual);

    metri->tempo_total_ocioso += tempo_atual - metri->tempo_inicio_ocioso;
}

void verifica_ocioso(metricas *metri, processo *tabela_processos, es_t *relogio) {
    bool metrica_antes = metri->esta_ocioso;
    metri->esta_ocioso = true;
    for (int i = 0; i < MAX_PROCESSOS; i++){
        if(tabela_processos[i].estado != BLOQUEADO && tabela_processos[i].estado != MORTO ){
        metri->esta_ocioso = false;
        break;
        }
    }
    if (metrica_antes != metri->esta_ocioso && metri->esta_ocioso == true) {
        entrou_ocioso(metri, relogio);
    }
    else if (metrica_antes != metri->esta_ocioso && metri->esta_ocioso == false) {
        saiu_ocioso(metri, relogio);
    }
    
}
