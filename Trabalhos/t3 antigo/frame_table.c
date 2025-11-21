#include "frame_table.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct frame_entry {
  int used;
  int pid;
  int vpage;
  unsigned aging;
  unsigned fifo_idx;
  int accessed;
  int dirty;
};

struct frame_table_t {
  int n_frames;
  struct frame_entry *entries;
  unsigned fifo_counter;
};

frame_table_t *ft_create(int n_frames) {
  frame_table_t *ft = malloc(sizeof(*ft));
  ft->n_frames = n_frames;
  ft->entries = calloc(n_frames, sizeof(struct frame_entry));
  ft->fifo_counter = 1;
  for (int i = 0; i < n_frames; i++) {
    ft->entries[i].used = 0;
    ft->entries[i].pid = -1;
    ft->entries[i].vpage = -1;
    ft->entries[i].aging = 0;
    ft->entries[i].fifo_idx = 0;
    ft->entries[i].accessed = 0;
    ft->entries[i].dirty = 0;
  }
  return ft;
}

void ft_destroy(frame_table_t *ft) {
  if (!ft) return;
  free(ft->entries);
  free(ft);
}

void ft_set_frame(frame_table_t *ft, int frame, int pid, int vpage, bool dirty) {
  if (!ft || frame < 0 || frame >= ft->n_frames) return;
  ft->entries[frame].used = 1;
  ft->entries[frame].pid = pid;
  ft->entries[frame].vpage = vpage;
  ft->entries[frame].dirty = dirty ? 1 : 0;
  ft->entries[frame].accessed = 1;
  ft->entries[frame].aging = 0x80000000u; // set MSB as recent access
  ft->entries[frame].fifo_idx = ft->fifo_counter++;
}

void ft_free_frame(frame_table_t *ft, int frame) {
  if (!ft || frame < 0 || frame >= ft->n_frames) return;
  ft->entries[frame].used = 0;
  ft->entries[frame].pid = -1;
  ft->entries[frame].vpage = -1;
  ft->entries[frame].aging = 0;
  ft->entries[frame].accessed = 0;
  ft->entries[frame].dirty = 0;
  ft->entries[frame].fifo_idx = 0;
}

int ft_find_free(frame_table_t *ft) {
  for (int i = 0; i < ft->n_frames; i++) if (!ft->entries[i].used) return i;
  return -1;
}

int ft_get_owner(frame_table_t *ft, int frame, int *pid, int *vpage, bool *dirty, bool *accessed) {
  if (!ft || frame < 0 || frame >= ft->n_frames) return -1;
  if (pid) *pid = ft->entries[frame].pid;
  if (vpage) *vpage = ft->entries[frame].vpage;
  if (dirty) *dirty = ft->entries[frame].dirty;
  if (accessed) *accessed = ft->entries[frame].accessed;
  return 0;
}

void ft_set_dirty(frame_table_t *ft, int frame, bool dirty) { if (!ft) return; ft->entries[frame].dirty = dirty ? 1 : 0; }
void ft_set_accessed(frame_table_t *ft, int frame, bool accessed) { if (!ft) return; ft->entries[frame].accessed = accessed ? 1 : 0; }

int ft_choose_victim_fifo(frame_table_t *ft) {
  unsigned best_idx = UINT_MAX;
  int best = -1;
  for (int i = 0; i < ft->n_frames; i++) {
    if (ft->entries[i].used && ft->entries[i].fifo_idx < best_idx) {
      best_idx = ft->entries[i].fifo_idx;
      best = i;
    }
  }
  return best;
}

int ft_choose_victim_lru(frame_table_t *ft) {
  unsigned best_age = UINT_MAX;
  int best = -1;
  for (int i = 0; i < ft->n_frames; i++) {
    if (ft->entries[i].used && ft->entries[i].aging < best_age) {
      best_age = ft->entries[i].aging;
      best = i;
    }
  }
  return best;
}