/*
 * fgtrace: replays a block I/O trace (or a synthetic workload) through the
 * firmware and reports write amplification, wear spread and read-back integrity.
 *
 * Trace format (from scripts/prep_trace.py): "FGTR", u32 nlpn, u64 nops, then
 * nops u32 records; bit 31 set = 4 KiB page write, clear = read; low bits = dense LPN.
 * The device is preconditioned by writing every logical page once, then the
 * trace is replayed; WA counts only the replay phase.
 */
#include <inttypes.h>
#include <math.h>
#include "rig.h"

int main(int argc, char **argv) {
    const char *trace = NULL, *policy = "cb", *wl = "static";
    double op = 0.07, hot_ops = 0.9, hot_space = 0.1, rber = 0.0;
    uint32_t ppb = 256, dsz = 64, osz = 64, syn_lpn = 0, ecc = 8;
    uint64_t syn_writes = 0, seed = 1;
    uint32_t wl_threshold = 64;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace = argv[++i];
        else if (!strcmp(argv[i], "--policy")) policy = argv[++i];
        else if (!strcmp(argv[i], "--wl")) wl = argv[++i];
        else if (!strcmp(argv[i], "--op")) op = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ppb")) ppb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ecc")) ecc = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--wl-threshold")) wl_threshold = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--synthetic")) { /* nlpn,writes,hot_ops,hot_space */
            char *s = argv[++i];
            syn_lpn = (uint32_t)strtoul(s, &s, 10); s++;
            syn_writes = strtoull(s, &s, 10); s++;
            hot_ops = strtod(s, &s); s++;
            hot_space = strtod(s, &s);
        }
    }
    uint32_t nlpn;
    uint64_t nops;
    uint32_t *ops = NULL;
    if (trace) {
        FILE *fp = fopen(trace, "rb");
        char mg[4];
        if (!fp || fread(mg, 1, 4, fp) != 4 || memcmp(mg, "FGTR", 4) || fread(&nlpn, 4, 1, fp) != 1 || fread(&nops, 8, 1, fp) != 1) {
            fprintf(stderr, "bad trace %s\n", trace);
            return 2;
        }
        ops = malloc(nops * 4);
        if (fread(ops, 4, nops, fp) != nops) { fprintf(stderr, "short trace\n"); return 2; }
        fclose(fp);
    } else {
        nlpn = syn_lpn;
        nops = syn_writes;
    }
    uint32_t phys_pages = (uint32_t)ceil((double)nlpn / (1.0 - op));
    uint32_t nblk = (phys_pages + ppb - 1) / ppb + 4; /* +4: GC reserve headroom on top of the OP target */
    nandsim_cfg_t nc = nandsim_default_cfg(nblk, ppb, dsz, osz);
    nc.seed = seed;
    nc.rber_base = rber;
    fg_cfg_t fc = default_fcfg(nlpn);
    fc.ecc_t = (int)ecc;
    fc.gc_policy = !strcmp(policy, "greedy") ? FG_GC_GREEDY : FG_GC_COST_BENEFIT;
    fc.wl_dynamic = strcmp(wl, "none") != 0;
    fc.wl_static = !strcmp(wl, "static");
    fc.wl_threshold = wl_threshold;
    static rig_t r;
    int rc = rig_open(&r, &nc, &fc);
    if (rc || fg_format(&r.ftl)) { fprintf(stderr, "setup failed rc=%d\n", rc); return 2; }
    uint32_t *ver = calloc(nlpn, sizeof(uint32_t));
    uint8_t *buf = malloc(dsz);
    double t0 = now_ms();
    for (uint32_t l = 0; l < nlpn; l++) { /* precondition: fill the drive */
        payload_fill(buf, dsz, l, ++ver[l]);
        if (fg_write(&r.ftl, l, buf)) { fprintf(stderr, "precondition failed\n"); return 3; }
    }
    fg_stats_t s0 = r.ftl.st;
    uint64_t prog0 = r.nand.st.programs, erase0 = r.nand.st.erases;
    uint64_t writes = 0, reads = 0, mismatches = 0, errors = 0, rng = seed * 1000003u;
    uint32_t hot_n = (uint32_t)(nlpn * hot_space);
    if (hot_n == 0) hot_n = 1;
    for (uint64_t i = 0; i < nops; i++) {
        uint32_t lpn;
        int is_write;
        if (ops) { lpn = ops[i] & 0x7FFFFFFFu; is_write = (ops[i] >> 31) != 0; }
        else {
            rng = mix64(rng);
            double u = (double)(rng >> 11) / 9007199254740992.0;
            rng = mix64(rng);
            lpn = u < hot_ops ? (uint32_t)(rng % hot_n) : hot_n + (uint32_t)(rng % (nlpn - hot_n));
            is_write = 1;
        }
        if (lpn >= nlpn) continue;
        if (is_write) {
            payload_fill(buf, dsz, lpn, ++ver[lpn]);
            if (fg_write(&r.ftl, lpn, buf)) { errors++; break; }
            writes++;
        } else {
            reads++;
            if (fg_read(&r.ftl, lpn, buf) != FG_OK || payload_check(buf, dsz, lpn) != (int64_t)ver[lpn]) mismatches++;
        }
    }
    /* final full read-back */
    for (uint32_t l = 0; l < nlpn; l++)
        if (fg_read(&r.ftl, l, buf) != FG_OK || payload_check(buf, dsz, l) != (int64_t)ver[l]) mismatches++;
    double ms = now_ms() - t0;
    fg_stats_t *s = &r.ftl.st;
    uint64_t host = s->host_writes - s0.host_writes;
    uint64_t gcw = s->gc_writes - s0.gc_writes, wlw = s->wl_writes - s0.wl_writes;
    uint64_t prog = r.nand.st.programs - prog0;
    /* wear over good blocks, from the simulator's true erase counters */
    uint64_t sum = 0, n = 0;
    uint32_t mx = 0, mn = 0xFFFFFFFFu;
    for (uint32_t b = 0; b < nblk; b++) {
        if (r.ftl.bstate[b] == 3 /* BS_BAD */) continue;
        uint32_t e = r.nand.ec[b];
        sum += e; n++;
        if (e > mx) mx = e;
        if (e < mn) mn = e;
    }
    double mean = n ? (double)sum / n : 0, var = 0;
    for (uint32_t b = 0; b < nblk; b++) {
        if (r.ftl.bstate[b] == 3) continue;
        double d = r.nand.ec[b] - mean;
        var += d * d;
    }
    printf("{\"trace\": \"%s\", \"policy\": \"%s\", \"wl\": \"%s\", \"op\": %.3f, \"logical_pages\": %u, \"blocks\": %u, "
           "\"pages_per_block\": %u, \"replay_host_writes\": %" PRIu64 ", \"replay_reads\": %" PRIu64 ", "
           "\"gc_writes\": %" PRIu64 ", \"wl_writes\": %" PRIu64 ", \"nand_programs\": %" PRIu64 ", \"write_amplification\": %.4f, "
           "\"replay_erases\": %" PRIu64 ", \"erase_max\": %u, \"erase_min\": %u, \"erase_mean\": %.2f, \"erase_stdev\": %.2f, "
           "\"wear_spread_max_over_mean\": %.4f, \"readback_mismatches\": %" PRIu64 ", \"write_errors\": %" PRIu64 ", \"runtime_ms\": %.0f}\n",
           trace ? trace : "synthetic", policy, wl, op, nlpn, nblk, ppb, writes, reads, gcw, wlw, prog,
           host ? (double)prog / (double)host : 0.0, r.nand.st.erases - erase0, mx, mn, mean, sqrt(var / (n ? n : 1)),
           mean > 0 ? mx / mean : 0.0, mismatches, errors, ms);
    rig_close(&r);
    free(ver); free(buf); free(ops);
    return (mismatches || errors) ? 1 : 0;
}
