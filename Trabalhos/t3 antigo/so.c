// so.c
// sistema operacional com suporte básico a memória virtual por paginação (T3)
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "mmu.h"
#include "tabpag.h"
#include "frame_table.h"
#include "swap.h"
#include "page_replace.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define MAX_PROCESSOS 4

enum EstadoProcesso {
    PRONTO,      /* 0 */
    EXECUTANDO,  /* 1 */
    BLOQUEADO,   /* 2 */
    MORTO       /* 3 */
};

typedef struct processo {
    int pid;                    // identificador do processo
    int ppid;                   // identificador do processo pai
    int terminal;               // terminal associado ao processo (se aplicável)
    enum EstadoProcesso estado; // estado do processo (pronto, executando, bloqueado)
    int regA, regX, regPC, regERRO; // registradores salvos do processo
    int regCOMPLEMENTO;         // registrador complemento (endereço/valor que causou erro)
    int quantum;                // quantum restante
    int esperando_pid;          // PID do processo que está esperando (SO_ESPERA_PROC)
    int esperando_dispositivo;  // dispositivo de E/S que está aguardando (se aplicável)
    int memoria_base;           // endereço base da memória do processo (não usado)
    int memoria_limite;         // limite superior da memória do processo (não usado)
    struct processo *prox;      // ponteiro para próximo processo na fila (lista encadeada)
    float prioridade;

    // campos para memória virtual (T3)
    tabpag_t *tab;           // tabela de páginas do processo
    int swap_base_page;      // base no swap (páginas)
    int num_paginas;         // número de páginas do processo
    int faltas_de_pagina;    // total de faltas atendidas (estatística)
    int bloqueado_ate;       // instante (relogio) até o qual está bloqueado por I/O
    bool esperando_swap;     // true se bloqueado por swap
} processo;

struct so_t {
    cpu_t *cpu;
    mmu_t *mmu;
    mem_t *mem; // memória física
    es_t *es;
    console_t *console;
    bool erro_interno;

    int processo_corrente; // índice do processo corrente na tabela de processos

    // Tabela de processos alocada dinamicamente
    processo *tabela_processos;

    // Fila circular de processos prontos
    int fila_prontos[MAX_PROCESSOS];
    int inicio_fila;
    int fim_fila;

    // pro segundo escalonador
    int estado;
    float prioridade;
    int contador_quantum;
    int quantum;

    // ponteiro para função de escalonamento (definida em runtime)
    void (*escalonador)(struct so_t *self);
};

// protótipos de escalonadores
static void so_escalona(so_t *self);
static void so_escalona2(so_t *self);

// API para escolher escalonador em runtime
void so_define_escalonador(so_t *self, int id);

static void insere_fila_prontos(so_t *self, int idx_processo);
static int remove_fila_prontos(so_t *self);

// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], so_t *self, int ender);

// subsistemas T3 (globais para o SO)
static frame_table_t *g_frame_table = NULL;
static swap_t *g_swap = NULL;
static int g_num_frames = 16;         // padrão, ajustável
static int g_io_time_per_page = 100;  // instruções por página (swap)
static repl_algo_t g_repl_algo = REPL_FIFO;

// protótipos internos
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static int so_despacha(so_t *self);
static void so_trata_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

// ---------------------------------------------------------------------
// CRIAÇÃO / DESTRUIÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mmu_t *mmu, es_t *es, console_t *console)
{
  console_printf("criando  ");
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->inicio_fila = 0;
  self->fim_fila = 0;
  self->processo_corrente = -1;
  self->quantum = 50;  // define o quantum inicial
  self->contador_quantum = 0;

  self->tabela_processos = malloc(sizeof(processo) * MAX_PROCESSOS);
  if (self->tabela_processos == NULL) { free(self); return NULL; }
  memset(self->tabela_processos, 0, sizeof(processo) * MAX_PROCESSOS);

  self->cpu = cpu;
  self->mmu = mmu;
  self->mem = NULL; // não assumir dono da mem física aqui
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // escalonador padrão: round-robin
  self->escalonador = so_escalona;

  for (int i = 0; i < MAX_PROCESSOS; i++) {
      self->tabela_processos[i].estado = MORTO; // inicializa todos os processos como mortos
      self->tabela_processos[i].tab = NULL;
      self->tabela_processos[i].swap_base_page = -1;
      self->tabela_processos[i].num_paginas = 0;
      self->tabela_processos[i].faltas_de_pagina = 0;
      self->tabela_processos[i].bloqueado_ate = 0;
      self->tabela_processos[i].esperando_swap = false;
      self->tabela_processos[i].esperando_dispositivo = -1;
      self->tabela_processos[i].esperando_pid = -1;
  }

  // registra a função CHAMAC da CPU
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // inicializa estruturas do subsistema de memória virtual (simples)
  g_frame_table = ft_create(g_num_frames);
  // cria swap com um backing store razoável (10000 palavras) e página TAM_PAGINA
  g_swap = swap_create(10000, TAM_PAGINA);
  pr_init(g_frame_table, g_repl_algo);

  console_printf("criei   ");

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);

  // libera estruturas por processo
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].tab) {
      tabpag_destroi(self->tabela_processos[i].tab);
      self->tabela_processos[i].tab = NULL;
    }
    if (self->tabela_processos[i].swap_base_page >= 0) {
      swap_free(g_swap, self->tabela_processos[i].swap_base_page, self->tabela_processos[i].num_paginas);
      self->tabela_processos[i].swap_base_page = -1;
    }
  }

  ft_destroy(g_frame_table);
  swap_destroy(g_swap);

  free(self->tabela_processos);
  free(self);
}

// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;

  // se for RESET, inicializa processo_corrente para 0 antes de salvar estado
  if (irq == IRQ_RESET) {
    self->processo_corrente = 0;
  }

  so_salva_estado_da_cpu(self);
  so_trata_irq(self, irq);
  so_trata_pendencias(self);
  if (self->escalonador) self->escalonador(self);
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // salva registradores do processo corrente lendo os valores no início da memória
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    self->erro_interno = true;
    return;
  }
  processo *proc = &self->tabela_processos[self->processo_corrente];
  int tmp;
  // usamos MMU em modo supervisor (acesso físico) para ler os locais onde a CPU salvou o estado
  if (mmu_le(self->mmu, CPU_END_A, &tmp, supervisor) == ERR_OK) proc->regA = tmp;
  if (mmu_le(self->mmu, CPU_END_PC, &tmp, supervisor) == ERR_OK) proc->regPC = tmp;
  if (mmu_le(self->mmu, CPU_END_erro, &tmp, supervisor) == ERR_OK) proc->regERRO = tmp;
  if (mmu_le(self->mmu, CPU_END_complemento, &tmp, supervisor) == ERR_OK) proc->regCOMPLEMENTO = tmp;
  if (mmu_le(self->mmu, 59, &tmp, supervisor) == ERR_OK) proc->regX = tmp;
}

static void so_trata_pendencias(so_t *self)
{
    for (int i = 0; i < MAX_PROCESSOS; i++){
      processo *proc = &self->tabela_processos[i];
      if (proc->estado == BLOQUEADO){
        if (proc->esperando_dispositivo >= 0) {
          int disp = proc->esperando_dispositivo;
          int estado_disp = 0;
          if (es_le(self->es, disp, &estado_disp) == ERR_OK && estado_disp != 0) {
                    proc->estado = PRONTO;
                    proc->esperando_dispositivo = -1;
                    proc->quantum = 0;
                    insere_fila_prontos(self, i);
                }
        }

        if (proc->esperando_pid > 0){
          bool terminou = true;
          for (int j = 0; j < MAX_PROCESSOS; j++){
            if (self->tabela_processos[j].pid == proc->esperando_pid && self->tabela_processos[j].estado != MORTO) {
              terminou = false;
              break;
            }
          }
          if (terminou) {
            proc->estado = PRONTO;
            proc->esperando_pid = -1;
            proc->quantum = 0;
            insere_fila_prontos(self, i);
          }
        }

        if (proc->esperando_swap) {
          int agora = 0;
          if (es_le(self->es, D_RELOGIO_INSTRUCOES, &agora) == ERR_OK) {
            if (agora >= proc->bloqueado_ate) {
              proc->esperando_swap = false;
              proc->estado = PRONTO;
              proc->quantum = 0;
              insere_fila_prontos(self, i);
            }
          }
        }
      }
    }
}

// ---------------------------------------------------------------------
// ESCALONADORES {{{1
// ---------------------------------------------------------------------

static void so_escalona(so_t *self)
{
    int atual = self->processo_corrente;
    int proximo = -1;
    int prontos = 0;

    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == PRONTO) {
            prontos++;
        }
    }

    if (atual >= 0 && atual < MAX_PROCESSOS &&
        self->tabela_processos[atual].estado == EXECUTANDO &&
        prontos > 0) {
        self->tabela_processos[atual].estado = PRONTO;
        insere_fila_prontos(self, atual);
    }

    int idx_fila = remove_fila_prontos(self);
    if (idx_fila != -1 && self->tabela_processos[idx_fila].estado == PRONTO) {
        proximo = idx_fila;
    } else {
        for (int i = 1; i <= MAX_PROCESSOS; i++) {
            int idx = (atual + i) % MAX_PROCESSOS;
            if (self->tabela_processos[idx].estado == PRONTO) {
                proximo = idx;
                break;
            }
        }
    }

    if (proximo != -1) {
        self->tabela_processos[proximo].estado = EXECUTANDO;
        self->processo_corrente = proximo;
        self->contador_quantum = self->quantum;
    } else if (atual >= 0 && atual < MAX_PROCESSOS &&
               self->tabela_processos[atual].estado != BLOQUEADO &&
               self->tabela_processos[atual].estado != MORTO) {
        self->tabela_processos[atual].estado = EXECUTANDO;
        self->processo_corrente = atual;
    } else {
        self->processo_corrente = -1;
    }
}

static void so_escalona2(so_t *self)
{
    // simplificação: uso de so_escalona para fallback
    so_escalona(self);
}

void so_define_escalonador(so_t *self, int id)
{
    if (id == 2) {
        self->escalonador = so_escalona2;
        console_printf("SO: usando escalonador 2 (prioridade)");
    } else {
        self->escalonador = so_escalona;
        console_printf("SO: usando escalonador 1 (round-robin)");
    }
}

// ---------------------------------------------------------------------
// DESPACHO {{{1
// ---------------------------------------------------------------------

static int so_despacha(so_t *self)
{
  if (self->processo_corrente < 0) {
    int tem_pronto = 0;
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado != MORTO) {
            if (self->tabela_processos[i].estado == PRONTO) tem_pronto = 1;
        }
    }
    if (tem_pronto) { self->erro_interno = true; return 1; }
    return 1;
  }
  if (self->processo_corrente >= MAX_PROCESSOS) { self->erro_interno = true; return 1; }

  processo *proc = &self->tabela_processos[self->processo_corrente];

  // configura a MMU para usar a tabela de páginas do processo corrente
  if (proc->tab) mmu_define_tabpag(self->mmu, proc->tab);
  else mmu_define_tabpag(self->mmu, NULL);

  // escreve registradores salvos nos locais do início da memória (ACESSO FÍSICO via supervisor)
  mmu_escreve(self->mmu, CPU_END_A, proc->regA, supervisor);
  mmu_escreve(self->mmu, CPU_END_PC, proc->regPC, supervisor);
  mmu_escreve(self->mmu, CPU_END_erro, proc->regERRO, supervisor);
  mmu_escreve(self->mmu, CPU_END_complemento, proc->regCOMPLEMENTO, supervisor);
  mmu_escreve(self->mmu, 59, proc->regX, supervisor);

  if (self->erro_interno) return 1;
  else return 0;
}

// ---------------------------------------------------------------------
// TRATAMENTO DE IRQs {{{1
// ---------------------------------------------------------------------

static void so_trata_irq(so_t *self, int irq)
{ 
  switch (irq) {
    case IRQ_RESET:
      so_trata_reset(self);
      break;
    case IRQ_SISTEMA:
      so_trata_irq_chamada_sistema(self);
      break;
    case IRQ_ERR_CPU:
      so_trata_irq_err_cpu(self);
      break;
    case IRQ_RELOGIO:
      so_trata_irq_relogio(self);
      break;
    default:
      so_trata_irq_desconhecida(self, irq);
  }
}

// chamada uma única vez, quando a CPU inicializa
static void so_trata_reset(so_t *self)
{
  // carrega o tratador de interrupção (mantém comportamento compatível T2)
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relogio
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // carrega init (comportamento similar ao T2)
  ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }

  // cria processo init
  self->processo_corrente = 0;
  processo *p0 = &self->tabela_processos[0];
  p0->pid = 1;
  p0->ppid = 0;
  p0->estado = EXECUTANDO;
  p0->regA = 0;
  p0->regX = 0;
  p0->regERRO = 0;
  p0->regPC = ender;
  p0->quantum = self->quantum;
  p0->esperando_pid = -1;
  p0->esperando_dispositivo = -1;
  p0->memoria_base = 0;
  p0->memoria_limite = 0;
  p0->terminal = D_TERM_A_TELA;
  p0->prioridade = 0.0f;

  // cria tabela de páginas vazia para init para compatibilidade
  p0->tab = tabpag_cria();
  p0->swap_base_page = -1;
  p0->num_paginas = 0;
  p0->faltas_de_pagina = 0;
  p0->esperando_swap = false;
}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    self->erro_interno = true;
    return;
  }
  processo *proc = &self->tabela_processos[self->processo_corrente];
  err_t err = proc->regERRO;

  if (err == ERR_PAG_AUSENTE) {
    // falta de página: identificar se acesso é ilegal ou page fault legítimo
    int endvirt = proc->regCOMPLEMENTO;
    int pagina = endvirt / TAM_PAGINA;

    if (pagina < 0 || pagina >= proc->num_paginas) {
      // acesso ilegal: matar processo
      proc->estado = MORTO;
      // desbloqueia possíveis processos esperando por este PID
      for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == BLOQUEADO && self->tabela_processos[i].esperando_pid == proc->pid) {
          self->tabela_processos[i].estado = PRONTO;
          self->tabela_processos[i].esperando_pid = -1;
          insere_fila_prontos(self, i);
        }
      }
      return;
    }

    // page fault legítimo: tratar carregando a página do swap para um quadro
    proc->faltas_de_pagina++;

    // 1) procurar quadro livre
    int quadro = ft_find_free(g_frame_table);
    if (quadro == -1) {
      // 2) escolher vítima via algoritmo
      quadro = pr_choose_victim();
      if (quadro == -1) {
        // erro sério: sem quadro possível
        self->erro_interno = true;
        return;
      }
      // 3) se vítima suja, escrever de volta para swap
      int vpid = -1, vvpage = -1;
      bool vdirty = false, vacc = false;
      if (ft_get_owner(g_frame_table, quadro, &vpid, &vvpage, &vdirty, &vacc) == 0 && vpid >= 0) {
        // localizar processo vítima
        processo *vproc = NULL;
        for (int i = 0; i < MAX_PROCESSOS; i++) {
          if (self->tabela_processos[i].pid == vpid) { vproc = &self->tabela_processos[i]; break; }
        }
        if (vproc && vproc->tab) {
          if (vdirty) {
            // copiar quadro para buffer (acesso físico em modo supervisor)
            int buf[TAM_PAGINA];
            for (int off = 0; off < TAM_PAGINA; off++) {
              int val;
              mmu_le(self->mmu, quadro * TAM_PAGINA + off, &val, supervisor);
              buf[off] = val;
            }
            // escrever no swap
            swap_write_page(g_swap, vproc->swap_base_page, vvpage, buf, TAM_PAGINA);
          }
          // invalidar entrada da tabela de páginas da vítima
          tabpag_invalida_pagina(vproc->tab, vvpage);
        }
      }
      // marco quadro como livre antes de usá-lo
      ft_free_frame(g_frame_table, quadro);
    }

    // 4) ler página do swap para o quadro (cópia para memória física via MMU em modo supervisor)
    int buffer[TAM_PAGINA];
    swap_read_page(g_swap, proc->swap_base_page, pagina, buffer, TAM_PAGINA);
    for (int off = 0; off < TAM_PAGINA; off++) {
      mmu_escreve(self->mmu, quadro * TAM_PAGINA + off, buffer[off], supervisor);
    }

    // 5) atualizar tabela de páginas do processo faltante
    tabpag_define_quadro(proc->tab, pagina, quadro);
    ft_set_frame(g_frame_table, quadro, proc->pid, pagina, false);

    // 6) agendar bloqueio pelo tempo de I/O simulado
    int agora = 0;
    if (es_le(self->es, D_RELOGIO_INSTRUCOES, &agora) != ERR_OK) agora = 0;
    int when = swap_schedule_io(g_swap, agora, 1, g_io_time_per_page);
    proc->bloqueado_ate = when;
    proc->esperando_swap = true;
    proc->estado = BLOQUEADO;
  } else {
    // outros erros: por enquanto marcar erro interno
    console_printf("SO: erro CPU não tratado: %s", err_nome(err));
    self->erro_interno = true;
  }
}

static void so_trata_irq_relogio(so_t *self)
{
  // rearma timer
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0);
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    self->erro_interno = true;
  }

  // decrementa quantum e força troca se necessário
  if (self->processo_corrente >= 0 && self->processo_corrente < MAX_PROCESSOS) {
    self->contador_quantum--;
    if (self->contador_quantum <= 0) {
      self->tabela_processos[self->processo_corrente].estado = PRONTO;
      insere_fila_prontos(self, self->processo_corrente);
      self->processo_corrente = -1;
    }
  }

  // rotina de envelhecimento para LRU (aproximação)
  if (g_repl_algo == REPL_LRU) {
    pr_on_clock_tick(self->processo_corrente);
  }
}

static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("SO: IRQ desconhecida %d\n", irq);
  self->erro_interno = true;
}

// ---------------------------------------------------------------------
// CHAMADAS DE SISTEMA {{{1
// ---------------------------------------------------------------------

static void so_chamada_le(so_t *self)
{
  int terminal_teclado = self->tabela_processos[self->processo_corrente].terminal - 2;
  int terminal_teclado_ok = self->tabela_processos[self->processo_corrente].terminal - 1;
  for (;;) {
    int estado;
    if (es_le(self->es, terminal_teclado_ok, &estado) != ERR_OK) { self->erro_interno = true; return; }
    if (estado != 0) break;
    console_tictac(self->console);
  }
  int dado;
  if (es_le(self->es, terminal_teclado, &dado) != ERR_OK) { self->erro_interno = true; return; }
  self->tabela_processos[self->processo_corrente].regA = dado;
}

static void so_chamada_escr(so_t *self)
{
  int terminal_tela_ok = self->tabela_processos[self->processo_corrente].terminal + 1;
  for (;;) {
    int estado;
    if (es_le(self->es, terminal_tela_ok, &estado) != ERR_OK) { self->erro_interno = true; return; }
    if (estado != 0) break;
    console_tictac(self->console);
  }
  int dado = self->tabela_processos[self->processo_corrente].regX;
  if (es_escreve(self->es, self->tabela_processos[self->processo_corrente].terminal, dado) != ERR_OK) { self->erro_interno = true; return; }
  self->tabela_processos[self->processo_corrente].regA = 0;
}

static void so_chamada_cria_proc(so_t *self)
{
  int ender_proc = self->tabela_processos[self->processo_corrente].regX;
  char nome[100];
  if (!copia_str_da_mem(100, nome, self, ender_proc)) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }

  programa_t *prog = prog_cria(nome);
  if (!prog) { self->tabela_processos[self->processo_corrente].regA = -1; return; }

  int tam = prog_tamanho(prog);
  int num_pages = (tam + TAM_PAGINA - 1) / TAM_PAGINA;

  int slot = -1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == MORTO) { slot = i; break; }
  }
  if (slot == -1) { prog_destroi(prog); self->tabela_processos[self->processo_corrente].regA = -1; return; }

  static int next_pid = 2;
  processo *np = &self->tabela_processos[slot];
  np->pid = next_pid++;
  np->ppid = self->tabela_processos[self->processo_corrente].pid;
  np->estado = PRONTO;
  np->regA = 0; np->regX = 0; np->regERRO = 0;
  np->regPC = 0; // começa em 0 no espaço virtual
  np->quantum = self->quantum;
  np->esperando_pid = -1; np->esperando_dispositivo = -1;
  np->memoria_base = 0; np->memoria_limite = 0;
  np->prioridade = 0.0f;
  np->tab = tabpag_cria();
  np->num_paginas = num_pages;
  np->swap_base_page = swap_alloc(g_swap, num_pages);
  if (np->swap_base_page < 0) {
    // sem espaço em swap
    tabpag_destroi(np->tab);
    np->tab = NULL;
    prog_destroi(prog);
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }
  np->faltas_de_pagina = 0;
  np->esperando_swap = false;

  // grava conteúdo do programa no swap, página por página
  for (int p = 0; p < num_pages; p++) {
    int buf[TAM_PAGINA];
    for (int off = 0; off < TAM_PAGINA; off++) {
      int idx = p * TAM_PAGINA + off;
      int val = 0;
      int abs_addr = prog_end_carga(prog) + idx;
      int d = prog_dado(prog, abs_addr);
      if (d >= 0) val = d;
      buf[off] = val;
    }
    swap_write_page(g_swap, np->swap_base_page, p, buf, TAM_PAGINA);
  }

  prog_destroi(prog);

  insere_fila_prontos(self, slot);
  self->tabela_processos[self->processo_corrente].regA = np->pid;
  np->terminal = D_TERM_A_TELA + ((np->pid - 1) % 4) * 4;
}

static void so_chamada_mata_proc(so_t *self)
{
  int pid_alvo = self->tabela_processos[self->processo_corrente].regX;
  int alvo = -1;
  if (pid_alvo == 0) alvo = self->processo_corrente;
  else {
    for (int i = 0; i < MAX_PROCESSOS; i++) {
      if (self->tabela_processos[i].pid == pid_alvo && self->tabela_processos[i].estado != MORTO) { alvo = i; break; }
    }
  }
  if (alvo == -1) { self->tabela_processos[self->processo_corrente].regA = -1; return; }
  // libera recursos do processo
  if (self->tabela_processos[alvo].tab) { tabpag_destroi(self->tabela_processos[alvo].tab); self->tabela_processos[alvo].tab = NULL; }
  if (self->tabela_processos[alvo].swap_base_page >= 0) { swap_free(g_swap, self->tabela_processos[alvo].swap_base_page, self->tabela_processos[alvo].num_paginas); self->tabela_processos[alvo].swap_base_page = -1; }
  self->tabela_processos[alvo].estado = MORTO;
  self->tabela_processos[self->processo_corrente].regA = 0;

  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == BLOQUEADO && self->tabela_processos[i].esperando_pid == self->tabela_processos[alvo].pid) {
      self->tabela_processos[i].estado = PRONTO;
      self->tabela_processos[i].esperando_pid = -1;
      self->tabela_processos[i].regA = 0;
    }
  }
}

static void so_chamada_espera_proc(so_t *self)
{
  int pid_esperado = self->tabela_processos[self->processo_corrente].regX;
  int existe = 0;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == pid_esperado && self->tabela_processos[i].estado != MORTO) { existe = 1; break; }
  }
  if (!existe) { self->tabela_processos[self->processo_corrente].regA = -1; return; }
  self->tabela_processos[self->processo_corrente].estado = BLOQUEADO;
  self->tabela_processos[self->processo_corrente].esperando_pid = pid_esperado;
}

static void so_trata_irq_chamada_sistema(so_t *self)
{
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) { self->erro_interno = true; return; }
  int id_chamada = self->tabela_processos[self->processo_corrente].regA;
  switch (id_chamada) {
    case SO_LE: so_chamada_le(self); break;
    case SO_ESCR: so_chamada_escr(self); break;
    case SO_CRIA_PROC: so_chamada_cria_proc(self); break;
    case SO_MATA_PROC: so_chamada_mata_proc(self); break;
    case SO_ESPERA_PROC: so_chamada_espera_proc(self); break;
    default: self->erro_interno = true;
  }
}

// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) return -1;
  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  // compatibilidade: carrega ROM / tratador em memória física (supervisor)
  for (int end = end_ini; end < end_fim; end++) {
    mmu_escreve(self->mmu, end, prog_dado(prog, end), supervisor);
  }
  prog_destroi(prog);
  return end_ini;
}

// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

static bool copia_str_da_mem(int tam, char str[tam], so_t *self, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    err_t r = mmu_le(self->mmu, ender + indice_str, &caractere, usuario);
    if (r == ERR_PAG_AUSENTE) {
      // leitura falhou por falta de página: retornamos false e o handler de IRQ tratará
      return false;
    } else if (r != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) return false;
    str[indice_str] = caractere;
    if (caractere == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------
// FILA DE PRONTOS {{{1
// ---------------------------------------------------------------------

static void insere_fila_prontos(so_t *self, int idx_processo) {
    self->fila_prontos[self->fim_fila] = idx_processo;
    self->fim_fila = (self->fim_fila + 1) % MAX_PROCESSOS;
}
 
static int remove_fila_prontos(so_t *self) {
    if (self->inicio_fila == self->fim_fila) {
        return -1; // fila vazia
    }
    int idx = self->fila_prontos[self->inicio_fila];
    self->inicio_fila = (self->inicio_fila + 1) % MAX_PROCESSOS;
    return idx;
}

// vim: foldmethod=marker