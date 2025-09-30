// so.c
// sistema operacional
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

#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define MAX_PROCESSOS 4

// modificacao:
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
    int quantum;                // quantum restante (se/quando usar escalonamento por tempo)
    int esperando_pid;          // PID do processo que está esperando (SO_ESPERA_PROC)
    int esperando_dispositivo;  // dispositivo de E/S que está aguardando (se aplicável)
    int memoria_base;           // endereço base da memória do processo (se/quando implementar)
    int memoria_limite;         // limite superior da memória do processo (se/quando implementar)
    struct processo *prox;      // ponteiro para próximo processo na fila (lista encadeada)
} processo;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  bool erro_interno;

  // int regA, regX, regPC, regERRO; // cópia do estado da CPU
  int processo_corrente; // índice do processo corrente na tabela de processos
  processo *tabela_processos; // t2: tabela de processos, processo corrente, pendências, etc
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  console_printf("criando  ");
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  self->tabela_processos = malloc(MAX_PROCESSOS * sizeof(processo));
  if (self->tabela_processos == NULL) {
      free(self);
      return NULL;
  }
  for (int i = 0; i < MAX_PROCESSOS; i++) {
      self->tabela_processos[i].estado = MORTO; // inicializa todos os processos como mortos

      
      // if(i == 0) self->tabela_processos[i].terminal = D_TERM_A_TELA;
      // else if(i == 1) self->tabela_processos[i].terminal = D_TERM_B_TELA;
      // else if(i == 2) self->tabela_processos[i].terminal = D_TERM_C_TELA;
      // else if(i == 3) self->tabela_processos[i].terminal = D_TERM_D_TELA;
  }

  self->processo_corrente = -1; // nenhum processo está executando

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // self->tabela_processos = NULL; // t2: inicializa a tabela de processos
  console_printf("criei   ");

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}


// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC, no tratador de
//   interrupção em assembly
// essa é a única forma de entrada no SO depois da inicialização
// na inicialização do SO, a CPU foi programada para chamar esta função para executar
//   a instrução CHAMAC
// a instrução CHAMAC só deve ser executada pelo tratador de interrupção
//
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// o valor retornado por esta função é colocado no registrador A, e pode ser
//   testado pelo código que está após o CHAMAC. No tratador de interrupção em
//   assembly esse valor é usado para decidir se a CPU deve retornar da interrupção
//   (e executar o código de usuário) ou executar PARA e ficar suspensa até receber
//   outra interrupção
static int so_trata_interrupcao(void *argC, int reg_A)
{
  console_printf("interrompido   ");
  so_t *self = argC;
  irq_t irq = reg_A;

  // Corrige: se for RESET, inicializa processo_corrente para 0 antes de salvar estado
  if (irq == IRQ_RESET) {
    self->processo_corrente = 0;
  }

  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
  console_printf("salvando estado da CPU   ");
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    console_printf("SO: processo_corrente inválido em salva_estado");
    self->erro_interno = true;
    return;
  }
  if (mem_le(self->mem, CPU_END_A, &self->tabela_processos[self->processo_corrente].regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &self->tabela_processos[self->processo_corrente].regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &self->tabela_processos[self->processo_corrente].regERRO) != ERR_OK
      || mem_le(self->mem, 59, &self->tabela_processos[self->processo_corrente].regX)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }
}

static void so_trata_pendencias(so_t *self)
{
  // t2: realiza ações que não são diretamente ligadas com a interrupção que
  //   está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades
  // - etc
}

static void so_escalona(so_t *self)
{
  console_printf("escalonando   %d", self->processo_corrente);
    int atual = self->processo_corrente;
    int proximo = -1;
    int prontos = 0;

    // Conta quantos processos PRONTO existem
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == PRONTO) {
            prontos++;
        }
    }

    // Só coloca o processo corrente em PRONTO se houver outro processo PRONTO
    if (atual >= 0 && atual < MAX_PROCESSOS &&
        self->tabela_processos[atual].estado == EXECUTANDO &&
        prontos > 0) {
        self->tabela_processos[atual].estado = PRONTO;
    }

    // Procura próximo processo PRONTO (round-robin)
    for (int i = 1; i <= MAX_PROCESSOS; i++) {
        int idx = (atual + i) % MAX_PROCESSOS;
        if (self->tabela_processos[idx].estado == PRONTO) {
            proximo = idx;
            break;
        }
    }

    // Se achou, coloca como EXECUTANDO
    if (proximo != -1) {
        self->tabela_processos[proximo].estado = EXECUTANDO;
        self->processo_corrente = proximo;
    } else if (atual >= 0 && atual < MAX_PROCESSOS &&
               self->tabela_processos[atual].estado != BLOQUEADO &&
               self->tabela_processos[atual].estado != MORTO) {
        // Se não achou, mas o atual não está bloqueado ou morto, mantém EXECUTANDO
        self->tabela_processos[atual].estado = EXECUTANDO;
        self->processo_corrente = atual;
    } else {
        // Nenhum processo pronto/executando
        self->processo_corrente = -1;
    }
}

static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  console_printf("despachando   %d  ", self->processo_corrente);
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    console_printf("SO: processo_corrente inválido em despacha");
    self->erro_interno = true;
    return 1;
  }
  if (mem_escreve(self->mem, CPU_END_A, self->tabela_processos[self->processo_corrente].regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->tabela_processos[self->processo_corrente].regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->tabela_processos[self->processo_corrente].regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->tabela_processos[self->processo_corrente].regX) != ERR_OK) {
    console_printf("SO: erro na escrita dos registradores");
    self->erro_interno = true;
  }
  if (self->erro_interno) return 1;
  else return 0;
}


// ---------------------------------------------------------------------
// TRATAMENTO DE UMA IRQ {{{1
// ---------------------------------------------------------------------

// funções auxiliares para tratar cada tipo de interrupção
static void so_trata_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

static void so_trata_irq(so_t *self, int irq)
{ 
  console_printf("tratando IRQ   ");
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
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
  console_printf("recebi RESET   ");
  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor,
  //   salva seu estado à partir do endereço CPU_END_PC, e desvia para o
  //   endereço CPU_END_TRATADOR
  // colocamos no endereço CPU_END_TRATADOR o programa de tratamento
  //   de interrupção (escrito em asm). esse programa deve conter a
  //   instrução CHAMAC, que vai chamar so_trata_interrupcao (como
  //   foi definido na inicialização do SO)
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // t2: deveria criar um processo para o init, e inicializar o estado do
  //   processador para esse processo com os registradores zerados, exceto
  //   o PC e o modo.
  // como não tem suporte a processos, está carregando os valores dos
  //   registradores diretamente no estado da CPU mantido pelo SO; daí vai
  //   copiar para o início da memória pelo despachante, de onde a CPU vai
  //   carregar para os seus registradores quando executar a instrução RETI
  //   em bios.asm (que é onde está a instrução CHAMAC que causou a execução
  //   deste código
  ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
  self->processo_corrente = 0;
  self->tabela_processos[0].pid = 1;
  self->tabela_processos[0].ppid = 0;
  self->tabela_processos[0].estado = EXECUTANDO;
  self->tabela_processos[0].regA = 0;
  self->tabela_processos[0].regX = 0;
  self->tabela_processos[0].regERRO = 0;
  self->tabela_processos[0].regPC = ender;
  self->tabela_processos[0].quantum = 0;
  self->tabela_processos[0].esperando_pid = -1;
  self->tabela_processos[0].esperando_dispositivo = -1;
  self->tabela_processos[0].memoria_base = 0;
  self->tabela_processos[0].memoria_limite = 0;
  self->tabela_processos[0].terminal = D_TERM_A_TELA;
  



  // coloca o programa init na memória
  // ender = so_carrega_programa(self, "init.maq");
  // if (ender != 100) {
  //   console_printf("SO: problema na carga do programa inicial");
  //   self->erro_interno = true;
  //   return;
  // }

  // altera o PC para o endereço de carga
  // self->tabela_processos->regPC = ender; // deveria ser no processo
}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  console_printf("erro na CPU   ");
  // Ocorreu um erro interno na CPU
  // O erro está codificado em CPU_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  // t2: com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)
  err_t err = self->tabela_processos[self->processo_corrente].regERRO;
  console_printf("SO: IRQ não tratada -- erro na CPU: %s", err_nome(err));
  self->erro_interno = true;
}

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  console_printf("interrupção do relógio   ");
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  console_printf("SO: interrupção do relógio (não tratada)");
}

// foi gerada uma interrupção para a qual o SO não está preparado
static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("tratando IRQ desconhecida   ");
  console_printf("SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  self->erro_interno = true;
}


// ---------------------------------------------------------------------
// CHAMADAS DE SISTEMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares para cada chamada de sistema
static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);

static void so_trata_irq_chamada_sistema(so_t *self)
{
  console_printf("chamada de sistema   ");
  // a identificação da chamada está no registrador A
  // t2: com processos, o reg A deve estar no descritor do processo corrente
  if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    console_printf("SO: processo_corrente inválido em chamada_sistema");
    self->erro_interno = true;
    return;
  }
  int id_chamada = self->tabela_processos[self->processo_corrente].regA;
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
    case SO_LE:
      so_chamada_le(self);
      break;
    case SO_ESCR:
      so_chamada_escr(self);
      break;
    case SO_CRIA_PROC:
      so_chamada_cria_proc(self);
      break;
    case SO_MATA_PROC:
      so_chamada_mata_proc(self);
      break;
    case SO_ESPERA_PROC:
      so_chamada_espera_proc(self);
      break;
    default:
      console_printf("SO: chamada de sistema desconhecida (%d)", id_chamada);
      // t2: deveria matar o processo
      self->erro_interno = true;
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  console_printf("chamada de leitura   ");
  int terminal_teclado = self->tabela_processos[self->processo_corrente].terminal - 2; // D_TERM_X_TELA - 2 = D_TERM_X_TECLADO
  int terminal_teclado_ok = self->tabela_processos[self->processo_corrente].terminal - 1; // D_TERM_X_TELA - 1 = D_TERM_X_TECLADO_OK
  for (;;) {  // espera ocupada!
    int estado;
    if (es_le(self->es, terminal_teclado_ok, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado do teclado");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    console_tictac(self->console);
  }
  int dado;
  if (es_le(self->es, terminal_teclado, &dado) != ERR_OK) {
    console_printf("SO: problema no acesso ao teclado");
    self->erro_interno = true;
    return;
  }
  self->tabela_processos[self->processo_corrente].regA = dado;
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  console_printf("chamada de escrita   ");
  int terminal_tela_ok = self->tabela_processos[self->processo_corrente].terminal + 1; // D_TERM_X_TELA + 1 = D_TERM_X_TELA_OK
  for (;;) {
    int estado;
    if (es_le(self->es, terminal_tela_ok, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado da tela");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    console_tictac(self->console);
  }
  int dado;
  dado = self->tabela_processos[self->processo_corrente].regX;
  if (es_escreve(self->es, self->tabela_processos[self->processo_corrente].terminal, dado) != ERR_OK) {
    console_printf("SO: problema no acesso à tela");
    self->erro_interno = true;
    return;
  }
  self->tabela_processos[self->processo_corrente].regA = 0;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  console_printf("chamada de criação de processo   ");
  int ender_proc = self->tabela_processos[self->processo_corrente].regX;
  char nome[100];
  if (!copia_str_da_mem(100, nome, self->mem, ender_proc)) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }

  int ender_carga = so_carrega_programa(self, nome);
  if (ender_carga <= 0) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }

  // Procura slot livre (MORTO ou nunca usado)
  int slot = -1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == MORTO) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }

  // Inicializa novo processo
  static int next_pid = 2; // init é 1
  self->tabela_processos[slot].pid = next_pid++;
  self->tabela_processos[slot].ppid = self->tabela_processos[self->processo_corrente].pid;
  self->tabela_processos[slot].estado = PRONTO;
  self->tabela_processos[slot].regA = 0;
  self->tabela_processos[slot].regX = 0;
  self->tabela_processos[slot].regERRO = 0;
  self->tabela_processos[slot].regPC = ender_carga;
  self->tabela_processos[slot].quantum = 0;
  self->tabela_processos[slot].esperando_pid = -1;
  self->tabela_processos[slot].esperando_dispositivo = -1;
  self->tabela_processos[slot].memoria_base = 0;
  self->tabela_processos[slot].memoria_limite = 0;

  // Retorna o PID do novo processo no regA do processo criador
  self->tabela_processos[self->processo_corrente].regA = self->tabela_processos[slot].pid;

  self->tabela_processos[slot].terminal = D_TERM_A_TELA + (self->tabela_processos[slot].pid-1%4)*4; // Associa terminal baseado no slot

  // if(i == 0) self->tabela_processos[i].terminal = D_TERM_A_TELA;

}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  console_printf("chamada de morte de processo   ");
  int pid_alvo = self->tabela_processos[self->processo_corrente].regX;
  int alvo = -1;
  if (pid_alvo == 0) {
    alvo = self->processo_corrente;
  } else {
    for (int i = 0; i < MAX_PROCESSOS; i++) {
      if (self->tabela_processos[i].pid == pid_alvo && self->tabela_processos[i].estado != MORTO) {
        alvo = i;
        break;
      }
    }
  }
  if (alvo == -1) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }
  self->tabela_processos[alvo].estado = MORTO;
  self->tabela_processos[alvo].regA = -1;
  // Desbloqueia o pai se estiver esperando esse filho
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == BLOQUEADO && self->tabela_processos[i].esperando_pid == self->tabela_processos[alvo].pid) {
      self->tabela_processos[i].estado = PRONTO;
      self->tabela_processos[i].esperando_pid = -1;
      self->tabela_processos[i].regA = 0; // sucesso
    }
  }
  self->tabela_processos[self->processo_corrente].regA = 0;
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  console_printf("chamada de espera de processo   ");
  int pid_esperado = self->tabela_processos[self->processo_corrente].regX;
  int existe = 0;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == pid_esperado && self->tabela_processos[i].estado != MORTO) {
      existe = 1;
      break;
    }
  }
  if (!existe) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }
  self->tabela_processos[self->processo_corrente].estado = BLOQUEADO;
  self->tabela_processos[self->processo_corrente].esperando_pid = pid_esperado;
}


// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  console_printf("carregando programa   ");
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// t2: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  console_printf("copia_str_da_mem: %d\n", ender);
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

// vim: foldmethod=marker
