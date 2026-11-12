#include "fg_ftl.h"
#include "fg_crc32.h"
#include "fg_util.h"

#ifndef FG_MUTANT
#define FG_MUTANT 0
#endif

#define NONE 0xFFFFFFFFu
#define UNMAPPED 0xFFFFFFFFu
#define BADBIT 0x80000000u

enum { S_HOST = 0, S_GC = 1, S_WL = 2, S_CKPT = 3 };
enum { BS_FREE = 0, BS_OPEN, BS_FULL, BS_BAD, BS_CKPT, BS_RETIRE };
enum { T_EMPTY = 0x10, T_DATA, T_CKPT, T_UNKNOWN }; /* mount-time page-0 classes */
enum { PT_DATA = 0xD1, PT_CKPT = 0xC5 };
enum { F_LOST = 1, F_LAST = 2 };
enum { RP_ERASED = -1, RP_BAD = -2, RP_UNCORR = -3 };

/* OOB layout: [0] bad-block marker | [1..29) metadata | metadata parity | sector parities */
#define OOB_BBM 0
#define OOB_META 1
#define META_LEN 28
#define OOB_MPAR (OOB_META + META_LEN)
#define CK_HDR 64
#define CK_MAGIC 0x4B434746u /* "FGCK" */

typedef struct {
    uint8_t type, flags;
    uint32_t lpn, ec, dcrc;
    uint64_t seq;
} meta_t;

/* ------------------------------------------------------------------ setup */

static int carve(fg_ftl_t *f, fg_arena_t *a, const fg_cfg_t *cfg, const fg_nand_t *n) {
    uint32_t npg = n->nblk * n->ppb;
#define TAKE(ptr, type, count) do { ptr = (type *)fg_arena_take(a, sizeof(type) * (size_t)(count), 8); \
        if (a->base && !(ptr)) return FG_EINVAL; } while (0)
    TAKE(f->crc, uint32_t, 256);
    if (cfg->ecc_t > 0) {
        void *bram = fg_arena_take(a, fg_bch_ram_bytes(cfg->ecc_t), 8);
        if (a->base) {
            if (!bram || fg_bch_init(&f->bch, cfg->ecc_t, bram)) return FG_EINVAL;
        }
    }
    TAKE(f->l2p, uint32_t, cfg->nlpn);
    TAKE(f->vbits, uint32_t, (npg + 31) / 32);
    TAKE(f->bvalid, uint16_t, n->nblk);
    TAKE(f->bwp, uint16_t, n->nblk);
    TAKE(f->bec, uint32_t, n->nblk);
    TAKE(f->bmtime, uint32_t, n->nblk);
    TAKE(f->bstate, uint8_t, n->nblk);
    TAKE(f->bseq0, uint64_t, n->nblk);
    TAKE(f->mbits, uint32_t, (cfg->nlpn + 31) / 32);
    TAKE(f->obuf, uint8_t, n->osz);
    TAKE(f->dbuf, uint8_t, n->dsz);
    TAKE(f->zbuf, uint8_t, n->dsz);
#undef TAKE
    return FG_OK;
}

size_t fg_ram_bytes(const fg_cfg_t *cfg, const fg_nand_t *nand) {
    fg_ftl_t tmp;
    fg_arena_t a = {0, 0, 0};
    carve(&tmp, &a, cfg, nand);
    return a.used + 8;
}

int fg_init(fg_ftl_t *f, const fg_cfg_t *cfg, const fg_nand_t *n, void *ram, size_t ram_bytes) {
    fg_memset(f, 0, sizeof *f);
    f->cfg = *cfg;
    f->nand = n;
    f->nblk = n->nblk; f->ppb = n->ppb; f->dsz = n->dsz; f->osz = n->osz;
    if (n->ppb > 65535u || n->ppb < 2 || cfg->nlpn == 0) return FG_EINVAL;
    if ((uint64_t)n->nblk * n->ppb >= 0xFFFFFFFFull) return FG_EINVAL;
    f->sect = n->dsz < 512 ? n->dsz : 512;
    if (n->dsz % f->sect) return FG_EINVAL;
    f->nsect = n->dsz / f->sect;
    f->rb = cfg->ecc_t > 0 ? (uint32_t)(FG_BCH_M * cfg->ecc_t / 8) : 0;
    if (n->osz < OOB_MPAR + f->rb * (1 + f->nsect)) return FG_EINVAL;
    if (cfg->gc_reserve < 2) f->cfg.gc_reserve = 2;
    fg_arena_t a = {(uint8_t *)(((uintptr_t)ram + 7) & ~(uintptr_t)7), 0, 0};
    a.cap = ram_bytes - (size_t)(a.base - (uint8_t *)ram);
    int rc = carve(f, &a, cfg, n);
    if (rc) return rc;
    fg_crc32_init(f->crc);
    fg_memset(f->zbuf, 0, n->dsz);
    /* usable logical space must leave room for GC */
    if ((uint64_t)cfg->nlpn + (uint64_t)(f->cfg.gc_reserve + 4) * n->ppb > (uint64_t)n->nblk * n->ppb) return FG_EINVAL;
    return FG_OK;
}

/* ------------------------------------------------------------ page codec */

static void encode_page(fg_ftl_t *f, uint8_t type, uint8_t flags, uint32_t lpn, uint64_t seq,
                        uint32_t ec, const uint8_t *data) {
    uint8_t *o = f->obuf, *m = f->obuf + OOB_META;
    fg_memset(o, 0xFF, f->osz);
    m[0] = type; m[1] = flags; m[2] = 0; m[3] = 0;
    fg_put32(m + 4, lpn);
    fg_put64(m + 8, seq);
    fg_put32(m + 16, ec);
    fg_put32(m + 20, fg_crc32(f->crc, data, f->dsz));
    fg_put32(m + 24, fg_crc32(f->crc, m, 24));
    if (f->rb) {
        fg_bch_encode(&f->bch, m, META_LEN, o + OOB_MPAR);
        for (uint32_t s = 0; s < f->nsect; s++)
            fg_bch_encode(&f->bch, data + s * f->sect, f->sect, o + OOB_MPAR + f->rb * (1 + s));
    }
}

static int program_page(fg_ftl_t *f, uint32_t blk, uint32_t pg, uint8_t type, uint8_t flags, uint32_t lpn,
                        uint64_t seq, const uint8_t *data) {
    encode_page(f, type, flags, lpn, seq, f->bec[blk], data);
    return f->nand->program(f->nand->ctx, blk, pg, data, f->obuf);
}

/* Reads a page; data may be NULL for a spare-area-only read. Returns corrected
 * bit count (>= 0) or RP_ERASED / RP_BAD (metadata unreadable) / RP_UNCORR. */
static int read_page(fg_ftl_t *f, uint32_t blk, uint32_t pg, uint8_t *data, meta_t *mt) {
    uint8_t *o = f->obuf, *m = f->obuf + OOB_META;
    f->nand->read(f->nand->ctx, blk, pg, data, o);
    int zeros = 0;
    for (int i = 0; i < META_LEN; i++) zeros += 8 - fg_popcount8(m[i]);
    if (zeros <= 6) return RP_ERASED; /* blank page, allowing for a few bit flips */
    int corrected = 0;
    if (f->rb) {
        int c = fg_bch_decode(&f->bch, m, META_LEN, o + OOB_MPAR);
        if (c < 0) return RP_BAD;
        corrected += c;
    }
    if (fg_crc32(f->crc, m, 24) != fg_get32(m + 24)) return RP_BAD;
    mt->type = m[0]; mt->flags = m[1];
    mt->lpn = fg_get32(m + 4);
    mt->seq = fg_get64(m + 8);
    mt->ec = fg_get32(m + 16);
    mt->dcrc = fg_get32(m + 20);
    if (!data) return corrected;
    if (f->rb) {
        for (uint32_t s = 0; s < f->nsect; s++) {
            int c = fg_bch_decode(&f->bch, data + s * f->sect, f->sect, o + OOB_MPAR + f->rb * (1 + s));
            if (c < 0) return RP_UNCORR;
            corrected += c;
        }
    }
    if (fg_crc32(f->crc, data, f->dsz) != mt->dcrc) return RP_UNCORR;
    return corrected;
}

/* --------------------------------------------------------- block helpers */

static inline int vtest(const fg_ftl_t *f, uint32_t p) { return (int)((f->vbits[p >> 5] >> (p & 31)) & 1u); }
static inline void vset(fg_ftl_t *f, uint32_t p) { f->vbits[p >> 5] |= 1u << (p & 31); f->bvalid[p / f->ppb]++; }
static inline void vclr(fg_ftl_t *f, uint32_t p) { f->vbits[p >> 5] &= ~(1u << (p & 31)); f->bvalid[p / f->ppb]--; }

static uint32_t alloc_block(fg_ftl_t *f, int prefer_worn) {
    for (;;) {
        uint32_t pick = NONE;
        if (prefer_worn || f->cfg.wl_dynamic) {
            for (uint32_t b = 0; b < f->nblk; b++) {
                if (f->bstate[b] != BS_FREE) continue;
                if (pick == NONE || (prefer_worn ? f->bec[b] > f->bec[pick] : f->bec[b] < f->bec[pick])) pick = b;
            }
        } else {
            for (uint32_t i = 0; i < f->nblk; i++) {
                uint32_t b = (f->cursor + i) % f->nblk;
                if (f->bstate[b] == BS_FREE) { pick = b; f->cursor = (b + 1) % f->nblk; break; }
            }
        }
        if (pick == NONE) return NONE;
        f->nfree--;
        f->bec[pick]++;
        f->st.erases++;
        f->allocs++;
        if (f->nand->erase(f->nand->ctx, pick) != FG_NAND_OK) {
            f->bstate[pick] = BS_BAD;
            f->st.erase_fails++;
            f->st.grown_bad++;
            continue;
        }
        f->bstate[pick] = BS_OPEN;
        f->bwp[pick] = 0;
        f->bvalid[pick] = 0;
        f->bmtime[pick] = f->clock;
        return pick;
    }
}

/* Appends one page to a stream's open block. seq == 0 means "next sequence number". */
static uint32_t prog_stream(fg_ftl_t *f, int s, uint8_t type, uint8_t flags, uint32_t lpn, uint64_t seq,
                            const uint8_t *data) {
    for (int tries = 0; tries < 64; tries++) {
        uint32_t blk = f->open[s];
        if (blk == NONE) {
            blk = alloc_block(f, s == S_WL);
            if (blk == NONE) return NONE;
            f->open[s] = blk;
        }
        uint32_t pg = f->bwp[blk]++;
        if (f->bwp[blk] == f->ppb) { f->bstate[blk] = BS_FULL; f->open[s] = NONE; }
        uint64_t sq = seq ? seq : f->seq++;
        if (program_page(f, blk, pg, type, flags, lpn, sq, data) == FG_NAND_OK) {
            f->bmtime[blk] = f->clock;
            return blk * f->ppb + pg;
        }
        /* program status FAIL: retire the block once its valid pages are moved out */
        f->st.program_fails++;
        f->bstate[blk] = BS_RETIRE;
        f->retire_pending++;
        if (f->open[s] == blk) f->open[s] = NONE;
    }
    return NONE;
}

static uint32_t find_lpn(const fg_ftl_t *f, uint32_t ppn) {
    for (uint32_t l = 0; l < f->cfg.nlpn; l++) if (f->l2p[l] == ppn) return l;
    return NONE;
}

/* Moves every valid page out of blk. Returns 0 or FG_ENOSPC. */
static int relocate(fg_ftl_t *f, uint32_t blk, int stream, uint64_t *counter) {
    for (uint32_t pg = 0; pg < f->bwp[blk] && f->bvalid[blk]; pg++) {
        uint32_t ppn = blk * f->ppb + pg;
        if (!vtest(f, ppn)) continue;
        meta_t mt;
        int rc = read_page(f, blk, pg, f->dbuf, &mt);
        uint32_t lpn;
        uint8_t flags = 0;
        const uint8_t *src = f->dbuf;
        if (rc >= 0) {
            f->st.ecc_corrected_bits += (uint64_t)rc;
            lpn = mt.lpn;
            flags = mt.flags & F_LOST;
            if (lpn >= f->cfg.nlpn || f->l2p[lpn] != ppn) lpn = find_lpn(f, ppn);
        } else {
            /* data unrecoverable: persist a "lost" marker so reads fail loudly instead of returning stale data */
            f->st.uncorrectable_reads++;
            f->st.lost_pages++;
            lpn = (rc == RP_UNCORR && mt.lpn < f->cfg.nlpn && f->l2p[mt.lpn] == ppn) ? mt.lpn : find_lpn(f, ppn);
            flags = F_LOST;
            src = f->zbuf;
        }
        if (lpn == NONE) { vclr(f, ppn); continue; }
        uint32_t np = prog_stream(f, stream, PT_DATA, flags, lpn, 0, src);
        if (np == NONE) return FG_ENOSPC;
        vclr(f, ppn);
        f->l2p[lpn] = np;
        vset(f, np);
        (*counter)++;
    }
    return FG_OK;
}

static void free_block(fg_ftl_t *f, uint32_t blk) {
    f->bstate[blk] = BS_FREE;
    f->bvalid[blk] = 0;
    f->nfree++;
    /* erase happens lazily when the block is next allocated */
}

static void process_retired(fg_ftl_t *f) {
    while (f->retire_pending) {
        f->retire_pending = 0;
        for (uint32_t b = 0; b < f->nblk; b++) {
            if (f->bstate[b] != BS_RETIRE) continue;
            if (relocate(f, b, S_GC, &f->st.retire_writes) != FG_OK) { f->retire_pending++; return; }
            f->bstate[b] = BS_BAD;
            f->st.grown_bad++;
        }
    }
}

static uint32_t pick_victim(const fg_ftl_t *f) {
    uint32_t best = NONE;
    float best_score = -1.0f;
    for (uint32_t b = 0; b < f->nblk; b++) {
        if (f->bstate[b] != BS_FULL || f->bvalid[b] >= f->ppb) continue;
        if (f->cfg.gc_policy == FG_GC_GREEDY) {
            if (best == NONE || f->bvalid[b] < f->bvalid[best]) best = b;
        } else {
            /* Rosenblum & Ousterhout: benefit/cost = (1-u) * age / (1+u) */
            float u = (float)f->bvalid[b] / (float)f->ppb;
            float age = (float)(f->clock - f->bmtime[b]) + 1.0f;
            float score = (1.0f - u) * age / (1.0f + u);
            if (score > best_score) { best_score = score; best = b; }
        }
    }
    return best;
}

static int gc_once(fg_ftl_t *f) {
    uint32_t v = pick_victim(f);
    if (v == NONE) return FG_ENOSPC;
    int rc = relocate(f, v, S_GC, &f->st.gc_writes);
    if (rc) return rc;
    free_block(f, v);
    f->st.gc_runs++;
    return FG_OK;
}

static int make_room(fg_ftl_t *f, uint32_t want) {
    uint32_t guard = 4 * f->nblk + 16;
    while (f->nfree < want) {
        if (f->retire_pending) process_retired(f);
        if (f->nfree >= want) break;
        if (gc_once(f) != FG_OK || --guard == 0) return FG_ENOSPC;
    }
    return FG_OK;
}

static void static_wl(fg_ftl_t *f) {
    if (!f->cfg.wl_static || f->allocs - f->wl_mark < 16u) return;
    f->wl_mark = f->allocs;
    uint32_t cold = NONE, maxec = 0;
    for (uint32_t b = 0; b < f->nblk; b++) {
        if (f->bstate[b] == BS_BAD) continue;
        if (f->bec[b] > maxec) maxec = f->bec[b];
        if (f->bstate[b] == BS_FULL && (cold == NONE || f->bec[b] < f->bec[cold])) cold = b;
    }
    if (cold == NONE || maxec - f->bec[cold] <= f->cfg.wl_threshold) return;
    if (make_room(f, f->cfg.gc_reserve + 2) != FG_OK || f->bstate[cold] != BS_FULL) return;
    if (relocate(f, cold, S_WL, &f->st.wl_writes) == FG_OK) {
        free_block(f, cold);
        f->st.wl_runs++;
    }
}

/* -------------------------------------------------------- checkpointing */

static uint32_t ckpt_bytes(const fg_ftl_t *f) { return CK_HDR + 4u * f->cfg.nlpn + 4u * f->nblk; }

/* Moves one checkpoint chunk between flash page buffer and RAM tables (dir 0: RAM -> buf, 1: buf -> RAM). */
static void ckpt_xfer(fg_ftl_t *f, uint32_t chunk, uint8_t *buf, int dir) {
    uint32_t total = ckpt_bytes(f);
    uint32_t l2p_end = CK_HDR + 4u * f->cfg.nlpn;
    for (uint32_t i = 0; i < f->dsz; i++) {
        uint32_t g = chunk * f->dsz + i;
        uint8_t *p;
        if (g >= total) { if (!dir) buf[i] = 0; continue; }
        if (g < CK_HDR) p = &f->ckhdr[g];
        else if (g < l2p_end) p = (uint8_t *)&f->l2p[(g - CK_HDR) >> 2] + ((g - CK_HDR) & 3);
        else p = (uint8_t *)&f->bec[(g - l2p_end) >> 2] + ((g - l2p_end) & 3);
        if (!dir) buf[i] = *p; else *p = buf[i];
    }
}

int fg_checkpoint(fg_ftl_t *f) {
    uint32_t npages = (ckpt_bytes(f) + f->dsz - 1) / f->dsz;
    uint32_t nblocks = (npages + f->ppb - 1) / f->ppb;
    if (nblocks + 1 > FG_CKPT_MAXBLK) return FG_EINVAL;
    if (f->retire_pending) process_retired(f);
    if (make_room(f, nblocks + f->cfg.gc_reserve + 1)) return FG_ENOSPC;
    uint64_t S = f->seq++;
    uint8_t *h = f->ckhdr;
    fg_memset(h, 0, CK_HDR);
    fg_put32(h, CK_MAGIC);
    fg_put32(h + 4, 1);
    fg_put32(h + 8, f->cfg.nlpn);
    fg_put32(h + 12, f->nblk);
    fg_put64(h + 16, S);
    fg_put32(h + 24, f->open[S_HOST]);
    fg_put32(h + 28, f->open[S_GC]);
    fg_put32(h + 32, f->open[S_WL]);
    fg_put32(h + 36, npages);
    for (uint32_t b = 0; b < f->nblk; b++) if (f->bstate[b] == BS_BAD || f->bstate[b] == BS_RETIRE) f->bec[b] |= BADBIT;
    uint64_t fails0 = f->st.program_fails;
    uint32_t newblk[FG_CKPT_MAXBLK], nnew = 0;
    f->open[S_CKPT] = NONE;
    int ok = 1;
    for (uint32_t i = 0; i < npages && ok; i++) {
        ckpt_xfer(f, i, f->dbuf, 0);
        uint32_t np = prog_stream(f, S_CKPT, PT_CKPT, i + 1 == npages ? F_LAST : 0, i, S, f->dbuf);
        if (np == NONE || f->st.program_fails != fails0) { ok = 0; break; }
        uint32_t b = np / f->ppb;
        if (nnew == 0 || newblk[nnew - 1] != b) {
            if (nnew == FG_CKPT_MAXBLK) { ok = 0; break; }
            newblk[nnew++] = b;
        }
        f->st.ckpt_writes++;
    }
    for (uint32_t b = 0; b < f->nblk; b++) f->bec[b] &= ~BADBIT;
    if (f->open[S_CKPT] != NONE) { f->bstate[f->open[S_CKPT]] = BS_FULL; f->open[S_CKPT] = NONE; }
    if (!ok) {
        for (uint32_t i = 0; i < nnew; i++)
            if (f->bstate[newblk[i]] == BS_FULL) free_block(f, newblk[i]);
        f->st.ckpt_aborts++;
        process_retired(f);
        return FG_EIO;
    }
    for (uint32_t i = 0; i < f->nck; i++) if (f->bstate[f->ckblk[i]] == BS_CKPT) free_block(f, f->ckblk[i]);
    for (uint32_t i = 0; i < nnew; i++) { f->bstate[newblk[i]] = BS_CKPT; f->ckblk[i] = newblk[i]; }
    f->nck = nnew;
    f->since_ckpt = 0;
    f->st.checkpoints++;
    return FG_OK;
}

/* ----------------------------------------------------------- host API */

int fg_write(fg_ftl_t *f, uint32_t lpn, const uint8_t *data) {
    if (lpn >= f->cfg.nlpn) return FG_EINVAL;
    if (make_room(f, f->cfg.gc_reserve + 1)) return FG_ENOSPC;
    uint32_t np = prog_stream(f, S_HOST, PT_DATA, 0, lpn, 0, data);
    if (np == NONE) return FG_ENOSPC;
    uint32_t old = f->l2p[lpn];
    if (old != UNMAPPED) vclr(f, old);
    f->l2p[lpn] = np;
    vset(f, np);
    f->st.host_writes++;
    f->clock++;
    /* the write is durable from here on; housekeeping below may be interrupted safely */
    if (f->retire_pending) process_retired(f);
    static_wl(f);
    if (f->cfg.ckpt_interval && ++f->since_ckpt >= f->cfg.ckpt_interval) fg_checkpoint(f);
    return FG_OK;
}

int fg_read(fg_ftl_t *f, uint32_t lpn, uint8_t *data) {
    if (lpn >= f->cfg.nlpn) return FG_EINVAL;
    uint32_t ppn = f->l2p[lpn];
    if (ppn == UNMAPPED) return FG_ENOENT;
    f->st.host_reads++;
    meta_t mt;
    int rc = read_page(f, ppn / f->ppb, ppn % f->ppb, data, &mt);
    if (rc < 0) { f->st.uncorrectable_reads++; return FG_EIO; }
    f->st.ecc_corrected_bits += (uint64_t)rc;
    if (mt.type != PT_DATA || mt.lpn != lpn) return FG_ECORRUPT;
    if (mt.flags & F_LOST) return FG_EIO;
    if (f->cfg.scrub_bits && rc >= f->cfg.scrub_bits && f->nfree > f->cfg.gc_reserve + 1) {
        /* read-scrub: rewrite data that is drifting toward the ECC limit */
        uint32_t np = prog_stream(f, S_GC, PT_DATA, 0, lpn, 0, data);
        if (np != NONE && f->l2p[lpn] == ppn) {
            vclr(f, ppn);
            f->l2p[lpn] = np;
            vset(f, np);
            f->st.scrub_writes++;
        }
        if (f->retire_pending) process_retired(f);
    }
    return FG_OK;
}

/* ------------------------------------------------------- format & mount */

static void reset_ram(fg_ftl_t *f) {
    for (uint32_t l = 0; l < f->cfg.nlpn; l++) f->l2p[l] = UNMAPPED;
    fg_memset(f->vbits, 0, sizeof(uint32_t) * ((f->nblk * f->ppb + 31) / 32));
    fg_memset(f->mbits, 0, sizeof(uint32_t) * ((f->cfg.nlpn + 31) / 32));
    for (uint32_t b = 0; b < f->nblk; b++) {
        f->bvalid[b] = 0; f->bwp[b] = 0; f->bec[b] = 0; f->bmtime[b] = 0; f->bstate[b] = BS_FREE; f->bseq0[b] = 0;
    }
    for (int s = 0; s < FG_STREAMS; s++) f->open[s] = NONE;
    f->nck = 0; f->nfree = 0; f->clock = 0; f->since_ckpt = 0; f->retire_pending = 0; f->cursor = 0;
}

static int factory_bad(fg_ftl_t *f, uint32_t blk) {
    f->nand->read(f->nand->ctx, blk, 0, NULL, f->obuf);
    return fg_popcount8(f->obuf[OOB_BBM]) <= 4; /* marker is 0x00; tolerate bit flips */
}

int fg_format(fg_ftl_t *f) {
    reset_ram(f);
    for (uint32_t b = 0; b < f->nblk; b++) {
        if (factory_bad(f, b)) { f->bstate[b] = BS_BAD; f->st.factory_bad++; }
        else { f->bstate[b] = BS_FREE; f->nfree++; }
    }
    f->seq = 1;
    if (f->nfree * f->ppb < f->cfg.nlpn + (f->cfg.gc_reserve + 4) * f->ppb) return FG_ENOSPC;
    return FG_OK;
}

static void commit_scan(fg_ftl_t *f, uint32_t lpn, uint32_t ppn, uint64_t seq) {
    if (lpn >= f->cfg.nlpn) return;
    if ((f->mbits[lpn >> 5] >> (lpn & 31)) & 1u) {
        meta_t cur;
        uint32_t c = f->l2p[lpn];
        f->st.mount_page_reads++;
        if (read_page(f, c / f->ppb, c % f->ppb, NULL, &cur) >= 0 && cur.seq > seq) return;
#if FG_MUTANT == 3 /* fault-injection build: first copy found wins, ignoring sequence numbers */
        return;
#endif
    }
    f->l2p[lpn] = ppn;
    f->mbits[lpn >> 5] |= 1u << (lpn & 31);
}

/* Replays every page in blk with seq > floor. Only the last programmed page of a
 * block can be torn by power loss, so that one alone gets a full data+CRC check. */
static void scan_block(fg_ftl_t *f, uint32_t blk, uint64_t floor, uint64_t *maxseq) {
    int have = 0;
    meta_t pend = {0, 0, 0, 0, 0, 0};
    uint32_t pend_pg = 0;
    f->st.mount_scanned_blocks++;
    for (uint32_t pg = 0; pg < f->ppb; pg++) {
        meta_t mt;
        int rc = read_page(f, blk, pg, NULL, &mt);
        f->st.mount_page_reads++;
        if (rc == RP_ERASED) break;
        if (have) { commit_scan(f, pend.lpn, blk * f->ppb + pend_pg, pend.seq); have = 0; }
        if (rc < 0 || mt.type != PT_DATA) continue;
        if (mt.seq > *maxseq) *maxseq = mt.seq;
        if (mt.seq <= floor) continue;
        pend = mt; pend_pg = pg; have = 1;
    }
    if (have) {
        meta_t mt;
        f->st.mount_page_reads++;
        f->st.mount_full_reads++;
#if FG_MUTANT == 2 /* fault-injection build: trust a possibly torn last page */
        (void)mt;
        if (1)
#else
        if (read_page(f, blk, pend_pg, f->dbuf, &mt) >= 0 && mt.seq == pend.seq)
#endif
            commit_scan(f, pend.lpn, blk * f->ppb + pend_pg, pend.seq);
        else
            f->st.mount_torn_pages++;
    }
}

static int load_ckpt(fg_ftl_t *f, uint64_t S, uint32_t *open_out) {
    uint32_t seen = 0, expect = 0;
    f->nck = 0;
    for (uint32_t b = 0; b < f->nblk; b++) {
        if (f->bstate[b] != T_CKPT || f->bseq0[b] != S) continue;
        if (f->nck < FG_CKPT_MAXBLK) f->ckblk[f->nck++] = b;
        for (uint32_t pg = 0; pg < f->ppb; pg++) {
            meta_t mt;
            int rc = read_page(f, b, pg, f->dbuf, &mt);
            f->st.mount_page_reads++;
            f->st.mount_full_reads++;
            if (rc == RP_ERASED) break;
            if (rc < 0 || mt.type != PT_CKPT || mt.seq != S) continue;
            ckpt_xfer(f, mt.lpn, f->dbuf, 1);
            seen++;
            if (mt.flags & F_LAST) expect = mt.lpn + 1;
        }
    }
    const uint8_t *h = f->ckhdr;
    if (!expect || seen != expect || fg_get32(h) != CK_MAGIC || fg_get32(h + 8) != f->cfg.nlpn ||
        fg_get32(h + 12) != f->nblk || fg_get64(h + 16) != S || fg_get32(h + 36) != expect)
        return 0;
    open_out[0] = fg_get32(h + 24); open_out[1] = fg_get32(h + 28); open_out[2] = fg_get32(h + 32);
    return 1;
}

int fg_mount(fg_ftl_t *f) {
    reset_ram(f);
    uint64_t maxseq = 0;
    uint32_t *ec0 = f->bmtime; /* scratch: erase count recorded in page 0 */
    /* pass 1: one spare-area read per block classifies it */
    for (uint32_t b = 0; b < f->nblk; b++) {
        meta_t mt;
        f->st.mount_page_reads++;
        if (factory_bad(f, b)) { f->bstate[b] = BS_BAD; ec0[b] = NONE; continue; }
        int rc = read_page(f, b, 0, NULL, &mt);
        ec0[b] = NONE;
        if (rc == RP_ERASED) { f->bstate[b] = T_EMPTY; continue; }
        if (rc < 0) { f->bstate[b] = T_UNKNOWN; continue; }
        f->bseq0[b] = mt.seq;
        ec0[b] = mt.ec;
        if (mt.seq > maxseq) maxseq = mt.seq;
        f->bstate[b] = mt.type == PT_CKPT ? T_CKPT : (mt.type == PT_DATA ? T_DATA : T_UNKNOWN);
    }
    /* newest complete checkpoint wins; incomplete ones (power cut mid-checkpoint) are skipped */
    uint64_t floor = 0, below = ~0ull;
    uint32_t ck_open[3] = {NONE, NONE, NONE};
    f->st.mount_used_ckpt = 0;
    for (;;) {
        uint64_t cand = 0;
        for (uint32_t b = 0; b < f->nblk; b++)
            if (f->bstate[b] == T_CKPT && f->bseq0[b] < below && f->bseq0[b] > cand) cand = f->bseq0[b];
        if (!cand) break;
        if (load_ckpt(f, cand, ck_open)) { floor = cand; f->st.mount_used_ckpt = 1; break; }
        for (uint32_t l = 0; l < f->cfg.nlpn; l++) f->l2p[l] = UNMAPPED;
        for (uint32_t b = 0; b < f->nblk; b++) f->bec[b] = 0;
        f->nck = 0;
        below = cand;
    }
    if (!f->st.mount_used_ckpt) for (uint32_t b = 0; b < f->nblk; b++) f->bec[b] = 0;
    /* pass 2: replay blocks written after the checkpoint, plus the ones it saw open */
    for (uint32_t b = 0; b < f->nblk; b++) {
        int open_at_ckpt = (b == ck_open[0] || b == ck_open[1] || b == ck_open[2]);
#if FG_MUTANT == 1 /* fault-injection build: forget blocks that were open at checkpoint time */
        open_at_ckpt = 0;
#endif
        if (f->bec[b] & BADBIT) continue; /* retired before the checkpoint: never scanned again */
        if ((f->bstate[b] == T_DATA && (f->bseq0[b] > floor || open_at_ckpt)) || f->bstate[b] == T_UNKNOWN)
            scan_block(f, b, floor, &maxseq);
    }
    /* rebuild validity and block states from the final map */
    for (uint32_t l = 0; l < f->cfg.nlpn; l++) {
        uint32_t p = f->l2p[l];
        if (p == UNMAPPED) continue;
        if (p / f->ppb >= f->nblk || f->bstate[p / f->ppb] == BS_BAD) { f->l2p[l] = UNMAPPED; continue; }
        vset(f, p);
    }
    uint64_t ecsum = 0, ecn = 0;
    for (uint32_t b = 0; b < f->nblk; b++) {
        uint32_t e = f->bec[b];
        int ckbad = (e & BADBIT) != 0;
        e &= ~BADBIT;
        if (ec0[b] != NONE && ec0[b] > e) e = ec0[b];
        f->bec[b] = e;
        if (f->bstate[b] == BS_BAD) { f->st.factory_bad++; continue; }
        if (ckbad && !f->bvalid[b]) { f->bstate[b] = BS_BAD; continue; }
        ecsum += e; ecn++;
        int is_ck = 0;
        for (uint32_t i = 0; i < f->nck; i++) if (f->ckblk[i] == b) is_ck = 1;
        if (is_ck) f->bstate[b] = BS_CKPT;
        else if (f->bvalid[b]) { f->bstate[b] = BS_FULL; f->bwp[b] = (uint16_t)f->ppb; }
        else { f->bstate[b] = BS_FREE; f->nfree++; }
    }
    if (f->st.mount_used_ckpt) {
        for (uint32_t b = 0; b < f->nblk; b++) f->bmtime[b] = 0;
    } else {
        uint32_t avg = ecn ? (uint32_t)(ecsum / ecn) : 0;
        for (uint32_t b = 0; b < f->nblk; b++) {
            if (f->bstate[b] == BS_FREE && ec0[b] == NONE) f->bec[b] = avg;
            f->bmtime[b] = 0;
        }
    }
    if (floor > maxseq) maxseq = floor;
    f->seq = maxseq + 1;
    return FG_OK;
}

uint32_t fg_free_blocks(const fg_ftl_t *f) { return f->nfree; }

int fg_check(const fg_ftl_t *f) {
    uint32_t mapped = 0, nfree = 0;
    for (uint32_t l = 0; l < f->cfg.nlpn; l++) {
        uint32_t p = f->l2p[l];
        if (p == UNMAPPED) continue;
        mapped++;
        if (!vtest(f, p)) return 1;
        uint8_t s = f->bstate[p / f->ppb];
        if (s != BS_OPEN && s != BS_FULL && s != BS_RETIRE) return 2;
    }
    uint64_t vsum = 0, bits = 0;
    for (uint32_t b = 0; b < f->nblk; b++) {
        vsum += f->bvalid[b];
        if (f->bstate[b] == BS_FREE) nfree++;
        uint32_t c = 0;
        for (uint32_t pg = 0; pg < f->ppb; pg++) c += (uint32_t)vtest(f, b * f->ppb + pg);
        if (c != f->bvalid[b]) return 3;
        bits += c;
    }
    if (vsum != mapped || bits != mapped) return 4;
    if (nfree != f->nfree) return 5;
    return 0;
}
