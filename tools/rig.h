/* Host-side rig shared by the tests and tools: simulator + firmware + payload helpers. */
#ifndef RIG_H
#define RIG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../fw/fg_ftl.h"
#include "../sim/nandsim.h"

typedef struct {
    nandsim_t nand;
    fg_ftl_t ftl;
    fg_cfg_t cfg;
    void *ram;      /* firmware RAM arena: allocated once by the host, never by firmware */
    size_t ram_bytes;
} rig_t;

static inline int rig_open(rig_t *r, const nandsim_cfg_t *ncfg, const fg_cfg_t *fcfg) {
    memset(r, 0, sizeof *r);
    if (nandsim_create(&r->nand, ncfg)) return -1;
    r->cfg = *fcfg;
    r->ram_bytes = fg_ram_bytes(fcfg, &r->nand.hal);
    r->ram = malloc(r->ram_bytes);
    if (!r->ram) return -1;
    return fg_init(&r->ftl, fcfg, &r->nand.hal, r->ram, r->ram_bytes);
}

/* Simulated reboot: all firmware RAM is wiped, only the NAND array survives. */
static inline int rig_reboot(rig_t *r) {
    memset(r->ram, 0xA5, r->ram_bytes);
    int rc = fg_init(&r->ftl, &r->cfg, &r->nand.hal, r->ram, r->ram_bytes);
    if (rc) return rc;
    return fg_mount(&r->ftl);
}

static inline void rig_close(rig_t *r) {
    nandsim_destroy(&r->nand);
    free(r->ram);
}

static inline uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Self-describing payload: [lpn][version][pseudo-random body keyed by both]. */
static inline void payload_fill(uint8_t *buf, uint32_t n, uint32_t lpn, uint32_t ver) {
    memcpy(buf, &lpn, 4);
    memcpy(buf + 4, &ver, 4);
    uint64_t st = ((uint64_t)lpn << 32) ^ ver;
    for (uint32_t i = 8; i < n; i += 8) {
        uint64_t v = mix64(st + i);
        memcpy(buf + i, &v, (n - i) < 8 ? (n - i) : 8);
    }
}

/* Returns the version stored in buf if the whole payload is intact for lpn, else -1. */
static inline int64_t payload_check(const uint8_t *buf, uint32_t n, uint32_t lpn) {
    uint32_t l, v;
    memcpy(&l, buf, 4);
    memcpy(&v, buf + 4, 4);
    if (l != lpn) return -1;
    uint64_t st = ((uint64_t)lpn << 32) ^ v;
    for (uint32_t i = 8; i < n; i += 8) {
        uint64_t x = mix64(st + i);
        if (memcmp(buf + i, &x, (n - i) < 8 ? (n - i) : 8)) return -1;
    }
    return v;
}

static inline double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static inline fg_cfg_t default_fcfg(uint32_t nlpn) {
    fg_cfg_t c;
    memset(&c, 0, sizeof c);
    c.nlpn = nlpn;
    c.ecc_t = 8;
    c.gc_policy = FG_GC_COST_BENEFIT;
    c.wl_dynamic = 1;
    c.wl_static = 1;
    c.wl_threshold = 64;
    c.gc_reserve = 3;
    c.ckpt_interval = 0;
    c.scrub_bits = 0;
    return c;
}

#endif
