#include "swap.h"
#include "memoria.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct swap_t {
  mem_t *backing;   // área de backing store (palavras)
  int total_words;
  int page_size;
  int total_pages;
  int *alloc; // mapa: 0 livre, 1 ocupado (por simplicidade guardamos apenas contiguo)
  int disk_busy_until;
};

swap_t *swap_create(int total_words, int page_size) {
  swap_t *s = malloc(sizeof(*s));
  assert(s);
  s->backing = mem_cria(total_words);
  s->total_words = total_words;
  s->page_size = page_size;
  s->total_pages = total_words / page_size;
  s->alloc = calloc(s->total_pages, sizeof(int));
  s->disk_busy_until = 0;
  return s;
}

void swap_destroy(swap_t *s) {
  if (!s) return;
  mem_destroi(s->backing);
  free(s->alloc);
  free(s);
}

int swap_alloc(swap_t *s, int num_pages) {
  if (num_pages <= 0 || num_pages > s->total_pages) return -1;
  for (int i = 0; i <= s->total_pages - num_pages; i++) {
    int ok = 1;
    for (int j = 0; j < num_pages; j++) {
      if (s->alloc[i+j]) { ok = 0; break; }
    }
    if (ok) {
      for (int j = 0; j < num_pages; j++) s->alloc[i+j] = 1;
      return i;
    }
  }
  return -1;
}

void swap_free(swap_t *s, int base_page, int num_pages) {
  if (!s || base_page < 0) return;
  for (int j = 0; j < num_pages && base_page + j < s->total_pages; j++) s->alloc[base_page+j] = 0;
}

void swap_write_page(swap_t *s, int base_page, int page_idx, int *buffer, int page_size) {
  if (!s) return;
  int page = base_page + page_idx;
  int start = page * s->page_size;
  for (int i = 0; i < s->page_size; i++) {
    mem_escreve(s->backing, start + i, buffer[i]);
  }
}

void swap_read_page(swap_t *s, int base_page, int page_idx, int *buffer, int page_size) {
  if (!s) return;
  int page = base_page + page_idx;
  int start = page * s->page_size;
  for (int i = 0; i < s->page_size; i++) {
    int v;
    mem_le(s->backing, start + i, &v);
    buffer[i] = v;
  }
}

int swap_schedule_io(swap_t *s, int now, int page_count, int io_time_per_page) {
  if (!s) return now;
  int start = (s->disk_busy_until > now) ? s->disk_busy_until : now;
  int duration = page_count * io_time_per_page;
  s->disk_busy_until = start + duration;
  return s->disk_busy_until;
}