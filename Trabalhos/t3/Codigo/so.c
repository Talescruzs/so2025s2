// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "cpu.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "tabpag.h"
#include "memoria_quadros.h"
#include "swap.h"
#include "relogio.h"
#include "processo.h"
#include "metrica.h"


#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas

#define QUANTUM 50
#define MEM_TAM 200 // tamanho da memória principal


// Não tem processos nem memória virtual, mas é preciso usar a paginação,
//   pelo menos para implementar relocação, já que os programas estão sendo
//   todos montados para serem executados no endereço 0 e o endereço 0
//   físico é usado pelo hardware nas interrupções.
// Os programas estão sendo carregados no início de um quadro, e usam quantos
//   quadros forem necessárias. Para isso a variável quadro_livre contém
//   o número do primeiro quadro da memória principal que ainda não foi usado.
//   Na carga do processo, a tabela de páginas (deveria ter uma por processo,
//   mas não tem processo) é alterada para que o endereço virtual 0 resulte
//   no quadro onde o programa foi carregado. Com isso, o programa carregado
//   é acessível, mas o acesso ao anterior é perdido.


// t3: Identificação de processos
//   Usa-se int para representar o PID (Process ID) de um processo.
//   NENHUM_PROCESSO (-1): indica operação na memória física (sem processo associado)
//   ALGUM_PROCESSO (0): usado para inicialização quando o PID específico não importa
//
// Processos válidos têm PID >= 1 (definido em so_cria_processo)
#define NENHUM_PROCESSO NULL
#define ALGUM_PROCESSO 0

#define ESC_TIPO ESC_PRIORIDADE
#define MEM_Q_TIPO MEM_Q_SC


typedef struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO, regComplemento; // cópia do estado da CPU

  processo *processo_corrente; // índice do processo corrente na tabela de processos

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

  // alocador global simples de quadros (novo)
  mem_quadros_t *quadros;
  int next_quadro_livre;

  int quadro_livre_pri;
  int quadro_livre_sec;


  // Memória secundária e relógio
  swap_t *swap;
  relogio_t *relogio;

} so_t;


// protótipos de escalonadores
static void so_escalona(so_t *self);



// static int remove_fila_prontos(so_t *self);

// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// no t3, foi adicionado o 'processo' aos argumentos dessas funções 
// carrega o programa contido no arquivo para memória virtual de um processo
// retorna o endereço virtual inicial de execução
static int so_carrega_programa(so_t *self, processo *processo,
                               char *nome_do_executavel);
// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo* processo);


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mmu_t *mmu,
              es_t *es, console_t *console, relogio_t *relogio)
{
  console_printf("criando  ");
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->inicio_fila = 0;
  self->fim_fila = 0;
  self->processo_corrente = NULL;
  self->quantum = QUANTUM;  // define o quantum inicial
  self->contador_quantum = 0;
  self->next_quadro_livre = 0; 

  /* aloca e inicializa a tabela de processos uma única vez */
  self->tabela_processos = NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->relogio = relogio;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // // inicializa a tabela de páginas por processo
  // for (int i = 0; i < MAX_PROCESSOS; i++) {
  //     self->tabela_processos[i].estado = MORTO; // inicializa todos os processos como mortos
  //     self->tabela_processos[i].tabpag  = tabpag_cria();
  //     self->tabela_processos[i].swap_inicio = -1;
  //     self->tabela_processos[i].n_paginas = 0;
  //     self->tabela_processos[i].tempo_desbloqueio = 0;
  //     self->tabela_processos[i].n_faltas_pagina = 0;
  //     for (int j = 0; j < 100; j++) {
  //         self->tabela_processos[i].lru_counter[j] = 0;
  //     }
  // }

  self->processo_corrente = 0; // nenhum processo está executando

  self->quadro_livre_pri = 99 / TAM_PAGINA + 1;
  self->quadro_livre_sec = 0;

  self->quadros = mem_quadros_cria(MEM_TAM / TAM_PAGINA, 99 / TAM_PAGINA + 1, MEM_Q_TIPO);
  
  // Cria memória secundária (swap) - tamanho generoso para todos os processos
  self->swap = swap_cria(1000, TAM_PAGINA, relogio);

  console_printf("criei   ");

  return self;
}
void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  
  // Imprime estatísticas antes de destruir
  console_printf("\n========== ESTATÍSTICAS DO SISTEMA ==========\n");
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid > 0) {
      console_printf("Processo PID=%d: %d faltas de página\n",
                     self->tabela_processos[i].pid,
                     self->tabela_processos[i].n_faltas_pagina);
    }
  }
  console_printf("Tamanho da memória principal: %d palavras (%d páginas)\n",
                 mem_tam(self->mem), mem_tam(self->mem) / TAM_PAGINA);
  console_printf("Tamanho de página: %d palavras\n", TAM_PAGINA);
  console_printf("=============================================\n");
  
  // Libera memória
  if (self->swap) swap_destroi(self->swap);
  if (self->quadros) free(self->quadros);
  
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
  
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  if(irq == 1){
    console_printf("A = %d, X = %d, PC = %d", self->regA, self->regX, self->regPC);

  }
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  if(self->tabela_processos == NULL){
    console_printf("Tabela de processos nula");
    return 1;
  }
  // recupera o estado do processo escolhido
  if (self->tabela_processos[0].estado != MORTO) {
      int retorno = so_despacha(self);
      console_printf("RETORNO DESPACHA: %d", retorno);
      return retorno;
  }
  return 1;
}

static void so_chamada_mata_proc(so_t *self);


static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
  if (self->tabela_processos == NULL || self->processo_corrente->estado != EXECUTANDO){
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
  if (
    a == 1 &&
  pc == 71 &&
  erro == 1 &&
  x == 32) {
    self->processo_corrente->regX = self->processo_corrente->pid;
      so_chamada_mata_proc(self);
      return;
  }

  self->processo_corrente->regA = a;
  self->processo_corrente->regPC = pc;
  self->processo_corrente->regERRO = erro;
  self->processo_corrente->regX = x;
}

static void so_trata_pendencias(so_t *self)
{
  console_printf("tratando pendências   ");
  if (self->tabela_processos == NULL) return;
  
  verifica_ocioso(self->metrica, self->tabela_processos, self->es);
  
  for (int i = 0; i < MAX_PROCESSOS; i++){
    processo *proc = &self->tabela_processos[i];
    if (proc == NULL){
      return;
    }
    if (proc->esperando_dispositivo >= 0){
      trata_bloqueio_disp(proc, self->metrica, self->es, &self->erro_interno);
    }
  } 

}

// AUXILIARES DE ESCALONADOR  
// Verifica se o processo atual pode continuar executando
static bool processo_atual_pode_continuar(so_t *self, processo *atual)
{
    if (atual == NULL) {
        return false;
    }

    // Pode continuar se está EXECUTANDO, não bloqueou e ainda tem quantum
    return (atual->estado == EXECUTANDO && self->contador_quantum > 0);
}

// Processa fim de quantum: recalcula prioridade e coloca processo como PRONTO
static void processa_fim_quantum(so_t *self, processo *atual)
{
    if (atual == NULL) {
        return;
    }

    if (atual->estado != EXECUTANDO) {
        return;
    }

    // Recalcula prioridade
    int t_quantum = self->quantum;
    int t_exec = t_quantum - self->contador_quantum;
    
    float prio_antiga = atual->prioridade;
    float prio_nova = (prio_antiga + ((float)t_exec / t_quantum)) / 2.0f;
    atual->prioridade = prio_nova;
    
    // Coloca como PRONTO e insere na fila
    muda_estado_proc(atual, self->metrica, self->es, PRONTO);
}

// Escolhe o próximo processo PRONTO com menor prioridade
static processo *escolhe_proximo_processo(so_t *self)
{
    processo *proximo = encontra_processo_por_menor_prioridade(self->tabela_processos);
    
    return proximo;
}

// Ativa um processo para execução
static void ativa_processo(so_t *self, processo *proximo, processo *atual)
{
    if (proximo == NULL) {
        return;
    }

    console_printf("SO: escalonador escolheu processo %d com prioridade %.2f", 
                 proximo->pid, proximo->prioridade);
    
    muda_estado_proc(proximo, self->metrica, self->es, EXECUTANDO);
    
    // Marca preempção se trocou de processo
    if (atual != NULL && atual != proximo) {
        marca_preempcao(self->metrica, self->es, atual->pid-1, self->quantum - self->contador_quantum);
    }
    
    self->processo_corrente = proximo;
    self->contador_quantum = self->quantum; // Reseta quantum
}

static void so_escalona(so_t *self)
{
    console_printf("escalonando %d", self->processo_corrente);

    processo *atual = self->processo_corrente;
    
    // 1. Verifica se o processo atual pode continuar executando
    bool atual_pode_continuar = processo_atual_pode_continuar(self, atual);
    
    // 2. Se não pode continuar, processa fim de quantum
    if (!atual_pode_continuar && atual != NULL) {
        processa_fim_quantum(self, atual);
    }

    // 3. Se precisa escolher novo processo
    if (!atual_pode_continuar) {
        processo *proximo = escolhe_proximo_processo(self);
        
        if (proximo != NULL) {
            // Ativa o próximo processo
            ativa_processo(self, proximo, atual);
        } else {
            // Nenhum processo pronto
            self->processo_corrente = NULL;
            console_printf("SO: nenhum processo pronto para executar");
        }
    } else {
        // 4. Processo atual continua executando
        console_printf("SO: processo %d continua executando (quantum=%d)", 
                     atual, self->contador_quantum);
        self->processo_corrente = atual;
    }

    // 5. Debug final (com verificação)
    if (self->processo_corrente != NULL) {
        console_printf("APOS ESCALONAR: proc=%d regA=%d regPC=%d regERRO=%d regX=%d(%c)",
                     self->processo_corrente,
                     self->processo_corrente->regA,
                     self->processo_corrente->regPC,
                     self->processo_corrente->regERRO,
                     self->processo_corrente->regX,
                     self->processo_corrente->regX);
    }
}

static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc. e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  
  if (self->processo_corrente == NULL) {
    return 1;
  }
  
  if (self->processo_corrente->estado == MORTO) {
    return 1;
  }
  
  // Configura a MMU com a tabela de páginas do processo corrente
  mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
  
  if (mem_escreve(self->mem, CPU_END_A, self->processo_corrente->regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->processo_corrente->regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->processo_corrente->regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->processo_corrente->regX)) {
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
  int ender = so_carrega_programa(self, NENHUM_PROCESSO, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }
  
  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }
  // define o primeiro quadro livre de memória como o seguinte àquele que
  //   contém o endereço final da memória protegida (que não podem ser usadas
  //   por programas de usuário)
  // t3: o controle de memória livre deve ser mais aprimorado que isso
  
  // teste

    self->next_quadro_livre = CPU_END_FIM_PROT / TAM_PAGINA + 1;
  // opcional: inicializar também os quadro_livre de cada processo como 0

  
  // for (int i = 0; i < MAX_PROCESSOS; i++) {
  //     self->tabela_processos[i].quadro_livre = 0;
  // }

  // Inicializa o processo 0 ANTES de carregar o programa
  self->processo_corrente = 0;
  processo *p_init = processo_cria(1, -1, ender, self->quantum);
  
  ender = so_carrega_programa(self, p_init, "init.maq");
  if (ender == -1) {
    console_printf("SO: problema na carga do programa inicial, ender = %d", ender);
    self->erro_interno = true;
    return;
  }
  p_init->regPC = ender;
  
  
  insere_novo_processo(self->tabela_processos, p_init);
}

// funções auxiliares para tratamento de falta de página

// Aloca um quadro livre ou libera um ocupado usando substituição de páginas
static int so_aloca_quadro(so_t *self)
{
  // Procura por um quadro livre
  int quadro = mem_quadros_tem_livre(self->quadros);
  
  if (quadro >= 0) {
    console_printf("SO: quadro %d alocado (livre)", quadro);
    return quadro;
  }
  
  // Não há quadros livres - precisa substituir uma página
  console_printf("SO: sem quadros livres, substituindo página (FIFO)");
  
  // Obtém o quadro a ser liberado (FIFO)
  quadro = mem_quadros_libera_quadro_fifo(self->quadros);
  
  // Obtém informações sobre a página que está sendo substituída
  int dono_pid = mem_quadros_pega_dono(self->quadros, quadro);
  int pagina_vitima = mem_quadros_pega_pagina(self->quadros, quadro);
  
  console_printf("SO: substituindo pag=%d proc=%d quadro=%d", pagina_vitima, dono_pid, quadro);
  
  // Encontra o processo dono
  processo *proc_dono = encontra_processo_por_pid(self->tabela_processos, dono_pid);
  
  if (proc_dono == NULL) {
    console_printf("SO: erro - processo dono %d não encontrado", dono_pid);
    return -1;
  }
  
  // Verifica se a página foi alterada
  bool alterada = tabpag_bit_alteracao(proc_dono->tabpag, pagina_vitima);
  
  if (alterada) {
    console_printf("SO: página alterada, salvando na swap");
    
    // Salva página na swap
    int end_swap = swap_endereco_pagina(self->swap, dono_pid, pagina_vitima);
    if (end_swap < 0) {
      console_printf("SO: erro ao obter endereço na swap");
      return -1;
    }
    
    // Lê dados da página da memória principal
    int dados[TAM_PAGINA];
    int end_fis = quadro * TAM_PAGINA;
    for (int i = 0; i < TAM_PAGINA; i++) {
      mem_le(self->mem, end_fis + i, &dados[i]);
    }
    
    // Escreve na swap
    int tempo_bloqueio;
    swap_escreve_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio);
    
    // Bloqueia o processo dono se for diferente do corrente
    if (proc_dono != self->processo_corrente && self->processo_corrente->estado != MORTO) {
      proc_dono->estado = BLOQUEADO;
      proc_dono->tempo_desbloqueio = tempo_bloqueio;
    }
  }
  
  // Invalida a página na tabela do processo dono
  tabpag_invalida_pagina(proc_dono->tabpag, pagina_vitima);
  
  return quadro;
}

// Trata uma falta de página
static void so_trata_falta_pagina(so_t *self, processo* proc, int pagina)
{
  fprintf(stderr, "DEBUG: so_trata_falta_pagina proc=%d pag=%d\n", proc->pid, pagina);
  console_printf("SO: tratando falta de página %d do processo idx=%d", pagina, proc->pid);
  
  // Aloca um quadro (pode fazer substituição)
  int quadro = so_aloca_quadro(self);
  if (quadro < 0) {
    fprintf(stderr, "DEBUG: erro ao alocar quadro\n");
    console_printf("SO: erro ao alocar quadro");
    proc->estado = MORTO;
    return;
  }
  
  fprintf(stderr, "DEBUG: quadro alocado=%d\n", quadro);
  
  // Obtém endereço da página na swap
  int end_swap = swap_endereco_pagina(self->swap, proc->pid, pagina);
  fprintf(stderr, "DEBUG: end_swap=%d pid=%d\n", end_swap, proc->pid);
  
  if (end_swap < 0) {
    fprintf(stderr, "DEBUG: erro ao obter endereço da página na swap\n");
    console_printf("SO: erro ao obter endereço da página na swap");
    proc->estado = MORTO;
    return;
  }
  
  // Lê a página da swap
  int dados[TAM_PAGINA];
  int tempo_bloqueio;
  if (swap_le_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio) != ERR_OK) {
    console_printf("SO: erro ao ler página da swap");
    proc->estado = MORTO;
    return;
  }
  
  // Escreve os dados no quadro da memória principal
  int end_fis = quadro * TAM_PAGINA;
  for (int i = 0; i < TAM_PAGINA; i++) {
    mem_escreve(self->mem, end_fis + i, dados[i]);
  }
  
  // Atualiza a tabela de páginas
  tabpag_define_quadro(proc->tabpag, pagina, quadro);
  
  // Registra o quadro como ocupado
  mem_quadros_muda_estado(self->quadros, quadro, false, proc->pid, pagina);
  
  // Bloqueia o processo até que a operação de disco termine
  proc->estado = BLOQUEADO;
  proc->tempo_desbloqueio = tempo_bloqueio;
  
  console_printf("SO: página %d mapeada no quadro %d, processo bloqueado até %d", 
                 pagina, quadro, tempo_bloqueio);
}

// interrupção gerada quando a CPU identifica um erro

static void so_trata_irq_err_cpu(so_t *self)
{
  console_printf("erro na CPU   ");

  processo *proc = self->processo_corrente;
  if (proc == NULL) {
    console_printf("SO: não há processo corrente válido ao tratar erro da CPU\n");
    return;
  }

  err_t err = proc->regERRO;
  int pid = proc->pid;
  int pc  = proc->regPC;
  
  // Lê o complemento da CPU para obter o endereço que causou a falha
  mem_le(self->mem, CPU_END_complemento, &self->regComplemento);
  
  console_printf("SO: IRQ de ERRO na CPU: %s (complemento=%d) proc_idx=%d pid=%d pc=%d",
                 err_nome(err), self->regComplemento, proc->pid, pid, pc);

  // Trata falta de página
  if (err == ERR_PAG_AUSENTE) {
    int end_virt = self->regComplemento;
    int pagina = end_virt / TAM_PAGINA;
    
    // Verifica se o endereço é válido para o processo
    if (pagina < 0 || pagina >= proc->n_paginas) {
      console_printf("SO: acesso inválido à página %d (processo tem %d páginas)",
                     pagina, proc->n_paginas);
      // Segmentation fault - mata o processo
      proc->estado = MORTO;
      
      // Libera recursos
      if (proc->swap_inicio >= 0) {
        swap_libera_processo(self->swap, pid);
      }
      mem_quadros_remove_processo(self->quadros, pid);
      
      console_printf("SO: processo %d morto por segmentation fault", pid);
      return;
    }
    
    // Falta de página válida - trata
    console_printf("SO: falta de página %d do processo %d", pagina, pid);
    so_trata_falta_pagina(self, proc, pagina);
    
    // Incrementa contador de faltas
    proc->n_faltas_pagina++;
    
    return;
  }
  
  // Outros erros - mata o processo
  console_printf("SO: erro fatal %s - matando processo %d", err_nome(err), pid);
  proc->estado = MORTO;
  
  // Libera recursos
  if (proc->swap_inicio >= 0) {
    swap_libera_processo(self->swap, pid);
  }
  mem_quadros_remove_processo(self->quadros, pid);
  if (proc->tabpag != NULL) {
    tabpag_destroi(proc->tabpag);
    proc->tabpag = tabpag_cria();
  }


  console_printf("SO: processo idx=%d pid=%d marcado como MORTO", proc->pid, pid);
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
  
  // Envelhecimento LRU para o processo corrente
  if (self->processo_corrente != NULL) {
    processo *proc = self->processo_corrente;
    
    if (proc->estado == EXECUTANDO) {
      // Envelhecimento: desloca bits para direita e adiciona bit de acesso
      for (int pag = 0; pag < proc->n_paginas; pag++) {
        // Desloca para direita (divide por 2)
        proc->lru_counter[pag] >>= 1;
        
        // Adiciona bit de acesso no bit mais significativo
        if (tabpag_bit_acesso(proc->tabpag, pag)) {
          proc->lru_counter[pag] |= 0x80000000; // Seta bit mais significativo
          tabpag_zera_bit_acesso(proc->tabpag, pag); // Zera o bit de acesso
        }
      }
    }
  }
  
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonamento com quantum
  if (self->processo_corrente != NULL) {
    self->contador_quantum--;
    if (self->contador_quantum <= 0) {
      // Quantum esgotado, força troca de contexto
      console_printf("SO: quantum esgotado para processo %d", self->processo_corrente->pid);
      self->processo_corrente->estado = PRONTO;
      self->processo_corrente = NULL;  // força troca de processo
      return;
    }
  }
  console_printf("SO: interrupção do relógio (não tratada)");
}

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

  if (self->processo_corrente == NULL) {
    console_printf("SO: processo_corrente inválido em chamada_sistema");
    self->erro_interno = true;
    return;
  }

  int id_chamada = self->processo_corrente->regA;
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

static void so_chamada_le(so_t *self)
{
  console_printf("chamada de leitura   ");
  int terminal_teclado = self->processo_corrente->terminal; // D_TERM_X_TELA - 2 = D_TERM_X_TECLADO
  int estado;

  estado = verifica_estado_dispositivo(self->es, terminal_teclado, &self->erro_interno);
  if(verifica_bloqueio_leitura(self->processo_corrente, self->metrica, self->es, estado, terminal_teclado)){
    int dado;
    if (es_le(self->es, terminal_teclado, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado");
      self->erro_interno = true;
      return;
    }
    self->processo_corrente->regA = dado;
  }
  else {
    self->metrica->n_interrupcoes_tipo[IRQ_TELA]++;
    self->processo_corrente->aguardando_leitura = true;
  }
  
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo

static void so_chamada_escr(so_t *self)
{
  console_printf("chamada de escrita   ");
  int terminal_tela = self->processo_corrente->terminal+2; // D_TERM_X_TELA = D_TERM_X + 2
  int estado;

  estado = verifica_estado_dispositivo(self->es, terminal_tela, &self->erro_interno);
  console_printf("quer escrever %c no disp %d", self->processo_corrente->regX, terminal_tela);
  if(verifica_bloqueio_leitura(self->processo_corrente, self->metrica, self->es, estado, terminal_tela)){
    int dado;
    dado = self->processo_corrente->regX;
    console_printf("escrevendo %c no disp %d", dado, terminal_tela);
    if (es_escreve(self->es, terminal_tela, dado) != ERR_OK) {
      console_printf("SO: problema no acesso à tela do dispositivo %d", terminal_tela);
      self->erro_interno = true;
      return;
    }
    self->processo_corrente->regA = 0;
  }
  else {
    self->metrica->n_interrupcoes_tipo[IRQ_TECLADO]++;
    self->processo_corrente->ultimo_char_para_escrever = self->processo_corrente->regX;
    console_printf("SO: bloqueando processo %d na escrita do dispositivo %d", self->processo_corrente, terminal_tela);
    console_printf("esperando disp %d estado %d", self->processo_corrente->esperando_dispositivo, self->processo_corrente->estado);
  }
  
}
// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo



// --- alteração em so_chamada_cria_proc: encontrar slot ANTES de carregar ---

//TESTE

static void so_chamada_cria_proc(so_t *self)
{
  console_printf("chamada de criação de processo   ");

  /* valida processo corrente */
  if (self->processo_corrente == NULL) {
    console_printf("SO: processo_corrente inválido em cria_proc\n");
    self->erro_interno = true;
    return;
  }

  /* obter endereço virtual (no regX do processo chamador) que aponta para o nome */
  int ender_proc = self->processo_corrente->regX;
  char nome[100];

  /* copia string do espaço do processo chamador */
  if (!so_copia_str_do_processo(self, 100, nome, ender_proc,
                                self->processo_corrente)) {
    /* falha ao copiar: retorna erro para o chamador */
    self->processo_corrente->regA = -1;
    return;
  }

  static int next_pid = 2; /* init já é 1 */

  processo *proc = processo_cria(next_pid++, self->processo_corrente->pid, 0, self->quantum);

  /* carrega o programa no slot correto (so_carrega_programa usa 'processo' para decidir) */
  int ender_carga = so_carrega_programa(self, proc, nome);
  if (ender_carga <= 0) {
    /* falha na carga */
    self->processo_corrente->regA = -1;
    return;
  }

  /* inicializa o novo processo no slot */
  proc->regPC = ender_carga;
  insere_novo_processo(self->tabela_processos, proc);

  /* retorna o PID do novo processo no regA do processo criador */
  self->processo_corrente->regA = proc->pid;

  console_printf("SO: processo criado pid=%d pc=%d",
                 proc->pid, proc->regPC);
}





// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  int pid_alvo = self->processo_corrente->regX;
  processo *alvo = encontra_processo_por_pid(self->tabela_processos, pid_alvo);
  
  if (alvo == NULL || alvo->estado == MORTO) {
    self->processo_corrente->regA = -1;
    return;
  }
  int tempo_inicio = 0;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_inicio);
  self->metrica->tempo_retorno[alvo->pid-1] = tempo_inicio - self->metrica->tempo_retorno[alvo->pid-1];

  muda_estado_proc(alvo, self->metrica, self->es, MORTO);
  
  console_printf("chamada de morte de processo   %d", alvo->pid);
  console_printf("dados do processo RegA %d, RegX %d, PC %d, erro %d", alvo->regA, alvo->regX, alvo->regPC, alvo->regERRO);
  self->processo_corrente = NULL; // força escalonamento na próxima vez

  processo *pai = encontra_processo_por_pid(self->tabela_processos, alvo->ppid);
  if (pai != NULL)
  {

    pai->esperando_pid[alvo->pid-1] = -1; // nao espera mais esse filho
    
  }
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  console_printf("chamada de espera de processo   ");
  int pid_esperado = self->processo_corrente->regX;
  int existe = 0;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == pid_esperado && self->tabela_processos[i].estado != MORTO) {
      existe = 1;
      break;
    }
  }
  if (!existe) {
    self->processo_corrente->regA = -1;
    return;
  }
  muda_estado_proc(self->processo_corrente, self->metrica, self->es, BLOQUEADO);
  
  self->metrica->n_entradas_estado[self->processo_corrente->pid-1][BLOQUEADO]++;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &self->metrica->tempo_estado[self->processo_corrente->pid-1][BLOQUEADO]);
  self->processo_corrente->esperando_pid[self->processo_corrente->indice_esperando_pid] = pid_esperado;
  self->processo_corrente->indice_esperando_pid++;
}

static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo *processo);

// carrega o programa na memória
// se processo for NENHUM_PROCESSO, carrega o programa na memória física
//   senão, carrega na memória virtual do processo
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, processo* processo,
                               char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);

  programa_t *programa = prog_cria(nome_do_executavel);
  if (programa == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_carga;
  if (processo == NENHUM_PROCESSO) {
    end_carga = so_carrega_programa_na_memoria_fisica(self, programa);
  } else {
    end_carga = so_carrega_programa_na_memoria_virtual(self, programa, processo);
  }


  prog_destroi(programa);

  return end_carga;
}

static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa)
{
  int end_ini = prog_end_carga(programa);
  int end_fim = end_ini + prog_tamanho(programa);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(programa, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  console_printf("SO: carga na memória física %d-%d\n", end_ini, end_fim);
  return end_ini;
}

static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo *processo)
{
  // t3: Implementação com paginação sob demanda
  // Carrega o programa inteiro na memória secundária (swap)
  // Não aloca nenhum quadro de memória principal
  // As páginas serão trazidas para memória principal por demanda (falta de página)
  
  if (processo == NULL) {
    console_printf("SO: processo inválido %d", processo);
    return -1;
  }
  
  int end_virt_ini = prog_end_carga(programa);
  
  // O código só funciona se o programa iniciar no início de uma página
  if ((end_virt_ini % TAM_PAGINA) != 0) {
    console_printf("SO: programa deve iniciar no início de uma página");
    return -1;
  }
  
  int end_virt_fim = end_virt_ini + prog_tamanho(programa) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int n_paginas = pagina_fim - pagina_ini + 1;
  
  console_printf("SO: carregando programa proc=%d end_virt=%d-%d n_pag=%d",
                 processo, end_virt_ini, end_virt_fim, n_paginas);
  
  // Aloca espaço na swap para todas as páginas do processo
  int swap_inicio = swap_aloca(self->swap, n_paginas, processo->pid);
  if (swap_inicio < 0) {
    console_printf("SO: erro ao alocar espaço na swap");
    return -1;
  }
  
  // Salva informações no processo
  processo->swap_inicio = swap_inicio;
  processo->n_paginas = n_paginas;
  
  // Carrega cada página do programa na swap
  for (int pag = 0; pag < n_paginas; pag++) {
    int dados[TAM_PAGINA];
    
    // Inicializa com zeros
    for (int i = 0; i < TAM_PAGINA; i++) {
      dados[i] = 0;
    }
    
    // Copia dados do programa para o buffer
    int end_virt_pag_ini = (pagina_ini + pag) * TAM_PAGINA;
    int end_virt_pag_fim = end_virt_pag_ini + TAM_PAGINA - 1;
    
    for (int end_virt = end_virt_pag_ini; end_virt <= end_virt_pag_fim; end_virt++) {
      if (end_virt >= end_virt_ini && end_virt <= end_virt_fim) {
        dados[end_virt - end_virt_pag_ini] = prog_dado(programa, end_virt);
      }
    }
    
    // Escreve a página na swap
    int end_swap = swap_inicio + pag;
    int tempo_bloqueio;
    
    if (swap_escreve_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio) != ERR_OK) {
      console_printf("SO: erro ao escrever página %d na swap", pag);
      return -1;
    }
    
    console_printf("SO: página %d carregada na swap[%d]", pag, end_swap);
  }
  
  // NÃO mapeia nenhuma página na tabela de páginas
  // As páginas serão mapeadas sob demanda quando houver falta de página
  console_printf("SO: programa carregado na swap, %d páginas, paginação sob demanda", n_paginas);
  
  return end_virt_ini;
}



// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do processo para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// O endereço é um endereço virtual de um processo.
// t3: Com memória virtual, cada valor do espaço de endereçamento do processo
//   pode estar em memória principal ou secundária (e tem que achar onde)
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo *processo)
{
  if (processo->estado == MORTO) return false;
  
  // Configura a MMU com a tabela de páginas do processo
  if (processo->pid != self->processo_corrente->pid) {
    // Salva tabela atual e configura a do processo desejado
    mmu_define_tabpag(self->mmu, processo->tabpag);
  }
  
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    // Lê da memória virtual do processo
    err_t err = mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario);
    
    // Se houver falta de página, trata
    if (err == ERR_PAG_AUSENTE) {
      int pagina = (end_virt + indice_str) / TAM_PAGINA;
      console_printf("SO: falta de página em copia_str (pag=%d)", pagina);
      so_trata_falta_pagina(self, processo, pagina);
      
      // Tenta ler novamente
      err = mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario);
    }
    
    if (err != ERR_OK) {
      // Restaura tabela de páginas anterior se necessário
      if (processo != self->processo_corrente) {
        mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
      }
      return false;
    }
    
    if (caractere < 0 || caractere > 255) {
      // Restaura tabela de páginas anterior se necessário
      if (processo != self->processo_corrente) {
        mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
      }
      return false;
    }
    
    str[indice_str] = caractere;
    if (caractere == 0) {
      // Restaura tabela de páginas anterior se necessário
      if (processo != self->processo_corrente) {
        mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
      }
      return true;
    }
  }
  
  // Restaura tabela de páginas anterior se necessário
  if (processo != self->processo_corrente) {
    mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
  }
  
  // estourou o tamanho de str
  return false;
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
