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
#define MEM_TAM 20000 // tamanho da memória principal


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
  mem_t *disco;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO, regComplemento; // cópia do estado da CPU

  processo *processo_corrente; 

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

  int proximo_end_livre_disco;


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
static int so_carrega_programa(so_t *self, char *nome_do_executavel);

static bool so_carrega_programa_na_swap(so_t *self, char *nome_do_executavel, processo *proc);

// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo* processo);


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mem_t *disco, mmu_t *mmu,
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
  self->disco = disco;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->relogio = relogio;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  self->metrica = cria_metrica();

  self->processo_corrente = NULL; // nenhum processo está executando

  self->quadro_livre_pri = 99 / TAM_PAGINA + 1;
  self->quadro_livre_sec = 0;

  self->quadros = mem_quadros_cria(MEM_TAM / TAM_PAGINA, 99 / TAM_PAGINA + 1, MEM_Q_TIPO);
  
  // Cria memória secundária (swap) - tamanho generoso para todos os processos
  self->swap = swap_cria(1000, TAM_PAGINA, relogio);

  self->proximo_end_livre_disco = 0;

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
  
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  
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
  int retorno = so_despacha(self);
  console_printf("depois despacha   ");
  
  // Debug final - USA OS VALORES DO PROCESSO CORRENTE, NÃO DA MEMÓRIA
  if (self->processo_corrente != NULL) {
    console_printf("RETORNO DESPACHA: %d, proc=%d regA=%d regPC=%d regERRO=%d regX=%d(%c)",
                   retorno,
                   self->processo_corrente->pid,
                   self->processo_corrente->regA,
                   self->processo_corrente->regPC,
                   self->processo_corrente->regERRO,
                   self->processo_corrente->regX,
                   self->processo_corrente->regX);
  } else {
    console_printf("RETORNO DESPACHA: %d (sem processo corrente)", retorno);
  }
  
  return retorno;
}

static void so_chamada_mata_proc(so_t *self);


static void so_salva_estado_da_cpu(so_t *self)
{
  // Se não houver processo corrente, não faz nada
  if (self->tabela_processos == NULL || self->processo_corrente == NULL) {
    console_printf("nenhum processo corrente, nada a salvar   ");
    return;
  }
  
  // IMPORTANTE: Lê da MEMÓRIA FÍSICA onde a CPU salvou os registradores
  int a, pc, erro, x;
  if (mem_le(self->mem, CPU_END_A, &a) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &pc) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &erro) != ERR_OK
      || mem_le(self->mem, 59, &x) != ERR_OK) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
    return;
  }

  // SALVA no descritor do processo
  self->processo_corrente->regA = a;
  self->processo_corrente->regPC = pc;
  self->processo_corrente->regERRO = erro;
  self->processo_corrente->regX = x;
  
  console_printf("SO: salvou estado proc=%d A=%d PC=%d X=%d erro=%d",
                 self->processo_corrente->pid, a, pc, x, erro);
}

static void so_trata_pendencias(so_t *self)
{

  
  console_printf("tratando pendências   ");
  if (self->tabela_processos == NULL) return;
  
  verifica_ocioso(self->metrica, self->tabela_processos, self->es);
  
  processo *proc = self->tabela_processos;
  while (proc != NULL)
  {
    if (proc->esperando_dispositivo >= 0){
      trata_bloqueio_disp(proc, self->metrica, self->es, &self->erro_interno);
    }
    proc = proc->prox;
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

static void so_escalona(so_t *self) {
  if (self->processo_corrente == NULL)
  {
    console_printf("escalonando sem processo corrente");
  }
  else {
    console_printf("escalonando %d", self->processo_corrente->pid);
  }
    

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
                     self->processo_corrente->pid,
                     self->processo_corrente->regA,
                     self->processo_corrente->regPC,
                     self->processo_corrente->regERRO,
                     self->processo_corrente->regX);
    }
}

static int so_despacha(so_t *self)
{
  // Verifica se há processo corrente válido
  if (self->processo_corrente == NULL) {
    console_printf("SO: sem processo para despachar");
    return 1;
  }
  
  
  if (self->processo_corrente->estado == MORTO) {
    console_printf("SO: processo %d está morto", self->processo_corrente->pid);
    return 1;
  }
  
  // DIAGNÓSTICO: Antes de configurar MMU
  console_printf("SO: despachando proc=%d PC=%d (virtual)", 
                 self->processo_corrente->pid, self->processo_corrente->regPC);
  
  // Verifica tabela de páginas
  if (self->processo_corrente->tabpag == NULL) {
    console_printf("SO: ERRO - tabela de páginas NULL!");
    self->erro_interno = true;
    return 1;
  }
  
  // Configura MMU com tabela de páginas do processo
  mmu_define_tabpag(self->mmu, self->processo_corrente->tabpag);
  
  console_printf("SO: MMU configurada com tabpag do processo %d", 
                 self->processo_corrente->pid);
// TESTE: Verifica tradução do PC
  int pagina_pc = self->processo_corrente->regPC / TAM_PAGINA;
  int quadro_pc;
  int end_fis;
  err_t err_traduz = tabpag_traduz(self->processo_corrente->tabpag, pagina_pc, &quadro_pc);
  
  if (err_traduz == ERR_OK) {
    end_fis = quadro_pc * TAM_PAGINA + (self->processo_corrente->regPC % TAM_PAGINA);
    int valor_fis;
    mem_le(self->mem, end_fis, &valor_fis);
    console_printf("Teste tradução: PC=%d página=%d quadro=%d end_fis=%d valor=%d",
                    self->processo_corrente->regPC, pagina_pc, quadro_pc, end_fis, valor_fis);
  } else {
    console_printf("Teste tradução: FALHOU - PC=%d página=%d err=%d (FALTA DE PÁGINA!)",
                    self->processo_corrente->regPC, pagina_pc, err_traduz);
  }
  
  // Escreve na MEMÓRIA FÍSICA de onde a CPU vai ler
  if (mem_escreve(self->mem, CPU_END_A, self->processo_corrente->regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->processo_corrente->regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->processo_corrente->regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->processo_corrente->regX) != ERR_OK) {
    console_printf("SO: erro na escrita dos registradores");
    self->erro_interno = true;
    return 1;
  }
  
  console_printf("SO: despachou proc=%d PC=%d A=%d X=%d erro=%d",
                 self->processo_corrente->pid,
                 self->processo_corrente->regPC,
                 self->processo_corrente->regA,
                 self->processo_corrente->regX,
                 self->processo_corrente->regERRO);
  
  return (self->erro_interno) ? 1 : 0;
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

static void diagnostico_memoria_virtual(so_t *self, processo *proc, const char *contexto);

static int so_aloca_quadro(so_t *self); 

// // chamada uma única vez, quando a CPU inicializa
// static void so_trata_reset(so_t *self)
// {
//   console_printf("recebi RESET   ");
  
//   // coloca o tratador de interrupção na memória
//   // quando a CPU aceita uma interrupção, passa para modo supervisor,
//   //   salva seu estado à partir do endereço CPU_END_PC, e desvia para o
//   //   endereço CPU_END_TRATADOR
//   // colocamos no endereço CPU_END_TRATADOR o programa de tratamento
//   //   de interrupção (escrito em asm). esse programa deve conter a
//   //   instrução CHAMAC, que vai chamar so_trata_interrupcao (como
//   //   foi definido na inicialização do SO)
//   int ender_trata = so_carrega_programa(self, "trata_int.maq");
//   if (ender_trata != CPU_END_TRATADOR) {
//     console_printf("SO: problema na carga do programa de tratamento de interrupção");
//     self->erro_interno = true;
//     return;
//   }
  
//   // Programa o relógio
//   if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
//     console_printf("SO: problema na programação do timer");
//     self->erro_interno = true;
//     return;
//   }

//   // Cria processo init
//   processo *p_init = processo_cria(1, -1, 0, self->quantum);
//   if (p_init == NULL) {
//     console_printf("SO: erro ao criar processo init");
//     self->erro_interno = true;
//     return;
//   }
  
//   // Carrega o programa init NA SWAP
//   if (!so_carrega_programa_na_swap(self, "init.maq", p_init)) {
//     console_printf("SO: erro ao carregar init na swap");
//     self->erro_interno = true;
//     free(p_init);
//     return;
//   }
  
//   // O PC inicial é sempre 0 (início do endereço virtual)
//   // O programa foi carregado começando em end_virt_ini, mas o processo
//   // vê tudo começando do 0
//   p_init->regPC = 0;
//   p_init->regA = 0;
//   p_init->regX = 0;
//   p_init->regERRO = ERR_OK;
  
//   console_printf("SO: init criado PC=%d A=%d X=%d erro=%d", 
//                  p_init->regPC, p_init->regA, p_init->regX, p_init->regERRO);
  
//   // Carrega a PRIMEIRA PÁGINA (página 0) do init na memória física
//   // para que ele possa começar a executar (paginação sob demanda)
//   console_printf("SO: carregando página inicial 0 do init na memória física");
  
//   int quadro = so_aloca_quadro(self);
//   if (quadro < 0) {
//     console_printf("SO: erro ao alocar quadro para página inicial");
//     self->erro_interno = true;
//     free(p_init);
//     return;
//   }
  
//   console_printf("SO: quadro %d alocado (livre)", quadro);
  
//   // Lê a página 0 da swap
//   int end_swap = swap_endereco_pagina(self->swap, p_init->pid, 0);
//   int dados[TAM_PAGINA];
//   int tempo_bloqueio;
  
//   err_t err = swap_le_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio);
//   if (err != ERR_OK) {
//     console_printf("SO: erro ao ler página inicial da swap");
//     self->erro_interno = true;
//     free(p_init);
//     return;
//   }
  
//   console_printf("SO: lido da swap - primeira instrução: %d (esperado: 2 para NOP ou CARGI)", dados[0]);
  
//   // Escreve na memória física
//   int end_fis = quadro * TAM_PAGINA;
//   for (int i = 0; i < TAM_PAGINA; i++) {
//     mem_escreve(self->mem, end_fis + i, dados[i]);
//   }
  
//   console_printf("SO: página escrita na memória física quadro=%d end_fis=%d", quadro, end_fis);
  
//   // Mapeia página 0 no quadro alocado
//   int pagina_inicial = 0;
//   tabpag_define_quadro(p_init->tabpag, pagina_inicial, quadro);
  
//   // Verifica mapeamento
//   int quadro_verificado;
//   err_t err_verif = tabpag_traduz(p_init->tabpag, pagina_inicial, &quadro_verificado);
  
//   console_printf("SO: verificação mapeamento - página=%d quadro_esperado=%d quadro_obtido=%d err=%d", 
//                  pagina_inicial, quadro, quadro_verificado, err_verif);
  
//   if (err_verif != ERR_OK || quadro_verificado != quadro) {
//     console_printf("SO: ERRO - mapeamento falhou!");
//     self->erro_interno = true;
//     free(p_init);
//     return;
//   }
  
//   // Registra quadro como ocupado
//   mem_quadros_muda_estado(self->quadros, quadro, false, p_init->pid, pagina_inicial);
  
//   console_printf("SO: página inicial mapeada com sucesso");
  
//   // Teste de leitura direta
//   int teste_fis;
//   mem_le(self->mem, end_fis, &teste_fis);
//   console_printf("SO: teste leitura física end=%d valor=%d", end_fis, teste_fis);
  
//   // CRUCIAL: Configura MMU com a tabela de páginas do processo
//   mmu_define_tabpag(self->mmu, p_init->tabpag);
//   console_printf("SO: MMU configurada com tabpag do processo %d", p_init->pid);
  
//   // Testa leitura via MMU
//   int valor_virt;
//   err_t err_mmu = mmu_le(self->mmu, 0, &valor_virt, supervisor);
//   console_printf("SO: teste leitura MMU end_virt=0 valor=%d err=%d (esperado: 2)", 
//                  valor_virt, err_mmu);
  
//   if (err_mmu != ERR_OK || valor_virt != 2) {
//     console_printf("SO: ERRO - MMU não está funcionando corretamente!");
//     self->erro_interno = true;
//     free(p_init);
//     return;
//   }

//   self->processo_corrente = p_init;
//   insere_novo_processo(&self->tabela_processos, p_init);
  
//   // Diagnóstico final
//   diagnostico_memoria_virtual(self, p_init, "APÓS CARGA E MAPEAMENTO INIT");
// }

// ---------------------------------------------------------------------
// TRATAMENTO DE IRQ {{{1
// ---------------------------------------------------------------------

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


// Trata uma falta de página
static void so_trata_falta_pagina(so_t *self, processo* proc, int pagina)
{
  console_printf("\n========== TRATANDO FALTA DE PÁGINA ==========");
  console_printf("SO: falta de página %d do processo %d", pagina, proc->pid);
  
  // VERIFICA SE A PÁGINA JÁ ESTÁ MAPEADA (pode acontecer se tratamos duas vezes)
  int quadro_teste;
  if (tabpag_traduz(proc->tabpag, pagina, &quadro_teste) == ERR_OK) {
    console_printf("SO: página %d JÁ ESTÁ MAPEADA no quadro %d - ignora falta", 
                   pagina, quadro_teste);
    
    // Apenas reseta o erro e retorna
    proc->regERRO = ERR_OK;
    return;
  }
  
  // Verifica se a página é válida para o processo
  if (pagina < 0 || pagina >= proc->n_paginas) {
    console_printf("SO: ERRO - página %d inválida (processo tem %d páginas)", 
                   pagina, proc->n_paginas);
    proc->estado = MORTO;
    return;
  }
  
  // Aloca um quadro (pode fazer substituição)
  int quadro = so_aloca_quadro(self);
  if (quadro < 0) {
    console_printf("SO: ERRO ao alocar quadro");
    proc->estado = MORTO;
    return;
  }
  
  console_printf("SO: quadro %d alocado para página %d", quadro, pagina);
  
  // Obtém endereço da página na swap
  int end_swap = swap_endereco_pagina(self->swap, proc->pid, pagina);
  
  if (end_swap < 0) {
    console_printf("SO: ERRO ao obter endereço da página na swap");
    proc->estado = MORTO;
    return;
  }
  
  // Lê a página da swap
  int dados[TAM_PAGINA];
  int tempo_bloqueio;
  err_t err = swap_le_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio);
  
  if (err != ERR_OK) {
    console_printf("SO: ERRO ao ler página da swap");
    proc->estado = MORTO;
    return;
  }
  
  console_printf("SO: dados lidos: %d %d %d %d", dados[0], dados[1], dados[2], dados[3]);
  
  // Escreve os dados no quadro da memória principal
  int end_fis = quadro * TAM_PAGINA;
  
  for (int i = 0; i < TAM_PAGINA; i++) {
    err_t err_mem = mem_escreve(self->mem, end_fis + i, dados[i]);
    if (err_mem != ERR_OK) {
      console_printf("SO: ERRO ao escrever na memória offset=%d err=%d", i, err_mem);
      proc->estado = MORTO;
      return;
    }
  }
  
  // CRUCIAL: Mapeia na tabela de páginas ANTES de marcar quadro como ocupado
  tabpag_define_quadro(proc->tabpag, pagina, quadro);
  
  // Verifica se foi realmente mapeado
  int quadro_mapeado;
  err_t err_verif = tabpag_traduz(proc->tabpag, pagina, &quadro_mapeado);
  
  if (err_verif != ERR_OK || quadro_mapeado != quadro) {
    console_printf("SO: ERRO - mapeamento falhou! err=%d quadro_esperado=%d quadro_obtido=%d",
                   err_verif, quadro, quadro_mapeado);
    proc->estado = MORTO;
    return;

    muda_estado_proc(proc, self->metrica, self->es, BLOQUEADO);
  }
  
  // Agora sim registra o quadro como ocupado
  mem_quadros_muda_estado(self->quadros, quadro, false, proc->pid, pagina);
  
  console_printf("SO: página %d mapeada no quadro %d", pagina, quadro);
  
  // CRUCIAL: Reseta o erro para que a CPU possa continuar
  proc->regERRO = ERR_OK;
  
  // Incrementa contador de faltas de página
  proc->n_faltas_pagina++;
  
  console_printf("========== FIM TRATAMENTO FALTA ==========\n");
}


static void so_trata_irq_err_cpu(so_t *self)
{
  if (self->processo_corrente == NULL) {
    console_printf("SO: erro de CPU sem processo corrente");
    return;
  }

  processo *proc = self->processo_corrente;
  err_t err = proc->regERRO;
  mem_le(self->mem, CPU_END_complemento, &self->regComplemento);
  
  console_printf("SO: IRQ de ERRO na CPU: %s (complemento=%d) proc_idx=%d pid=%d pc=%d",
                 err_nome(err), self->regComplemento, 
                 /* índice do processo */ proc->pid, proc->regPC);
  
  if (err == ERR_PAG_AUSENTE) {
    int end_virt = self->regComplemento;
    int pagina = end_virt / TAM_PAGINA;
    
    console_printf("SO: falta de página %d do processo %d", pagina, proc->pid);
    
    // Verifica se o endereço é válido para o processo
    if (pagina < 0 || pagina >= proc->n_paginas) {
      console_printf("SO: acesso inválido à página %d (processo tem %d páginas)",
                     pagina, proc->n_paginas);
      proc->estado = MORTO;
      return;
    }
    
    // Trata a falta de página
    so_trata_falta_pagina(self, proc, pagina);
    
    // NÃO incrementa PC - a CPU vai reexecutar a mesma instrução
    // mas agora a página estará mapeada!
    
    return;
  }
  
}


// Aloca um quadro livre ou libera um ocupado usando substituição de páginas
static int so_aloca_quadro(so_t *self)
{
  // Procura por um quadro livre
  int quadro = mem_quadros_tem_livre(self->quadros);
  
  if (quadro >= 0) {
    console_printf("SO: quadro %d alocado (livre)", quadro);
    
    // IMPORTANTE: Marca o quadro como OCUPADO imediatamente
    // para que não seja alocado novamente antes de ser usado
    mem_quadros_muda_estado(self->quadros, quadro, false, -1, -1);
    
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
    console_printf("SO: bloqueando processo %d na escrita do dispositivo %d\n", self->processo_corrente->pid, terminal_tela);
    console_printf("esperando disp %d estado %d", self->processo_corrente->esperando_dispositivo, self->processo_corrente->estado);
  }
  
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
      console_printf("chamada de mata proc   ");
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
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// Carrega o programa na memória virtual (swap)
// Retorna o endereço virtual inicial ou -1 se erro
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);
  
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("SO: erro na leitura do programa '%s'", nome_do_executavel);
    return -1;
  }

  int end_virt_ini = prog_end_carga(prog);
  int tamanho = prog_tamanho(prog);
  int end_virt_fim = end_virt_ini + tamanho;
  
  console_printf("SO: programa '%s' end_virt=%d-%d tamanho=%d", 
                 nome_do_executavel, end_virt_ini, end_virt_fim, tamanho);

  // Se for trata_int.maq, carrega DIRETO na memória física (não usa paginação)
  if (strcmp(nome_do_executavel, "trata_int.maq") == 0) {
    for (int end = end_virt_ini; end < end_virt_fim; end++) {
      if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
        console_printf("SO: erro na carga do programa de interrupção");
        prog_destroi(prog);
        return -1;
      }
    }
    prog_destroi(prog);
    console_printf("SO: carga física de '%s' em %d-%d", 
                   nome_do_executavel, end_virt_ini, end_virt_fim);
    return end_virt_ini;
  }

  // Para programas de usuário, carrega na SWAP (memória virtual)
  // Calcula número de páginas necessárias
  int n_paginas = (tamanho + TAM_PAGINA - 1) / TAM_PAGINA;
  
  // NOTA: NÃO aloca na swap aqui! O processo é quem deve fazer isso
  // quando for criado. Aqui só retornamos o endereço de carga.
  
  prog_destroi(prog);
  console_printf("SO: programa '%s' precisa de %d páginas", nome_do_executavel, n_paginas);
  
  return end_virt_ini;
}

// Carrega o programa do arquivo diretamente na swap de um processo
static bool so_carrega_programa_na_swap(so_t *self, char *nome_do_executavel, processo *proc)
{
  if (proc == NULL) {
    console_printf("SO: processo NULL em carrega_programa_na_swap");
    return false;
  }

  console_printf("SO: carregando programa '%s' proc=%d end_virt=%d-%d n_pag=%d", 
                 nome_do_executavel, proc->pid, 
                 proc->swap_inicio * TAM_PAGINA,
                 (proc->swap_inicio + proc->n_paginas) * TAM_PAGINA,
                 proc->n_paginas);
  
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("SO: erro na leitura do programa '%s'", nome_do_executavel);
    return false;
  }

  int end_virt_ini = prog_end_carga(prog);
  int tamanho = prog_tamanho(prog);
  int end_virt_fim = end_virt_ini + tamanho;
  
  // Aloca espaço na swap para o processo
  int n_paginas = (tamanho + TAM_PAGINA - 1) / TAM_PAGINA;
  
  int swap_inicio = swap_aloca(self->swap, n_paginas, proc->pid);
  if (swap_inicio < 0) {
    console_printf("SO: erro ao alocar swap para processo %d", proc->pid);
    prog_destroi(prog);
    return false;
  }
  
  proc->swap_inicio = swap_inicio;
  proc->n_paginas = n_paginas;
  
  console_printf("SO: alocado swap[%d..%d] para proc=%d (%d páginas)", 
                 swap_inicio, swap_inicio + n_paginas - 1, proc->pid, n_paginas);
  
  // Carrega cada página do programa na swap
  for (int pag = 0; pag < n_paginas; pag++) {
    int dados_pagina[TAM_PAGINA];
    memset(dados_pagina, 0, sizeof(dados_pagina));
    
    // Preenche a página com os dados do programa
    int offset_inicio = pag * TAM_PAGINA;
    int offset_fim = offset_inicio + TAM_PAGINA;
    
    for (int offset = offset_inicio; offset < offset_fim; offset++) {
      int end_prog = end_virt_ini + offset;
      
      if (end_prog < end_virt_fim) {
        // Ainda tem dados do programa
        dados_pagina[offset - offset_inicio] = prog_dado(prog, end_prog);
      } else {
        // Já passou do fim do programa, preenche com zero
        dados_pagina[offset - offset_inicio] = 0;
      }
    }
    
    // Escreve a página na swap
    int tempo_bloqueio;
    err_t err = swap_escreve_pagina(self->swap, swap_inicio + pag, 
                                     dados_pagina, TAM_PAGINA, &tempo_bloqueio);
    
    if (err != ERR_OK) {
      console_printf("SO: erro ao escrever página %d na swap", pag);
      prog_destroi(prog);
      return false;
    }
    
    if ((pag + 1) % 10 == 0) {
      console_printf("SO: página %d/%d carregada na swap[%d]", 
                     pag + 1, n_paginas, swap_inicio + pag);
    }
  }
  
  console_printf("SO: página %d/%d carregada na swap[%d]", 
                 n_paginas, n_paginas, swap_inicio + n_paginas - 1);
  
  prog_destroi(prog);
  console_printf("SO: programa carregado na swap, %d páginas, paginação sob demanda", n_paginas);
  
  return true;
}

// ---------------------------------------------------------------------
// TRATAMENTO DE RESET
// ---------------------------------------------------------------------



static void so_trata_reset(so_t *self)
{
  console_printf("recebi RESET   ");
  
  // Carrega tratador de interrupção DIRETO na memória física
  int ender_trata = so_carrega_programa(self, "trata_int.maq");
  if (ender_trata != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
    return;
  }
  
  // Programa o relógio
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
    return;
  }

  // Cria processo init
  processo *p_init = processo_cria(1, -1, 0, self->quantum);
  if (p_init == NULL) {
    console_printf("SO: erro ao criar processo init");
    self->erro_interno = true;
    return;
  }
  
  // Carrega o programa init NA SWAP
  if (!so_carrega_programa_na_swap(self, "init.maq", p_init)) {
    console_printf("SO: erro ao carregar init na swap");
    self->erro_interno = true;
    free(p_init);
    return;
  }
  
  // O PC inicial é sempre 0 (início do endereço virtual)
  // O programa foi carregado começando em end_virt_ini, mas o processo
  // vê tudo começando do 0
  p_init->regPC = 0;
  p_init->regA = 0;
  p_init->regX = 0;
  p_init->regERRO = ERR_OK;
  
  console_printf("SO: init criado PC=%d A=%d X=%d erro=%d", 
                 p_init->regPC, p_init->regA, p_init->regX, p_init->regERRO);
  
  // Carrega a PRIMEIRA PÁGINA (página 0) do init na memória física
  // para que ele possa começar a executar (paginação sob demanda)
  console_printf("SO: carregando página inicial 0 do init na memória física");
  
  int quadro = so_aloca_quadro(self);
  if (quadro < 0) {
    console_printf("SO: erro ao alocar quadro para página inicial");
    self->erro_interno = true;
    free(p_init);
    return;
  }
  
  console_printf("SO: quadro %d alocado (livre)", quadro);
  
  // Lê a página 0 da swap
  int end_swap = swap_endereco_pagina(self->swap, p_init->pid, 0);
  int dados[TAM_PAGINA];
  int tempo_bloqueio;
  
  err_t err = swap_le_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo_bloqueio);
  if (err != ERR_OK) {
    console_printf("SO: erro ao ler página inicial da swap");
    self->erro_interno = true;
    free(p_init);
    return;
  }
  
  console_printf("SO: lido da swap - primeira instrução: %d (esperado: 2 para NOP ou CARGI)", dados[0]);
  
  // Escreve na memória física
  int end_fis = quadro * TAM_PAGINA;
  for (int i = 0; i < TAM_PAGINA; i++) {
    mem_escreve(self->mem, end_fis + i, dados[i]);
  }
  
  console_printf("SO: página escrita na memória física quadro=%d end_fis=%d", quadro, end_fis);
  
  // Mapeia página 0 no quadro alocado
  int pagina_inicial = 0;
  tabpag_define_quadro(p_init->tabpag, pagina_inicial, quadro);
  
  // Verifica mapeamento
  int quadro_verificado;
  err_t err_verif = tabpag_traduz(p_init->tabpag, pagina_inicial, &quadro_verificado);
  
  console_printf("SO: verificação mapeamento - página=%d quadro_esperado=%d quadro_obtido=%d err=%d", 
                 pagina_inicial, quadro, quadro_verificado, err_verif);
  
  if (err_verif != ERR_OK || quadro_verificado != quadro) {
    console_printf("SO: ERRO - mapeamento falhou!");
    self->erro_interno = true;
    free(p_init);
    return;
  }
  
  // Registra quadro como ocupado
  mem_quadros_muda_estado(self->quadros, quadro, false, p_init->pid, pagina_inicial);
  
  console_printf("SO: página inicial mapeada com sucesso");
  
  // Teste de leitura direta
  int teste_fis;
  mem_le(self->mem, end_fis, &teste_fis);
  console_printf("SO: teste leitura física end=%d valor=%d", end_fis, teste_fis);
  
  // CRUCIAL: Configura MMU com a tabela de páginas do processo
  mmu_define_tabpag(self->mmu, p_init->tabpag);
  console_printf("SO: MMU configurada com tabpag do processo %d", p_init->pid);
  
  // Testa leitura via MMU
  int valor_virt;
  err_t err_mmu = mmu_le(self->mmu, 0, &valor_virt, supervisor);
  console_printf("SO: teste leitura MMU end_virt=0 valor=%d err=%d (esperado: 2)", 
                 valor_virt, err_mmu);
  
  if (err_mmu != ERR_OK || valor_virt != 2) {
    console_printf("SO: ERRO - MMU não está funcionando corretamente!");
    self->erro_interno = true;
    free(p_init);
    return;
  }

  self->processo_corrente = p_init;
  insere_novo_processo(&self->tabela_processos, p_init);
}

// ---------------------------------------------------------------------
// CHAMADA DE SISTEMA: CRIA PROCESSO
static void so_chamada_cria_proc(so_t *self)
{
  console_printf("chamada de criação de processo   ");
  
  if (self->processo_corrente == NULL) {
    console_printf("SO: processo_corrente NULL em cria_proc");
    return;
  }
  
  int ender_proc = self->processo_corrente->regX;
  char nome[100];
  
  if (!so_copia_str_do_processo(self, 100, nome, ender_proc, self->processo_corrente)) {
    console_printf("SO: erro ao copiar nome do programa");
    self->processo_corrente->regA = -1;
    return;
  }
  
  // Cria o processo
  static int proximo_pid = 2; // init é 1
  processo *novo_proc = processo_cria(proximo_pid++, 
                                       self->processo_corrente->pid, 
                                       0, 
                                       self->quantum);
  
  if (novo_proc == NULL) {
    console_printf("SO: erro ao criar processo");
    self->processo_corrente->regA = -1;
    return;
  }
  
  // Carrega o programa NA SWAP
  if (!so_carrega_programa_na_swap(self, nome, novo_proc)) {
    console_printf("SO: erro ao carregar programa '%s' na swap", nome);
    self->processo_corrente->regA = -1;
    free(novo_proc);
    return;
  }
  
  // Inicializa registradores
  novo_proc->regPC = 0;  // começa do endereço virtual 0
  novo_proc->regA = 0;
  novo_proc->regX = 0;
  novo_proc->regERRO = ERR_OK;
  novo_proc->estado = PRONTO;
  
  // Insere na tabela de processos
  insere_novo_processo(&self->tabela_processos, novo_proc);
  
  // Retorna PID do filho
  self->processo_corrente->regA = novo_proc->pid;
  
  console_printf("SO: processo %d criado com sucesso (filho de %d)", 
                 novo_proc->pid, self->processo_corrente->pid);

  diagnostico_memoria_virtual(self, novo_proc, "após criação do processo");
}

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


// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  int pid_alvo = self->processo_corrente->regX;
  if(pid_alvo == 0) {
    pid_alvo = self->processo_corrente->pid;
  }
  console_printf("SO: chamada de mata processo %d \n", pid_alvo);
  processo *alvo = encontra_processo_por_pid(self->tabela_processos, pid_alvo);
  
  if (alvo == NULL || alvo->estado == MORTO) {
    self->processo_corrente->regA = -1;
    return;
  }
  
  int tempo_inicio = 0;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_inicio);
  self->metrica->tempo_retorno[alvo->pid-1] = tempo_inicio - self->metrica->tempo_retorno[alvo->pid-1];

  console_printf("SO: matando processo %d", alvo->pid);
  muda_estado_proc(alvo, self->metrica, self->es, MORTO);
  
  // CRUCIAL: Se matou a si mesmo, anula processo_corrente
  if (alvo == self->processo_corrente) {
    console_printf("SO: processo %d se matou, anulando processo_corrente", alvo->pid);
    self->processo_corrente = NULL;
  }
  
  // Desbloqueia processo pai se estava esperando
  processo *pai = encontra_processo_por_pid(self->tabela_processos, alvo->ppid);
  if (pai != NULL) {
    // Verifica se o pai estava esperando este filho
    for (int i = 0; i < pai->indice_esperando_pid; i++) {
      if (pai->esperando_pid[i] == alvo->pid) {
        pai->esperando_pid[i] = -1; // não espera mais esse filho
        
        // Se o pai estava bloqueado esperando, desbloqueia
        if (pai->estado == BLOQUEADO) {
          bool ainda_esperando = false;
          for (int j = 0; j < pai->indice_esperando_pid; j++) {
            if (pai->esperando_pid[j] > 0) {
              ainda_esperando = true;
              break;
            }
          }
          
          if (!ainda_esperando) {
            console_printf("SO: desbloqueando pai %d", pai->pid);
            muda_estado_proc(pai, self->metrica, self->es, PRONTO);
          }
        }
        break;
      }
    }
  }
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  int pid_esperado = self->processo_corrente->regX;
  processo *alvo = encontra_processo_por_pid(self->tabela_processos, pid_esperado);
  
  console_printf("SO: chamada de espera de processo %d \n", pid_esperado);
  
  if (alvo == NULL || alvo->estado == MORTO) {
    console_printf("SO: processo esperado já terminou ou não existe %d \n", pid_esperado);
    self->processo_corrente->regA = -1;
    return;
  }
  
  // Bloqueia o processo corrente
  muda_estado_proc(self->processo_corrente, self->metrica, self->es, BLOQUEADO);
  
  // Registra métricas
  self->metrica->n_entradas_estado[self->processo_corrente->pid-1][BLOQUEADO]++;
  es_le(self->es, D_RELOGIO_INSTRUCOES, &self->metrica->tempo_estado[self->processo_corrente->pid-1][BLOQUEADO]);
  
  // Marca que está esperando o processo
  self->processo_corrente->esperando_pid[self->processo_corrente->indice_esperando_pid] = pid_esperado;
  self->processo_corrente->indice_esperando_pid++;
  
  // CRUCIAL: Anula processo_corrente para forçar escalonamento
  self->processo_corrente = NULL;
  
  console_printf("SO: processo %d bloqueado esperando processo %d \n", 
                 self->tabela_processos ? self->tabela_processos->pid : -1, pid_esperado);
}

// Função de diagnóstico para verificar a configuração de memória virtual
static void diagnostico_memoria_virtual(so_t *self, processo *proc, const char *contexto)
{
  console_printf("\n========== DIAGNÓSTICO: %s ==========", contexto);
  console_printf("Processo PID=%d PC=%d", proc->pid, proc->regPC);
  console_printf("Swap: inicio=%d n_paginas=%d", proc->swap_inicio, proc->n_paginas);
  
  // Verifica tabela de páginas
  if (proc->tabpag == NULL) {
    console_printf("ERRO: tabela de páginas é NULL!");
    return;
  }
  
  // Tenta acessar a primeira página (onde está o PC)
  int pagina_pc = proc->regPC / TAM_PAGINA;
  console_printf("PC=%d está na página %d", proc->regPC, pagina_pc);
  
  // Usa tabpag_traduz para verificar se a página está mapeada
  int quadro_pc;
  err_t err_traduz = tabpag_traduz(proc->tabpag, pagina_pc, &quadro_pc);
  
  if (err_traduz == ERR_OK) {
    console_printf("Página %d VÁLIDA - mapeada no quadro %d", pagina_pc, quadro_pc);
    
    // Lê diretamente da memória física usando o quadro
    int end_fis = quadro_pc * TAM_PAGINA + (proc->regPC % TAM_PAGINA);
    int valor_fis;
    err_t err_mem = mem_le(self->mem, end_fis, &valor_fis);
    console_printf("Leitura física: end=%d valor=%d err=%d", end_fis, valor_fis, err_mem);
    
    // Mostra primeiros valores do quadro
    int val0, val1, val2, val3;
    mem_le(self->mem, quadro_pc * TAM_PAGINA + 0, &val0);
    mem_le(self->mem, quadro_pc * TAM_PAGINA + 1, &val1);
    mem_le(self->mem, quadro_pc * TAM_PAGINA + 2, &val2);
    mem_le(self->mem, quadro_pc * TAM_PAGINA + 3, &val3);
    console_printf("Primeiros valores no quadro físico: %d %d %d %d", val0, val1, val2, val3);
  } else {
    console_printf("Página %d AUSENTE (err=%d) - está apenas na swap", pagina_pc, err_traduz);
  }
  
  // Verifica swap
  int end_swap = swap_endereco_pagina(self->swap, proc->pid, pagina_pc);
  console_printf("Endereço na swap: %d", end_swap);
  
  if (end_swap >= 0) {
    int dados[TAM_PAGINA];
    int tempo;
    err_t err = swap_le_pagina(self->swap, end_swap, dados, TAM_PAGINA, &tempo);
    console_printf("Leitura swap: err=%d primeiro_valor=%d", err, dados[0]);
    
    // Mostra primeiros valores
    console_printf("Primeiros valores na swap: %d %d %d %d", 
                   dados[0], dados[1], dados[2], dados[3]);
  }
  
  console_printf("========== FIM DIAGNÓSTICO ==========\n");
}
// vim: foldmethod=marker
