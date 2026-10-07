#pragma once
#include <stdlib.h>
#include <stddef.h>
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM 0
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0
#endif
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_DEFAULT 0
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 4u << 20; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 4u << 20; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 4u << 20; }
