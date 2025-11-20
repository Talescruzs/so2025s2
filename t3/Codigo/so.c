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

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>


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
    float prioridade;
} processo;

// t3: a interface de algumas funções que manipulam memória teve que ser alterada,
//   para incluir o processo ao qual elas se referem. Para isso, é necessário um
//   tipo de dados para identificar um processo. Neste código, não tem processos
//   implementados, e não tem um tipo para isso. Foi usado o tipo int.
//   É necessário também um valor para representar um processo inexistente.
//   Foi usado o valor -1. Altere para o seu tipo, ou substitua os usos de
//   processo_t e NENHUM_PROCESSO para o seu tipo.
//   ALGUM_PROCESSO serve para representar um processo que não é NENHUM. Só tem
//   algum sentido enquanto não tem implementação de processos.
#define NENHUM_PROCESSO -1
#define ALGUM_PROCESSO 0

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO, regComplemento; // cópia do estado da CPU
  // t2: tabela de processos, processo corrente, pendências, etc

  int processo_corrente; // índice do processo corrente na tabela de processos

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

  // primeiro quadro da memória que está livre (quadros anteriores estão ocupados)
  // t3: com memória virtual, o controle de memória livre e ocupada deve ser mais
  //     completo que isso
  int quadro_livre;
  // uma tabela de páginas para poder usar a MMU
  // t3: com processos, não tem esta tabela global, tem que ter uma para
  //     cada processo
  tabpag_t *tabpag_global;
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
// no t3, foi adicionado o 'processo' aos argumentos dessas funções 
// carrega o programa contido no arquivo para memória virtual de um processo
// retorna o endereço virtual inicial de execução
static int so_carrega_programa(so_t *self, processo processo,
                               char *nome_do_executavel);
// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo processo);


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mmu_t *mmu,
              es_t *es, console_t *console)
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
  memset(self->tabela_processos, 0, sizeof(processo) * MAX_PROCESSOS);

  self->cpu = cpu;
  self->mem = mem;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // escalonador padrão: round-robin
  self->escalonador = so_escalona;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO

  // inicializa a tabela de páginas global, e entrega ela para a MMU
  // t3: com processos, essa tabela não existiria, teria uma por processo, que
  //     deve ser colocada na MMU quando o processo é despachado para execução
  self->tabpag_global = tabpag_cria();
  mmu_define_tabpag(self->mmu, self->tabpag_global);

  self->tabela_processos = malloc(MAX_PROCESSOS * sizeof(processo));
  if (self->tabela_processos == NULL) {
      free(self);
      return NULL;
  }
  for (int i = 0; i < MAX_PROCESSOS; i++) {
      self->tabela_processos[i].estado = MORTO; // inicializa todos os processos como mortos
  }
  self->processo_corrente = -1; // nenhum processo está executando

  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

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
  
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  if (self->escalonador) self->escalonador(self);
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
    for (int i = 0; i < MAX_PROCESSOS; i++){
      processo *proc = &self->tabela_processos[i];
      // ex: desbloq processo que espera dispositivo pronto
      if (proc->estado == BLOQUEADO){
        if (proc->esperando_dispositivo >= 0) {
          int disp = proc->esperando_dispositivo;
          int estado_disp = 0;
          if (es_le(self->es, disp, &estado_disp) == ERR_OK && estado_disp != 0) {
                    proc->estado = PRONTO;
                    proc->esperando_dispositivo = -1;
                    proc->quantum = 0;
                    insere_fila_prontos(self, i);  // i é o índice do processo desbloqueado

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
            insere_fila_prontos(self, i);  // i é o índice do processo desbloqueado

          }
        }
      }
    }
}


static void so_escalona(so_t *self)
{
    console_printf("escalonando (RR) %d", self->processo_corrente);

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

    // pega próximo da fila se houver
    int idx_fila = remove_fila_prontos(self);
    if (idx_fila != -1 && self->tabela_processos[idx_fila].estado == PRONTO) {
        proximo = idx_fila;
    } else {
        // fallback: busca sequencial
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
    console_printf("escalonando %d", self->processo_corrente);

    int atual = self->processo_corrente;
    int proximo = -1;
    int prontos = 0;

    // Contar processos PRONTOS
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == PRONTO) {
            prontos++;
        }
    }

    // Se processo atual está EXECUTANDO e há outro processo PRONTO,
    // recalcula prioridade do atual (quantum estourado ou bloqueio deve ser tratado externamente)
    if (atual >= 0 && atual < MAX_PROCESSOS &&
        self->tabela_processos[atual].estado == EXECUTANDO &&
        prontos > 0) {

        // Obtém t_exec e tempo de quantum (deve estar armazenado em algum lugar)
        int t_quantum = self->quantum;
        int t_exec = t_quantum - self->contador_quantum; // contador_quantum é decrementado a cada interrupção

        float prio_antiga = self->tabela_processos[atual].prioridade;
        float prio_nova = (prio_antiga + ((float)t_exec / t_quantum)) / 2.0f;

        self->tabela_processos[atual].prioridade = prio_nova;
        self->tabela_processos[atual].estado = PRONTO;

        // Insere o processo atual no fim da fila de prontos
        insere_fila_prontos(self, atual);
    }

    // Escolhe o próximo processo PRONTO com menor valor de prioridade (maior prioridade real)
    float menor_prio = 1000.0f; // valor grande inicial
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == PRONTO) {
            float prio = self->tabela_processos[i].prioridade;
            if (prio < menor_prio) {
                menor_prio = prio;
                proximo = i;
            }
        }
    }

    if (proximo != -1) {
        self->tabela_processos[proximo].estado = EXECUTANDO;
        self->processo_corrente = proximo;
        self->contador_quantum = self->quantum; // reseta contador do quantum para novo processo
    } else if (atual >= 0 && atual < MAX_PROCESSOS &&
               self->tabela_processos[atual].estado != BLOQUEADO &&
               self->tabela_processos[atual].estado != MORTO) {
        // Mantém processo atual EXECUTANDO se não há outro PRONTO
        self->tabela_processos[atual].estado = EXECUTANDO;
        self->processo_corrente = atual;
    } else {
        // Nenhum processo PRONTO ou EXECUTANDO disponível
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
// Se não há processo corrente (-1), mas existem processos PRONTOS,
// isso é um erro de escalonamento
if (self->processo_corrente < 0) {
    int tem_pronto = 0;
    console_printf("Aviso: processo_corrente é -1, verificando processos disponíveis...\n");
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado != MORTO) {
            console_printf("  Processo %d (PID %d): estado=%d PC=%d\n",
                         i,
                         self->tabela_processos[i].pid,
                         self->tabela_processos[i].estado,
                         self->tabela_processos[i].regPC);
            if (self->tabela_processos[i].estado == PRONTO) {
                tem_pronto = 1;
            }
        }
    }
    // Se tem processo PRONTO mas processo_corrente é -1, é um erro
    if (tem_pronto) {
        console_printf("Erro fatal: Há processos PRONTOS mas processo_corrente é -1\n");
        self->erro_interno = true;
        return 1;
    }
    // Se não tem nenhum processo PRONTO, retorna 1 para CPU aguardar
    console_printf("Nenhum processo disponível para executar, CPU aguardando...\n");
    return 1;
}
// Verifica se processo_corrente está dentro dos limites
if (self->processo_corrente >= MAX_PROCESSOS) {
    console_printf("Erro fatal: processo_corrente inválido (%d)\n", self->processo_corrente);
    self->erro_interno = true;
    return 1;
}

  /* if (self->processo_corrente < 0 || self->processo_corrente >= MAX_PROCESSOS) {
    console_printf("SO: processo_corrente inválido em despacha");
    self->erro_interno = true;
    return 1;
  } */
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
  int ender = so_carrega_programa(self, self->tabela_processos[0], "trata_int.maq");
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
  self->quadro_livre = CPU_END_FIM_PROT / TAM_PAGINA + 1;

  // t2: deveria criar um processo para o init, e inicializar o estado do
  //   processador para esse processo com os registradores zerados, exceto
  //   o PC e o modo.
  // como não tem suporte a processos, está carregando os valores dos
  //   registradores diretamente no estado da CPU mantido pelo SO; daí vai
  //   copiar para o início da memória pelo despachante, de onde a CPU vai
  //   carregar para os seus registradores quando executar a instrução RETI
  //   em bios.asm (que é onde está a instrução CHAMAC que causou a execução
  //   deste código
  ender = so_carrega_programa(self, self->tabela_processos[self->processo_corrente], "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial, ender = %d", ender);
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
  self->tabela_processos[0].quantum = self->quantum;  // inicializa com quantum completo
  self->tabela_processos[0].esperando_pid = -1;
  self->tabela_processos[0].esperando_dispositivo = -1;
  self->tabela_processos[0].memoria_base = 0;
  self->tabela_processos[0].memoria_limite = 0;
  self->tabela_processos[0].terminal = D_TERM_A_TELA;
  self->tabela_processos[0].prioridade = 0.0f;  // prioridade inicial máxima

  // coloca o programa init na memória
  
  ender = so_carrega_programa(self, *self->tabela_processos[self->processo_corrente].prox, "init.maq");
  if (ender == -1) {
    console_printf("SO: problema na carga do programa inicial na memoria virtual");
    self->erro_interno = true;
    return;
  }

  // altera o PC para o endereço de carga
  self->regPC = ender; // deveria ser no processo
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
  console_printf("SO: IRQ não tratada -- erro na CPU: %s (%d)",
                 err_nome(err), self->regComplemento);
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
  if (self->processo_corrente >= 0 && self->processo_corrente < MAX_PROCESSOS) {
    self->contador_quantum--;
    if (self->contador_quantum <= 0) {
      // Quantum esgotado, força troca de contexto
      console_printf("SO: quantum esgotado para processo %d", self->processo_corrente);
      self->tabela_processos[self->processo_corrente].estado = PRONTO;
      insere_fila_prontos(self, self->processo_corrente);
      self->processo_corrente = -1;  // força troca de processo
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
  if (!so_copia_str_do_processo(self, 100, nome, ender_proc,
                                self->tabela_processos[self->processo_corrente])) {
    self->tabela_processos[self->processo_corrente].regA = -1;
    return;
  }

  int ender_carga = so_carrega_programa(self, self->tabela_processos[self->processo_corrente], nome);
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
  self->tabela_processos[slot].quantum = self->quantum;  // inicializa com quantum completo
  self->tabela_processos[slot].esperando_pid = -1;
  self->tabela_processos[slot].esperando_dispositivo = -1;
  self->tabela_processos[slot].memoria_base = 0;
  self->tabela_processos[slot].memoria_limite = 0;
  self->tabela_processos[slot].prioridade = 0.0f;  // prioridade inicial máxima

  insere_fila_prontos(self, slot);

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

// funções auxiliares
static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo processo);

// carrega o programa na memória
// se processo for NENHUM_PROCESSO, carrega o programa na memória física
//   senão, carrega na memória virtual do processo
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, processo processo,
                               char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);

  programa_t *programa = prog_cria(nome_do_executavel);
  if (programa == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_carga;
  int flag = 0;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == MORTO) {
      end_carga = so_carrega_programa_na_memoria_fisica(self, programa);
      flag = 1;
      break;
    }
  }
  if (flag == 0) {
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

  console_printf("SO: carga na memória física %d-%d", end_ini, end_fim);
  return end_ini;
}

static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo processo)
{
  // t3: isto tá furado...
  // está simplesmente lendo para o próximo quadro que nunca foi ocupado,
  //   nem testa se tem memória disponível
  // com memória virtual, a forma mais simples de implementar a carga de um
  //   programa é carregá-lo para a memória secundária, e mapear todas as páginas
  //   da tabela de páginas do processo como inválidas. Assim, as páginas serão
  //   colocadas na memória principal por demanda. Para simplificar ainda mais, a
  //   memória secundária pode ser alocada da forma como a principal está sendo
  //   alocada aqui (sem reuso)
  int end_virt_ini = prog_end_carga(programa);
  // o código abaixo só funciona se o programa iniciar no início de uma página
  if ((end_virt_ini % TAM_PAGINA) != 0) return -1;
  int end_virt_fim = end_virt_ini + prog_tamanho(programa) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int n_paginas = pagina_fim - pagina_ini + 1;
  int quadro_ini = self->quadro_livre;
  int quadro_fim = quadro_ini + n_paginas - 1;
  // mapeia as páginas nos quadros
  for (int i = 0; i < n_paginas; i++) {
    tabpag_define_quadro(self->tabpag_global, pagina_ini + i, quadro_ini + i);
  }
  self->quadro_livre = quadro_fim + 1;

  // carrega o programa na memória principal
  int end_fis_ini = quadro_ini * TAM_PAGINA;
  int end_fis = end_fis_ini;
  for (int end_virt = end_virt_ini; end_virt <= end_virt_fim; end_virt++) {
    if (mem_escreve(self->mem, end_fis, prog_dado(programa, end_virt)) != ERR_OK) {
      console_printf("Erro na carga da memória, end virt %d fís %d\n", end_virt,
                     end_fis);
      return -1;
    }
    end_fis++;
  }
  console_printf("SO: carga na memória virtual V%d-%d F%d-%d npag=%d",
                 end_virt_ini, end_virt_fim, end_fis_ini, end_fis - 1, n_paginas);
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
                                     int end_virt, processo processo)
{
  if (self->tabela_processos[self->processo_corrente].estado == MORTO) return false;
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    // não tem memória virtual implementada, posso usar a mmu para traduzir
    //   os endereços e acessar a memória, porque todo o conteúdo do processo
    //   está na memória principal, e só temos uma tabela de páginas
    if (mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario) != ERR_OK) {
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
 
static int remove_fila_prontos(so_t *self) {
    if (self->inicio_fila == self->fim_fila) {
        return -1; // fila vazia
    }
    int idx = self->fila_prontos[self->inicio_fila];
    self->inicio_fila = (self->inicio_fila + 1) % MAX_PROCESSOS;
    return idx;
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

// vim: foldmethod=marker
