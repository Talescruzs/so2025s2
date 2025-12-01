#include "processo.h"

#include <stdbool.h>
#include <stdlib.h>

processo *cria_processo() {
    processo *p = malloc(sizeof(processo));
    if (p == NULL) {
        return NULL;
    }
    p->pid = -1;
    p->ppid = -1;
    p->terminal = -1;
    p->estado = MORTO;
    p->regA = 0;
    p->regX = 0;
    p->regPC = 0;
    p->regERRO = 0;
    p->quantum = 0;
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        p->esperando_pid[i] = -1;
    }
    p->esperando_dispositivo = -1;
    p->indice_esperando_pid = 0;
    p->memoria_base = 0;
    p->memoria_limite = 0;
    p->prox = NULL;
    p->prioridade = 0.0f;
    p->ultimo_char_para_escrever = 0;
    p->aguardando_leitura = false;
    return p;
}

void muda_estado_proc(processo *proc, metricas *metri, es_t *relogio, enum EstadoProcesso novo_estado) {
    // processo *proc = &self->tabela_processos[processo_id];
    int pid = proc->pid;
    if(proc->estado == novo_estado) {
        return;
    }
    console_printf("muda estado do processo %d para %d   ", pid-1, novo_estado);

    // Marca tempo atual
    int tempo_atual = 0, tempo_inicio = 0;
    if(novo_estado == MORTO) {
      // Compara com a ultima mudanca de estado e soma no total de tempo do estado anterior
      es_le(relogio, D_RELOGIO_INSTRUCOES, &tempo_inicio);
      tempo_atual = tempo_inicio - metri->tempo_inicio_estado[pid-1][proc->estado];

      metri->tempo_estado[pid-1][proc->estado] += tempo_atual;
      // Muda estado
      proc->estado = novo_estado;
      return;
    }

    metri->n_entradas_estado[pid-1][novo_estado]++;

    // Compara com a ultima mudanca de estado e soma no total de tempo do estado anterior
    es_le(relogio, D_RELOGIO_INSTRUCOES, &tempo_inicio);
    tempo_atual = tempo_inicio - metri->tempo_inicio_estado[pid-1][proc->estado];
    metri->tempo_estado[pid-1][proc->estado] += tempo_atual;
    // Muda estado
    proc->estado = novo_estado;
    // Marca tempo de inicio do novo estado
    es_le(relogio, D_RELOGIO_INSTRUCOES, &metri->tempo_inicio_estado[pid-1][novo_estado]);
}

// BLOQUEIOS DE DISPOSITIVOS DE E/S

bool verifica_bloqueio_leitura(processo *proc, metricas *metrica, es_t *es, int estado, int dispositivo) {
  console_printf("verificando bloqueio de E/S do dispositivo %d ", dispositivo);
  if (estado != 0) return true;
  console_printf("estado zero   ");
  muda_estado_proc(proc, metrica, es, BLOQUEADO);
  proc->esperando_dispositivo = dispositivo;
  return false;
}

int verifica_estado_dispositivo(es_t *es, int dispositivo, bool *erro_interno) {
  int estado;
  int dispositivo_ok = dispositivo + 1; // D_TERM_X + 1 = D_TERM_X_OK
  if (es_le(es, dispositivo_ok, &estado) != ERR_OK) {
    console_printf("SO: problema no acesso ao estado do dispositivo");
    *erro_interno = true;
    return -1;
  }
  console_printf("estado do dispositivo %d: %d   ", dispositivo_ok, estado);
  return estado;
}

bool trata_bloqueio_disp(processo *proc, metricas *metrica, es_t *es, bool *erro_interno) {
    console_printf("verificando desbloqueio do processo %d   ", proc->pid-1);
    console_printf("dispositivo %d   ", proc->esperando_dispositivo);
    int estado_disp = verifica_estado_dispositivo(es, proc->esperando_dispositivo, erro_interno);
    if (verifica_bloqueio_leitura(proc, metrica, es, estado_disp, proc->esperando_dispositivo)) { 
        console_printf("%d pronto", proc->esperando_dispositivo);
        
        if(proc->aguardando_leitura == false){
            int dado = proc->ultimo_char_para_escrever;
            console_printf("escrevendo %c no disp %d", dado, proc->esperando_dispositivo);
            if (es_escreve(es, proc->esperando_dispositivo, dado) != ERR_OK) {
                console_printf("SO: problema no acesso à tela do dispositivo %d", proc->esperando_dispositivo);
                *erro_interno = true;
                return false;
            }
            proc->regA = 0;
        }
        else{
            int dado;
            if (es_le(es, proc->esperando_dispositivo, &dado) != ERR_OK) {
                console_printf("SO: problema no acesso ao teclado do dispositivo %d", proc->esperando_dispositivo);
                *erro_interno = true;
                return false;
            }
            console_printf("lendo %c do disp %d", dado, proc->esperando_dispositivo);
            proc->regA = dado;
            proc->aguardando_leitura = false;
        }

        muda_estado_proc(proc, metrica, es, PRONTO);
        proc->esperando_dispositivo = -1;
        proc->quantum = 0;
        return true;              
    }
    return false;              
}

