#ifndef PROCESSO_H
#define PROCESSO_H

#include "config.h"  // Inclui definições compartilhadas

#include <stdbool.h>
#include <stdlib.h>
#include "es.h"
#include "metrica.h"
#include "tabpag.h"



typedef struct processo {
    int pid;                    // identificador do processo
    int ppid;                   // identificador do processo pai
    int terminal;               // terminal associado ao processo (se aplicável)
    enum EstadoProcesso estado; // estado do processo (pronto, executando, bloqueado)
    int regA, regX, regPC, regERRO; // registradores salvos do processo
    int quantum;                // quantum restante (se/quando usar escalonamento por tempo)
    int esperando_pid[MAX_PROCESSOS];          // PID do processo que está esperando (SO_ESPERA_PROC)
    int esperando_dispositivo;  // dispositivo de E/S que está aguardando (se aplicável)
    int indice_esperando_pid;
    int memoria_base;           // endereço base da memória do processo (se/quando implementar)
    int memoria_limite;         // limite superior da memória do processo (se/quando implementar)
    struct processo *prox;      // ponteiro para próximo processo na fila (lista encadeada)
    float prioridade;

    int ultimo_char_para_escrever;
    bool aguardando_leitura; 

    // primeiro quadro da memória que está livre (quadros anteriores estão ocupados)
    // t3: com memória virtual, o controle de memória livre e ocupada deve ser mais
    //     completo que isso
    int quadro_livre;
    // uma tabela de páginas para poder usar a MMU
    // t3: com processos, não tem esta tabela global, tem que ter uma para
    //     cada processo
    tabpag_t *tabpag;
    
    // Campos para memória virtual
    int swap_inicio;            // endereço inicial na memória secundária
    int n_paginas;              // número de páginas do processo
    int tempo_desbloqueio;      // tempo até o qual o processo deve ficar bloqueado (I/O disco)
    int n_faltas_pagina;        // contador de faltas de página
    unsigned int lru_counter[100]; // contador LRU para cada página (máximo 100 páginas)
} processo;

processo *processo_cria(int id, int p_id, int pc, int max_quantum);

void insere_novo_processo(processo *self, processo *novo);

processo *encontra_processo_por_pid(processo *lista, int pid);

processo *encontra_processo_por_menor_prioridade(processo *lista);

void muda_estado_proc(processo *proc, metricas *metri, es_t *relogio, enum EstadoProcesso novo_estado);

bool verifica_bloqueio_leitura(processo *proc, metricas *metrica, es_t *es, int estado, int dispositivo);

int verifica_estado_dispositivo(es_t *es, int dispositivo, bool *erro_interno);

bool trata_bloqueio_disp(processo *proc, metricas *metrica, es_t *es, bool *erro_interno);

#endif // PROCESSO_H