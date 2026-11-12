/*
 * NAND flash simulator (host side). Models what firmware has to survive:
 *  - program-once pages, in-order programming within a block, block erase;
 *  - raw bit errors on every read, growing with program/erase wear and retention;
 *  - factory bad blocks (marker in page 0 spare byte 0) and grown failures
 *    (program/erase status FAIL) that become likely past rated endurance;
 *  - power loss at any NAND operation, including mid-program (torn page) and
 *    mid-erase (partially erased block), delivered via longjmp to the harness.
 * Error and timing parameters are illustrative, not calibrated to a real part.
 */
#ifndef NANDSIM_H
#define NANDSIM_H
#include <setjmp.h>
#include <stdint.h>
#include "../fw/fg_nand.h"

typedef struct {
    uint32_t nblk, ppb, dsz, osz;
    uint64_t seed;
    double factory_bad_frac;
    double rber_base;       /* raw bit error rate of a fresh block */
    double rber_wear;       /* added RBER at rated endurance, scales with (pe/endurance)^2 */
    double rber_retention;  /* added RBER per retention tick, scaled by (1 + pe/endurance) */
    uint32_t endurance;     /* rated program/erase cycles */
    double fail_prog_worn;  /* program FAIL probability once past endurance */
    double fail_erase_worn; /* erase FAIL probability once past endurance */
    double fail_prog_base, fail_erase_base;
} nandsim_cfg_t;

typedef struct {
    uint64_t reads, oob_reads, programs, erases, violations;
    uint64_t bits_flipped, program_fails, erase_fails;
    double busy_us; /* modelled array + bus time */
} nandsim_stats_t;

typedef struct {
    fg_nand_t hal;
    nandsim_cfg_t cfg;
    uint8_t *mem;       /* nblk * ppb * (dsz + osz) */
    uint8_t *pstate;    /* per page: 0 erased, 1 programmed */
    uint32_t *ptime;    /* per page: retention clock at program time */
    uint32_t *ec;       /* true erase count per block */
    uint16_t *next_pg;  /* in-order programming pointer */
    uint64_t rng;
    uint32_t now;       /* retention clock, advanced by nandsim_age() */
    /* power-cut injection */
    int64_t cut_countdown; /* -1 = disarmed */
    jmp_buf *cut_env;
    int cut_kind;          /* last cut: 0 before op, 1 torn program, 2 partial erase, 3 during read */
    int cut_filter;        /* which ops count down: 0 all, 1 programs only, 2 erases only */
    nandsim_stats_t st;
} nandsim_t;

nandsim_cfg_t nandsim_default_cfg(uint32_t nblk, uint32_t ppb, uint32_t dsz, uint32_t osz);
int nandsim_create(nandsim_t *s, const nandsim_cfg_t *cfg);
void nandsim_destroy(nandsim_t *s);
void nandsim_arm_cut(nandsim_t *s, int64_t ops_from_now, jmp_buf *env);
void nandsim_disarm(nandsim_t *s);
void nandsim_age(nandsim_t *s, uint32_t ticks);
double nandsim_rber(const nandsim_t *s, uint32_t blk, uint32_t pg);
uint64_t nandsim_rand(nandsim_t *s);

/* datasheet-typical SLC-ish latencies used for the time model */
#define NANDSIM_T_READ_US 50.0
#define NANDSIM_T_PROG_US 500.0
#define NANDSIM_T_ERASE_US 3500.0
#define NANDSIM_BUS_US_PER_BYTE 0.0025 /* 400 MB/s */

#endif
