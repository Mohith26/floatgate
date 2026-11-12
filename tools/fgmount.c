/*
 * fgmount: mount/recovery cost after an unclean shutdown, full out-of-band scan
 * versus checkpoint + delta scan. The drive is aged with random overwrites,
 * power is "cut" (firmware RAM discarded without any shutdown work) and the
 * mount is timed in NAND operations, modelled NAND busy time and host wall time.
 */
#include <inttypes.h>
#include "rig.h"

int main(int argc, char **argv) {
    uint32_t ckpt = 4096, nblk = 256, ppb = 128, dsz = 4096, osz = 224;
    uint64_t seed = 1;
    double overwrite_factor = 3.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ckpt")) ckpt = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--blocks")) nblk = (uint32_t)atoi(argv[++i]);
    }
    nandsim_cfg_t nc = nandsim_default_cfg(nblk, ppb, dsz, osz);
    nc.seed = seed;
    nc.factory_bad_frac = 0.02;
    uint32_t nlpn = (uint32_t)(nblk * ppb * 0.80);
    fg_cfg_t fc = default_fcfg(nlpn);
    fc.ckpt_interval = ckpt;
    static rig_t r;
    if (rig_open(&r, &nc, &fc) || fg_format(&r.ftl)) { fprintf(stderr, "setup failed\n"); return 2; }
    uint32_t *ver = calloc(nlpn, sizeof(uint32_t));
    uint8_t *buf = malloc(dsz);
    uint64_t rng = seed * 31337u;
    for (uint32_t l = 0; l < nlpn; l++) { payload_fill(buf, dsz, l, ++ver[l]); fg_write(&r.ftl, l, buf); }
    uint64_t n = (uint64_t)(nlpn * overwrite_factor);
    for (uint64_t i = 0; i < n; i++) {
        rng = mix64(rng);
        uint32_t l = (uint32_t)(rng % nlpn);
        payload_fill(buf, dsz, l, ++ver[l]);
        if (fg_write(&r.ftl, l, buf)) { fprintf(stderr, "write failed\n"); return 3; }
    }
    uint32_t writes_since_ckpt = r.ftl.since_ckpt;
    double ckpt_ratio = (double)r.ftl.st.ckpt_writes / (double)r.ftl.st.host_writes;
    uint64_t rd0 = r.nand.st.reads, oob0 = r.nand.st.oob_reads;
    double busy0 = r.nand.st.busy_us, t0 = now_ms();
    if (rig_reboot(&r)) { fprintf(stderr, "mount failed\n"); return 4; }
    double wall = now_ms() - t0;
    uint64_t full = r.nand.st.reads - rd0, oob = r.nand.st.oob_reads - oob0;
    double model_ms = (r.nand.st.busy_us - busy0) / 1000.0;
    uint64_t bad = 0;
    for (uint32_t l = 0; l < nlpn; l++)
        if (fg_read(&r.ftl, l, buf) != FG_OK || payload_check(buf, dsz, l) != (int64_t)ver[l]) bad++;
    printf("{\"ckpt_interval\": %u, \"blocks\": %u, \"pages_per_block\": %u, \"page_bytes\": %u, \"logical_pages\": %u, "
           "\"writes_since_last_ckpt\": %u, \"used_checkpoint\": %u, \"mount_spare_reads\": %" PRIu64 ", \"mount_full_reads\": %" PRIu64 ", "
           "\"mount_blocks_scanned\": %" PRIu64 ", \"ckpt_page_writes_per_host_write\": %.4f, \"mount_model_ms\": %.2f, \"mount_wall_ms\": %.2f, \"pages_wrong_after_mount\": %" PRIu64 "}\n",
           ckpt, nblk, ppb, dsz, nlpn, writes_since_ckpt, r.ftl.st.mount_used_ckpt, oob, full, r.ftl.st.mount_scanned_blocks, ckpt_ratio,
           model_ms, wall, bad);
    rig_close(&r);
    free(ver); free(buf);
    return bad ? 1 : 0;
}
