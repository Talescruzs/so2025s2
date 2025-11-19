#ifndef PAGE_REPLACE_H
#define PAGE_REPLACE_H

#include "frame_table.h"

typedef enum { REPL_FIFO, REPL_LRU } repl_algo_t;

void pr_init(frame_table_t *ft, repl_algo_t algo);
void pr_on_clock_tick(int current_proc_idx); // atualiza aging para frames do processo atual
int pr_choose_victim(void);

#endif // PAGE_REPLACE_H