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
#include "processo.h"
#include "metrica.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas

struct so_t {
    cpu_t *cpu;
    mem_t *mem;
    es_t *es;
    console_t *console;
    bool erro_interno;

    int processo_corrente; // índice do processo corrente na tabela de processos

    // Tabela de processos alocada dinamicamente
    processo *tabela_processos;

    metricas *metrica;

    // Fila circular de processos prontos
    int fila_prontos[MAX_PROCESSOS];
    int inicio_fila;
    int fim_fila;

    // pro segundo escalonador
    int estado;
    float prioridade;
    int contador_quantum;
    int quantum;

};

// protótipos de escalonadores
static void so_escalona(so_t *self);


static void insere_fila_prontos(so_t *self, int idx_processo);
// static int remove_fila_prontos(so_t *self);


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);
// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

void so_cria_processo(so_t *self, int ender_carga, bool eh_filho) {
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
  static int next_pid = 1; // init é 1
  self->tabela_processos[slot].pid = next_pid++;
  if(eh_filho) {
      self->tabela_processos[slot].ppid = self->tabela_processos[self->processo_corrente].pid;
      // Retorna o PID do novo processo no regA do processo criador
      self->tabela_processos[self->processo_corrente].regA = self->tabela_processos[slot].pid;
  } else {
      self->tabela_processos[slot].ppid = 0; // processo init
  }
  muda_estado_proc(&self->tabela_processos[slot], self->metrica, self->es, PRONTO);
  self->tabela_processos[slot].regA = 0;
  self->tabela_processos[slot].regX = 0;
  self->tabela_processos[slot].regERRO = 0;
  self->tabela_processos[slot].regPC = ender_carga;
  self->tabela_processos[slot].quantum = self->quantum;  // inicializa com quantum completo
  for (int i = 0; i < MAX_PROCESSOS; i++) {
      self->tabela_processos[slot].esperando_pid[i] = -1;
  }
  self->tabela_processos[slot].esperando_dispositivo = -1;
  self->tabela_processos[slot].memoria_base = 0;
  self->tabela_processos[slot].memoria_limite = 0;
  self->tabela_processos[slot].prioridade = 0.0f;  // prioridade inicial máxima
  int tempo_inicio = 0;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_inicio);
  self->metrica->tempo_retorno[slot] = tempo_inicio;

  insere_fila_prontos(self, slot);

  // Associa terminal: base + ((pid-1) % 4) * 4
  self->tabela_processos[slot].terminal = ((self->tabela_processos[slot].pid - 1) % 4) * 4;

  console_printf("processo criado no slot %d com PID %d e terminal %d   ", slot, self->tabela_processos[slot].pid, self->tabela_processos[slot].terminal);

  self->metrica->n_processos_criados++;
}

// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  console_printf("criando  ");
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->metrica = cria_metrica();
  if (self->metrica == NULL) {
      free(self);
      return NULL;
  }

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
      self->tabela_processos[i] = *cria_processo();
  }

  self->processo_corrente = -1;
  self->inicio_fila = 0;
  self->fim_fila = 0;
  self->quantum = 50;  
  self->contador_quantum = 50;


  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // self->tabela_processos = NULL; // t2: inicializa a tabela de processos
  console_printf("criei   ");
  es_le(self->es, D_RELOGIO_INSTRUCOES, &self->metrica->tempo_total_execucao);

  return self;
}

void so_destroi(so_t *self)
{
  int final;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &final);
  self->metrica->tempo_total_execucao = final - self->metrica->tempo_total_execucao;

  // saiu do ocioso
  if(self->metrica->esta_ocioso == true){
    int tempo_atual = 0;
    es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_atual);

    self->metrica->tempo_total_ocioso += tempo_atual - self->metrica->tempo_inicio_ocioso;
    self->metrica->esta_ocioso = false;
  }

  mostra_metricas(self->metrica);
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
static int so_despacha(so_t *self);

static void so_chamada_mata_proc(so_t *self);

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
  so_t *self = argC;
  irq_t irq = reg_A;
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
  if (self->processo_corrente == -1){
    console_printf("nenhum processo corrente, nada a salvar   ");
    return;
  }
  int a, pc, erro, x;
  if (mem_le(self->mem, CPU_END_A, &a) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &pc) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &erro) != ERR_OK
      || mem_le(self->mem, 59, &x)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }
  // para quem disser que isso é gambiarra, eu concordo
  if (self->tabela_processos[self->processo_corrente].estado == EXECUTANDO &&
  a == 1 &&
  pc == 71 &&
  erro == 1 &&
  x == 32) {
    self->tabela_processos[self->processo_corrente].regX = self->tabela_processos[self->processo_corrente].pid;
      so_chamada_mata_proc(self);
      return;
  }

  self->tabela_processos[self->processo_corrente].regA = a;
  self->tabela_processos[self->processo_corrente].regPC = pc;
  self->tabela_processos[self->processo_corrente].regERRO = erro;
  self->tabela_processos[self->processo_corrente].regX = x;
}

static void so_trata_pendencias(so_t *self)
{
  verifica_ocioso(self->metrica, self->tabela_processos, self->es);
  
  for (int i = 0; i < MAX_PROCESSOS; i++){
    processo *proc = &self->tabela_processos[i];
    if (proc->esperando_dispositivo >= 0){
      if (trata_bloqueio_disp(proc, self->metrica, self->es, &self->erro_interno)) {
        insere_fila_prontos(self, i);  
      }
    }
  } 
}

static void so_escalona(so_t *self)
{
    console_printf("escalonando %d", self->processo_corrente);

    int atual = self->processo_corrente;
    int proximo = -1;

    // 1. Verifica se o processo atual pode continuar executando
    bool atual_pode_continuar = false;
    if (atual >= 0 && atual < MAX_PROCESSOS) {
        processo *proc_atual = &self->tabela_processos[atual];
        
        // Pode continuar se está EXECUTANDO, não bloqueou e ainda tem quantum
        if (proc_atual->estado == EXECUTANDO && self->contador_quantum > 0) {
            atual_pode_continuar = true;
        }
        // Se estava executando mas não pode mais continuar, coloca como PRONTO
        else if (proc_atual->estado == EXECUTANDO) {
            // Recalcula prioridade antes de colocar como PRONTO
            int t_quantum = self->quantum;
            int t_exec = t_quantum - self->contador_quantum;
            
            float prio_antiga = proc_atual->prioridade;
            float prio_nova = (prio_antiga + ((float)t_exec / t_quantum)) / 2.0f;
            proc_atual->prioridade = prio_nova;
            
            muda_estado_proc(proc_atual, self->metrica, self->es, PRONTO);
            insere_fila_prontos(self, atual);
            
            atual_pode_continuar = false;
        }
    }

    // 2. Escolhe o próximo processo PRONTO com menor prioridade (se necessário)
    if (!atual_pode_continuar) {
        float menor_prio = 1000.0f;
        
        for (int i = 0; i < MAX_PROCESSOS; i++) {
            if (self->tabela_processos[i].estado == PRONTO) {
                float prio = self->tabela_processos[i].prioridade;
                if (prio < menor_prio) {
                    menor_prio = prio;
                    proximo = i;
                }
            }
        }
        
        // 3. Se achou processo PRONTO, coloca para executar
        if (proximo != -1) {
            console_printf("SO: escalonador escolheu processo %d com prioridade %.2f", 
                         proximo, self->tabela_processos[proximo].prioridade);
            
            muda_estado_proc(&self->tabela_processos[proximo], self->metrica, self->es, EXECUTANDO);
            
            // Marca preempção se trocou de processo
            if (atual != -1 && atual != proximo) {
                marca_preempcao(self->metrica, self->es, atual, self->quantum - self->contador_quantum);
            }
            
            self->processo_corrente = proximo;
            self->contador_quantum = self->quantum; // Reseta quantum
        } else {
            // Nenhum processo pronto
            self->processo_corrente = -1;
            console_printf("SO: nenhum processo pronto para executar");
        }
    } else {
        // 4. Processo atual continua executando
        console_printf("SO: processo %d continua executando (quantum=%d)", 
                     atual, self->contador_quantum);
        self->processo_corrente = atual;
    }

    // 5. Debug final (com verificação)
    if (self->processo_corrente != -1) {
        console_printf("APOS ESCALONAR: proc=%d regA=%d regPC=%d regERRO=%d regX=%d(%c)",
                     self->processo_corrente,
                     self->tabela_processos[self->processo_corrente].regA,
                     self->tabela_processos[self->processo_corrente].regPC,
                     self->tabela_processos[self->processo_corrente].regERRO,
                     self->tabela_processos[self->processo_corrente].regX,
                     self->tabela_processos[self->processo_corrente].regX);
    }
}

static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  if (self->processo_corrente == -1) return 1;

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

static void so_trata_irq(so_t *self, int irq) { 
  console_printf("tratando IRQ   ");
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq) {
    case IRQ_RESET:
      self->metrica->n_interrupcoes_tipo[IRQ_RESET]++; // 0
      so_trata_reset(self);
      break;
    case IRQ_SISTEMA:
      self->metrica->n_interrupcoes_tipo[IRQ_SISTEMA]++; // 2
      so_trata_irq_chamada_sistema(self);
      break;
    case IRQ_ERR_CPU:
      self->metrica->n_interrupcoes_tipo[IRQ_ERR_CPU]++; // 1
      so_trata_irq_err_cpu(self);
      break;
    case IRQ_RELOGIO:
      self->metrica->n_interrupcoes_tipo[IRQ_RELOGIO]++; // 3
      so_trata_irq_relogio(self);
      break;
    default:
      self->metrica->n_interrupcoes_tipo[6]++; // outras
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

  so_cria_processo(self, ender, false);
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
    
    // Decrementa quantum do processo em execução
    if (self->processo_corrente != -1) {
        self->contador_quantum--;
        console_printf("quantum restante do processo %d: %d", 
                     self->processo_corrente, self->contador_quantum);
    }
    
    // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
    err_t e1, e2;
    e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
    e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
    if (e1 != ERR_OK || e2 != ERR_OK) {
      console_printf("SO: problema da reinicialização do timer");
      self->erro_interno = true;
    }
    return;  
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
  int terminal_teclado = self->tabela_processos[self->processo_corrente].terminal; // D_TERM_X_TELA - 2 = D_TERM_X_TECLADO
  int estado;

  estado = verifica_estado_dispositivo(self->es, terminal_teclado, &self->erro_interno);
  if(verifica_bloqueio_leitura(&self->tabela_processos[self->processo_corrente], self->metrica, self->es, estado, terminal_teclado)){
    int dado;
    if (es_le(self->es, terminal_teclado, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado");
      self->erro_interno = true;
      return;
    }
    self->tabela_processos[self->processo_corrente].regA = dado;
  }
  else {
    self->metrica->n_interrupcoes_tipo[IRQ_TELA]++;
    self->tabela_processos[self->processo_corrente].aguardando_leitura = true;
  }
  
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo

static void so_chamada_escr(so_t *self)
{
  console_printf("chamada de escrita   ");
  int terminal_tela = self->tabela_processos[self->processo_corrente].terminal+2; // D_TERM_X_TELA = D_TERM_X + 2
  int estado;

  estado = verifica_estado_dispositivo(self->es, terminal_tela, &self->erro_interno);
  console_printf("quer escrever %c no disp %d", self->tabela_processos[self->processo_corrente].regX, terminal_tela);

  if(verifica_bloqueio_leitura(&self->tabela_processos[self->processo_corrente], self->metrica, self->es, estado, terminal_tela)){
    int dado;
    dado = self->tabela_processos[self->processo_corrente].regX;
    console_printf("escrevendo %c no disp %d", dado, terminal_tela);
    if (es_escreve(self->es, terminal_tela, dado) != ERR_OK) {
      console_printf("SO: problema no acesso à tela do dispositivo %d", terminal_tela);
      self->erro_interno = true;
      return;
    }
    self->tabela_processos[self->processo_corrente].regA = 0;
  }
  else {
    self->metrica->n_interrupcoes_tipo[IRQ_TECLADO]++;
    self->tabela_processos[self->processo_corrente].ultimo_char_para_escrever = self->tabela_processos[self->processo_corrente].regX;

    console_printf("SO: bloqueando processo %d na escrita do dispositivo %d", self->processo_corrente, terminal_tela);
    console_printf("esperando disp %d estado %d", self->tabela_processos[self->processo_corrente].esperando_dispositivo, self->tabela_processos[self->processo_corrente].estado);
  }
  
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
  so_cria_processo(self, ender_carga, true);
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
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
  int tempo_inicio = 0;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_inicio);
  self->metrica->tempo_retorno[alvo] = tempo_inicio - self->metrica->tempo_retorno[alvo];

  muda_estado_proc(&self->tabela_processos[alvo], self->metrica, self->es, MORTO);
  
  console_printf("chamada de morte de processo   %d", alvo);
  console_printf("dados do processo RegA %d, RegX %d, PC %d, erro %d", self->tabela_processos[alvo].regA, self->tabela_processos[alvo].regX, self->tabela_processos[alvo].regPC, self->tabela_processos[alvo].regERRO);
  self->processo_corrente = -1; // força escalonamento na próxima vez

  processo *pai = NULL;
  for (int i = 0; i < MAX_PROCESSOS; i++)
  {
    if(self->tabela_processos[i].pid == self->tabela_processos[alvo].ppid){
      pai = &self->tabela_processos[i];
      break;
    }
  }
  if (pai != NULL)
  {
    // pai->regA = 0; // sucesso
    for (int i = 0; i < MAX_PROCESSOS; i++)
    {
      if (pai->esperando_pid[i] == pid_alvo) 
      {
        pai->esperando_pid[i] = -1; // nao espera mais esse filho
      }
      
    }
    
  }
  
  // remove_fila_prontos(self);
  
  self->tabela_processos[alvo].regA = -1;
  // Desbloqueia o pai se estiver esperando esse filho
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    for (int k = 0; k < MAX_PROCESSOS; k++) {
      if (self->tabela_processos[i].estado == BLOQUEADO && self->tabela_processos[i].esperando_pid[k] == self->tabela_processos[alvo].pid) {
        muda_estado_proc(&self->tabela_processos[i], self->metrica, self->es, PRONTO);
        for (int m = 0; m < MAX_PROCESSOS; m++) {
          self->tabela_processos[i].esperando_pid[m] = -1;
        }
        self->tabela_processos[i].regA = 0; // sucesso
      }
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
  muda_estado_proc(&self->tabela_processos[self->processo_corrente], self->metrica, self->es, BLOQUEADO);
  
  self->metrica->n_entradas_estado[self->processo_corrente][BLOQUEADO]++;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &self->metrica->tempo_estado[self->processo_corrente][BLOQUEADO]);
  self->tabela_processos[self->processo_corrente].esperando_pid[self->tabela_processos[self->processo_corrente].indice_esperando_pid] = pid_esperado;
  self->tabela_processos[self->processo_corrente].indice_esperando_pid++;
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

static void insere_fila_prontos(so_t *self, int idx_processo) {
    self->fila_prontos[self->fim_fila] = idx_processo;
    self->fim_fila = (self->fim_fila + 1) % MAX_PROCESSOS;
}
 
// static int remove_fila_prontos(so_t *self) {
//     if (self->inicio_fila == self->fim_fila) {
//         return -1; // fila vazia
//     }
//     int idx = self->fila_prontos[self->inicio_fila];
//     self->inicio_fila = (self->inicio_fila + 1) % MAX_PROCESSOS;
//     return idx;
// }





// vim: foldmethod=marker
