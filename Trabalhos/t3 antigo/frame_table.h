#ifndef FRAME_TABLE_H
#define FRAME_TABLE_H

#include <stdbool.h>

typedef struct frame_table_t frame_table_t;

frame_table_t *ft_create(int n_frames);
void ft_destroy(frame_table_t *ft);

void ft_set_frame(frame_table_t *ft, int frame, int pid, int vpage, bool dirty);
void ft_free_frame(frame_table_t *ft, int frame);
int ft_find_free(frame_table_t *ft);
int ft_get_owner(frame_table_t *ft, int frame, int *pid, int *vpage, bool *dirty, bool *accessed);

void ft_set_dirty(frame_table_t *ft, int frame, bool dirty);
void ft_set_accessed(frame_table_t *ft, int frame, bool accessed);

int ft_choose_victim_fifo(frame_table_t *ft);
int ft_choose_victim_lru(frame_table_t *ft);

#endif // FRAME_TABLE_H