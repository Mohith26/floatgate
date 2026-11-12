#include "nandsim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint64_t nandsim_rand(nandsim_t *s) { /* splitmix64 */
    uint64_t z = (s->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double urand(nandsim_t *s) { return (double)(nandsim_rand(s) >> 11) * (1.0 / 9007199254740992.0); }

static uint32_t poisson(nandsim_t *s, double lambda) {
    if (lambda <= 0) return 0;
    if (lambda > 40) { /* normal approximation */
        double u1 = urand(s), u2 = urand(s);
        double z = sqrt(-2.0 * log(u1 + 1e-300)) * cos(6.283185307179586 * u2);
        double v = lambda + sqrt(lambda) * z + 0.5;
        return v < 0 ? 0 : (uint32_t)v;
    }
    double L = exp(-lambda), p = 1.0;
    uint32_t k = 0;
    do { k++; p *= urand(s); } while (p > L);
    return k - 1;
}

nandsim_cfg_t nandsim_default_cfg(uint32_t nblk, uint32_t ppb, uint32_t dsz, uint32_t osz) {
    nandsim_cfg_t c;
    memset(&c, 0, sizeof c);
    c.nblk = nblk; c.ppb = ppb; c.dsz = dsz; c.osz = osz;
    c.seed = 1;
    c.endurance = 3000;
    return c;
}

static inline size_t pstride(const nandsim_t *s) { return (size_t)s->cfg.dsz + s->cfg.osz; }
static inline uint8_t *page_ptr(nandsim_t *s, uint32_t b, uint32_t p) {
    return s->mem + ((size_t)b * s->cfg.ppb + p) * pstride(s);
}

double nandsim_rber(const nandsim_t *s, uint32_t blk, uint32_t pg) {
    double w = (double)s->ec[blk] / (double)(s->cfg.endurance ? s->cfg.endurance : 1);
    double age = s->pstate[(size_t)blk * s->cfg.ppb + pg] ? (double)(s->now - s->ptime[(size_t)blk * s->cfg.ppb + pg]) : 0.0;
    return s->cfg.rber_base + s->cfg.rber_wear * w * w + s->cfg.rber_retention * age * (1.0 + w);
}

/* returns 1 if the power cut fires on this operation */
static int cut_check(nandsim_t *s, int op) {
    if (s->cut_countdown < 0) return 0;
    if (s->cut_filter && s->cut_filter != op) return 0;
    if (s->cut_countdown-- > 0) return 0;
    s->cut_countdown = -1;
    return 1;
}

static void cut_now(nandsim_t *s, int kind) {
    s->cut_kind = kind;
    longjmp(*s->cut_env, 1);
}

static int sim_read(void *ctx, uint32_t blk, uint32_t pg, uint8_t *data, uint8_t *oob) {
    nandsim_t *s = (nandsim_t *)ctx;
    if (cut_check(s, 3)) cut_now(s, 3);
    if (blk >= s->cfg.nblk || pg >= s->cfg.ppb) { s->st.violations++; return FG_NAND_FAIL; }
    uint8_t *src = page_ptr(s, blk, pg);
    if (data) memcpy(data, src, s->cfg.dsz);
    memcpy(oob, src + s->cfg.dsz, s->cfg.osz);
    if (data) s->st.reads++; else s->st.oob_reads++;
    s->st.busy_us += NANDSIM_T_READ_US + NANDSIM_BUS_US_PER_BYTE * (double)((data ? s->cfg.dsz : 0) + s->cfg.osz);
    double rber = nandsim_rber(s, blk, pg);
    if (rber > 0) {
        uint32_t nbits = 8u * ((data ? s->cfg.dsz : 0) + s->cfg.osz);
        uint32_t k = poisson(s, rber * (double)nbits);
        for (uint32_t i = 0; i < k; i++) {
            uint32_t bit = (uint32_t)(nandsim_rand(s) % nbits);
            uint32_t byte = bit >> 3;
            if (data) {
                if (byte < s->cfg.dsz) data[byte] ^= (uint8_t)(1u << (bit & 7));
                else oob[byte - s->cfg.dsz] ^= (uint8_t)(1u << (bit & 7));
            } else {
                oob[byte] ^= (uint8_t)(1u << (bit & 7));
            }
        }
        s->st.bits_flipped += k;
    }
    return FG_NAND_OK;
}

static uint8_t partial_mask(nandsim_t *s, double frac) {
    uint8_t m = 0;
    for (int i = 0; i < 8; i++) if (urand(s) < frac) m |= (uint8_t)(1u << i);
    return m;
}

static int sim_program(void *ctx, uint32_t blk, uint32_t pg, const uint8_t *data, const uint8_t *oob) {
    nandsim_t *s = (nandsim_t *)ctx;
    size_t pi = (size_t)blk * s->cfg.ppb + pg;
    int cut = cut_check(s, 1);
    if (cut && (nandsim_rand(s) & 1)) cut_now(s, 0); /* power lost before the command latched */
    if (blk >= s->cfg.nblk || pg >= s->cfg.ppb || s->pstate[pi] || pg < s->next_pg[blk]) {
        s->st.violations++; /* firmware bug: reprogram or out-of-order program */
        return FG_NAND_FAIL;
    }
    uint8_t *dst = page_ptr(s, blk, pg);
    s->pstate[pi] = 1;
    s->ptime[pi] = s->now;
    s->next_pg[blk] = (uint16_t)(pg + 1);
    s->st.programs++;
    s->st.busy_us += NANDSIM_T_PROG_US + NANDSIM_BUS_US_PER_BYTE * (double)(s->cfg.dsz + s->cfg.osz);
    if (cut) { /* torn page: only some cells reached their programmed state */
        double frac = urand(s);
        for (uint32_t i = 0; i < s->cfg.dsz + s->cfg.osz; i++) {
            uint8_t want = i < s->cfg.dsz ? data[i] : oob[i - s->cfg.dsz];
            dst[i] = (uint8_t)(0xFF & ~((uint8_t)~want & partial_mask(s, frac)));
        }
        cut_now(s, 1);
    }
    double w = (double)s->ec[blk] / (double)s->cfg.endurance;
    double pf = w > 1.0 ? s->cfg.fail_prog_worn : s->cfg.fail_prog_base;
    if (pf > 0 && urand(s) < pf) { /* program FAIL: page left with garbage */
        for (uint32_t i = 0; i < s->cfg.dsz + s->cfg.osz; i++) dst[i] = (uint8_t)nandsim_rand(s);
        s->st.program_fails++;
        return FG_NAND_FAIL;
    }
    memcpy(dst, data, s->cfg.dsz);
    memcpy(dst + s->cfg.dsz, oob, s->cfg.osz);
    return FG_NAND_OK;
}

static int sim_erase(void *ctx, uint32_t blk) {
    nandsim_t *s = (nandsim_t *)ctx;
    int cut = cut_check(s, 2);
    if (cut && (nandsim_rand(s) & 1)) cut_now(s, 0);
    if (blk >= s->cfg.nblk) { s->st.violations++; return FG_NAND_FAIL; }
    size_t bytes = (size_t)s->cfg.ppb * pstride(s);
    uint8_t *b0 = page_ptr(s, blk, 0);
    s->st.erases++;
    s->st.busy_us += NANDSIM_T_ERASE_US;
    s->ec[blk]++;
    if (cut) { /* partially erased: some cells back to 1, page states unknown */
        double frac = urand(s);
        for (size_t i = 0; i < bytes; i++) b0[i] |= partial_mask(s, frac);
        for (uint32_t p = 0; p < s->cfg.ppb; p++) s->pstate[(size_t)blk * s->cfg.ppb + p] = 1;
        s->next_pg[blk] = (uint16_t)s->cfg.ppb;
        cut_now(s, 2);
    }
    double w = (double)s->ec[blk] / (double)s->cfg.endurance;
    double pf = w > 1.0 ? s->cfg.fail_erase_worn : s->cfg.fail_erase_base;
    if (pf > 0 && urand(s) < pf) { s->st.erase_fails++; return FG_NAND_FAIL; }
    memset(b0, 0xFF, bytes);
    for (uint32_t p = 0; p < s->cfg.ppb; p++) s->pstate[(size_t)blk * s->cfg.ppb + p] = 0;
    s->next_pg[blk] = 0;
    return FG_NAND_OK;
}

int nandsim_create(nandsim_t *s, const nandsim_cfg_t *cfg) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    size_t npg = (size_t)cfg->nblk * cfg->ppb;
    s->mem = (uint8_t *)malloc(npg * ((size_t)cfg->dsz + cfg->osz));
    s->pstate = (uint8_t *)calloc(npg, 1);
    s->ptime = (uint32_t *)calloc(npg, sizeof(uint32_t));
    s->ec = (uint32_t *)calloc(cfg->nblk, sizeof(uint32_t));
    s->next_pg = (uint16_t *)calloc(cfg->nblk, sizeof(uint16_t));
    if (!s->mem || !s->pstate || !s->ptime || !s->ec || !s->next_pg) return -1;
    memset(s->mem, 0xFF, npg * ((size_t)cfg->dsz + cfg->osz));
    s->rng = cfg->seed * 0xD1B54A32D192ED03ull + 7;
    s->cut_countdown = -1;
    /* factory bad blocks: block 0 is guaranteed good, as on real parts */
    for (uint32_t b = 1; b < cfg->nblk; b++)
        if (urand(s) < cfg->factory_bad_frac) page_ptr(s, b, 0)[cfg->dsz] = 0x00;
    s->hal.nblk = cfg->nblk; s->hal.ppb = cfg->ppb; s->hal.dsz = cfg->dsz; s->hal.osz = cfg->osz;
    s->hal.ctx = s;
    s->hal.read = sim_read;
    s->hal.program = sim_program;
    s->hal.erase = sim_erase;
    return 0;
}

void nandsim_destroy(nandsim_t *s) {
    free(s->mem); free(s->pstate); free(s->ptime); free(s->ec); free(s->next_pg);
    memset(s, 0, sizeof *s);
}

void nandsim_arm_cut(nandsim_t *s, int64_t ops, jmp_buf *env) { s->cut_countdown = ops; s->cut_env = env; }
void nandsim_disarm(nandsim_t *s) { s->cut_countdown = -1; }
void nandsim_age(nandsim_t *s, uint32_t ticks) { s->now += ticks; }
