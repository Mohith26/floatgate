/*
 * Floatgate FTL: page-mapped flash translation layer for raw NAND.
 *
 * Firmware rules this file follows:
 *  - freestanding C99, no libc, no malloc: every table is carved out of one
 *    caller-provided RAM arena inside fg_init() and never resized;
 *  - a host write is acknowledged only after its NAND program reports success
 *    (no volatile write cache), so "acknowledged" means "on flash";
 *  - every page carries ECC-protected out-of-band metadata (LPN, sequence
 *    number, erase count, CRCs) so the mapping can be rebuilt after power loss.
 */
#ifndef FG_FTL_H
#define FG_FTL_H
#include <stddef.h>
#include <stdint.h>
#include "fg_bch.h"
#include "fg_nand.h"

enum {
    FG_OK = 0,
    FG_ENOENT = -2,   /* logical page never written */
    FG_EIO = -5,      /* uncorrectable media error */
    FG_EINVAL = -22,
    FG_ENOSPC = -28,  /* GC could not reclaim space */
    FG_ECORRUPT = -74 /* internal consistency failure (should never happen) */
};

enum { FG_GC_GREEDY = 0, FG_GC_COST_BENEFIT = 1 };
enum { FG_STREAMS = 4 }; /* host, gc, static-WL (cold), checkpoint */

typedef struct {
    uint32_t nlpn;          /* logical pages exported to the host */
    int ecc_t;              /* BCH correction per sector: 0 (CRC only), 8 or 16 */
    int gc_policy;          /* FG_GC_GREEDY or FG_GC_COST_BENEFIT */
    int wl_dynamic;         /* allocate the least-worn free block (else round robin) */
    int wl_static;          /* periodically move cold data onto worn blocks */
    uint32_t wl_threshold;  /* max-min erase-count gap that triggers static WL */
    uint32_t gc_reserve;    /* free blocks held back for GC */
    uint32_t ckpt_interval; /* host writes between mapping checkpoints, 0 = off */
    int scrub_bits;         /* corrected bits on a read that trigger a refresh, 0 = off */
} fg_cfg_t;

typedef struct {
    uint64_t host_writes, host_reads;
    uint64_t gc_writes, wl_writes, scrub_writes, retire_writes, ckpt_writes;
    uint64_t gc_runs, wl_runs, erases, checkpoints, ckpt_aborts;
    uint64_t ecc_corrected_bits, uncorrectable_reads, lost_pages;
    uint64_t program_fails, erase_fails, grown_bad, factory_bad;
    uint64_t mount_page_reads, mount_full_reads, mount_scanned_blocks, mount_torn_pages;
    uint32_t mount_used_ckpt;
} fg_stats_t;

#define FG_CKPT_MAXBLK 64

typedef struct {
    fg_cfg_t cfg;
    const fg_nand_t *nand;
    uint32_t nblk, ppb, dsz, osz, sect, nsect, rb;
    uint32_t *crc;
    fg_bch_t bch;
    uint32_t *l2p;     /* logical -> physical page */
    uint32_t *vbits;   /* physical page valid bitmap */
    uint16_t *bvalid;  /* valid pages per block */
    uint16_t *bwp;     /* next page to program per block */
    uint32_t *bec;     /* erase count per block */
    uint32_t *bmtime;  /* last-modified time per block (host-write clock) */
    uint8_t *bstate;
    uint64_t *bseq0;   /* mount scratch: sequence number of page 0 */
    uint32_t *mbits;   /* mount scratch: LPN set by this mount's scan */
    uint8_t *obuf, *dbuf, *zbuf;
    uint64_t seq;
    uint32_t clock, nfree, cursor, since_ckpt, allocs, wl_mark, retire_pending;
    uint32_t open[FG_STREAMS];
    uint32_t ckblk[FG_CKPT_MAXBLK], nck;
    uint8_t ckhdr[64];
    fg_stats_t st;
} fg_ftl_t;

size_t fg_ram_bytes(const fg_cfg_t *cfg, const fg_nand_t *nand);
int fg_init(fg_ftl_t *f, const fg_cfg_t *cfg, const fg_nand_t *nand, void *ram, size_t ram_bytes);
int fg_format(fg_ftl_t *f);   /* first power-on: scan factory bad-block markers */
int fg_mount(fg_ftl_t *f);    /* every later power-on: rebuild state from flash */
int fg_write(fg_ftl_t *f, uint32_t lpn, const uint8_t *data);
int fg_read(fg_ftl_t *f, uint32_t lpn, uint8_t *data);
int fg_checkpoint(fg_ftl_t *f);
int fg_check(const fg_ftl_t *f); /* internal invariants, 0 if consistent */
uint32_t fg_free_blocks(const fg_ftl_t *f);

#endif
