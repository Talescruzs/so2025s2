#ifndef CONFIG_H
#define CONFIG_H

#include "console.h"

typedef struct processo processo;

typedef struct metricas metricas;

enum EstadoProcesso {
    PRONTO,      /* 0 */
    EXECUTANDO,  /* 1 */
    BLOQUEADO,   /* 2 */
    MORTO       /* 3 */
};

#define MAX_PROCESSOS 4

#endif
