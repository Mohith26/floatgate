/*
 * fgverify: host-side integrity checker for power-loss safety.
 *
 * Runs a random hot/cold workload against the firmware, cuts power at a random
 * NAND operation (including mid-program and mid-erase), reboots (wiping all
 * firmware RAM), remounts from flash alone and then reads back EVERY logical
 * page. Each page must hold exactly the last acknowledged version, or the one
 * write that was in flight when power died. Anything else is a lost or
 * corrupted acknowledged write. Output: one JSON object on stdout.
 */
#include <inttypes.h>
#include "rig.h"

typedef struct {
    uint32_t cuts, cycles_seed;
    uint64_t seed;
    double rber;
    int ckpt;
    double fail_prog, fail_erase;
} vargs_t;

int main(int argc, char **argv) {
    vargs_t a = {2000, 0, 1, 0.0, 256, 0.0, 0.0};
    uint32_t nblk = 128, ppb = 64, dsz = 2048, osz = 128, max_ops = 4000;
    const char *only = "any";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cuts")) a.cuts = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")) a.seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--rber")) a.rber = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ckpt")) a.ckpt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fail-prog")) a.fail_prog = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fail-erase")) a.fail_erase = atof(argv[++i]);
        else if (!strcmp(argv[i], "--blocks")) nblk = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-ops")) max_ops = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only")) only = argv[++i]; /* any | program | erase */
    }
    nandsim_cfg_t nc = nandsim_default_cfg(nblk, ppb, dsz, osz);
    nc.seed = a.seed;
    nc.factory_bad_frac = 0.02;
    nc.rber_base = a.rber;
    nc.fail_prog_base = a.fail_prog;
    nc.fail_erase_base = a.fail_erase;
    uint32_t nlpn = (uint32_t)((double)nblk * ppb * 0.70);
    fg_cfg_t fc = default_fcfg(nlpn);
    fc.ckpt_interval = (uint32_t)a.ckpt;
    fc.scrub_bits = a.rber > 0 ? 4 : 0;
    static rig_t r;
    if (rig_open(&r, &nc, &fc) || fg_format(&r.ftl)) { fprintf(stderr, "setup failed\n"); return 2; }
    r.nand.cut_filter = !strcmp(only, "program") ? 1 : (!strcmp(only, "erase") ? 2 : 0);
    if (r.nand.cut_filter) max_ops = r.nand.cut_filter == 1 ? 600 : 12;

    uint32_t *acked = calloc(nlpn, sizeof(uint32_t)); /* 0 = never acknowledged */
    uint32_t *next = calloc(nlpn, sizeof(uint32_t));
    uint8_t *buf = malloc(dsz);
    /* static: these are modified between setjmp and longjmp */
    static uint64_t rng, acked_writes, lost, corrupt, eio, mount_fail, inflight_landed;
    rng = a.seed * 7919u + 13u;
    uint64_t kinds[4] = {0, 0, 0, 0}, mount_reads_sum = 0, mount_reads_max = 0, verify_reads = 0, check_fail = 0;
    double mount_us_sum = 0;
    jmp_buf env;
    volatile uint32_t inflight_lpn = 0xFFFFFFFFu, inflight_ver = 0;

    for (uint32_t cut = 0; cut < a.cuts; cut++) {
        rng = mix64(rng);
        nandsim_arm_cut(&r.nand, (int64_t)(1 + rng % max_ops), &env);
        if (setjmp(env) == 0) {
            for (;;) { /* workload until the power cut fires */
                rng = mix64(rng);
                uint32_t lpn = (rng & 0xFF) < 204 ? (uint32_t)((rng >> 8) % (nlpn / 5))   /* 80% of ops on 20% of pages */
                                                  : (uint32_t)((rng >> 8) % nlpn);
                if (((rng >> 40) & 0xF) < 11) {
                    uint32_t v = ++next[lpn];
                    payload_fill(buf, dsz, lpn, v);
                    inflight_lpn = lpn; inflight_ver = v;
                    int rc = fg_write(&r.ftl, lpn, buf);
                    inflight_lpn = 0xFFFFFFFFu;
                    if (rc == FG_OK) { acked[lpn] = v; acked_writes++; }
                } else {
                    int rc = fg_read(&r.ftl, lpn, buf);
                    if (rc == FG_OK && payload_check(buf, dsz, lpn) != (int64_t)acked[lpn]) corrupt++;
                }
            }
        }
        kinds[r.nand.cut_kind & 3]++;
        nandsim_disarm(&r.nand);
        /* reboot: firmware RAM is gone, rebuild from flash */
        double busy0 = r.nand.st.busy_us;
        uint64_t rd0 = r.nand.st.reads + r.nand.st.oob_reads;
        if (rig_reboot(&r) != FG_OK) { mount_fail++; continue; }
        uint64_t mr = r.nand.st.reads + r.nand.st.oob_reads - rd0;
        mount_reads_sum += mr;
        if (mr > mount_reads_max) mount_reads_max = mr;
        mount_us_sum += r.nand.st.busy_us - busy0;
        if (fg_check(&r.ftl)) check_fail++;
        /* verify every logical page */
        for (uint32_t l = 0; l < nlpn; l++) {
            int rc = fg_read(&r.ftl, l, buf);
            verify_reads++;
            int64_t got = rc == FG_OK ? payload_check(buf, dsz, l) : (rc == FG_ENOENT ? 0 : -2);
            if (rc == FG_EIO) { eio++; continue; }
            if (got == (int64_t)acked[l]) continue;
            if (l == inflight_lpn && got == (int64_t)inflight_ver) { acked[l] = inflight_ver; inflight_landed++; continue; }
            if (got >= 0 && got < (int64_t)acked[l]) lost++;
            else corrupt++;
        }
        inflight_lpn = 0xFFFFFFFFu;
        /* an unacknowledged version number must not be reused */
    }
    printf("{\"cut_ops\": \"%s\", \"cuts\": %u, \"seed\": %" PRIu64 ", \"rber\": %g, \"ckpt_interval\": %d, \"logical_pages\": %u, "
           "\"acked_writes\": %" PRIu64 ", \"acked_data_failures\": %" PRIu64 ", \"lost_acked_writes\": %" PRIu64 ", \"corrupt_reads\": %" PRIu64 ", "
           "\"eio_reads\": %" PRIu64 ", \"mount_failures\": %" PRIu64 ", \"invariant_failures\": %" PRIu64 ", "
           "\"inflight_writes_persisted\": %" PRIu64 ", \"verify_reads\": %" PRIu64 ", "
           "\"cuts_before_op\": %" PRIu64 ", \"cuts_torn_program\": %" PRIu64 ", \"cuts_partial_erase\": %" PRIu64 ", \"cuts_during_read\": %" PRIu64 ", "
           "\"mount_page_reads_mean\": %.1f, \"mount_page_reads_max\": %" PRIu64 ", \"mount_model_ms_mean\": %.2f, "
           "\"grown_bad_blocks_final\": %" PRIu64 ", \"nand_violations\": %" PRIu64 ", \"nand_programs\": %" PRIu64 ", \"nand_erases\": %" PRIu64 "}\n",
           only, a.cuts, a.seed, a.rber, a.ckpt, nlpn, acked_writes, lost + corrupt + eio, lost, corrupt, eio, mount_fail, check_fail, inflight_landed,
           verify_reads, kinds[0], kinds[1], kinds[2], kinds[3], (double)mount_reads_sum / (a.cuts ? a.cuts : 1),
           mount_reads_max, mount_us_sum / 1000.0 / (a.cuts ? a.cuts : 1), r.ftl.st.grown_bad, r.nand.st.violations,
           r.nand.st.programs, r.nand.st.erases);
    free(acked); free(next); free(buf);
    rig_close(&r);
    return (lost || corrupt || eio || mount_fail || check_fail || r.nand.st.violations) ? 1 : 0;
}
