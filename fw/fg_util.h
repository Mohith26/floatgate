/* Small freestanding helpers so the firmware never depends on a hosted libc. */
#ifndef FG_UTIL_H
#define FG_UTIL_H

#include <stddef.h>
#include <stdint.h>

static inline void fg_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)v;
}

static inline void fg_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static inline int fg_popcount8(uint8_t x) {
    int c = 0;
    while (x) { x &= (uint8_t)(x - 1); c++; }
    return c;
}

static inline void fg_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t fg_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void fg_put64(uint8_t *p, uint64_t v) { fg_put32(p, (uint32_t)v); fg_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint64_t fg_get64(const uint8_t *p) { return (uint64_t)fg_get32(p) | ((uint64_t)fg_get32(p + 4) << 32); }

/* Bump allocator used only inside fg_*_init(). Nothing allocates after init. */
typedef struct { uint8_t *base; size_t used, cap; } fg_arena_t;

static inline void *fg_arena_take(fg_arena_t *a, size_t n, size_t align) {
    size_t off = (a->used + (align - 1)) & ~(align - 1);
    if (a->base == NULL) { a->used = off + n; return NULL; } /* sizing pass */
    if (off + n > a->cap) return NULL;
    a->used = off + n;
    return a->base + off;
}

#endif
