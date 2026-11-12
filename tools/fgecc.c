/*
 * fgecc: uncorrectable-read rate through the full firmware read path at a fixed
 * raw bit error rate, with BCH ECC on versus off (CRC-only detection).
 * 4 KiB pages + 224-byte spare, BCH over 512-byte sectors.
 */
#include <inttypes.h>
#include "rig.h"

int main(int argc, char **argv) {
    double rber = 1e-4;
    int t = 8;
    uint32_t passes = 4;
    uint64_t seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rber")) rber = atof(argv[++i]);
        else if (!strcmp(argv[i], "--t")) t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--passes")) passes = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], 0, 10);
    }
    uint32_t nblk = 64, ppb = 64, dsz = 4096, osz = 224;
    nandsim_cfg_t nc = nandsim_default_cfg(nblk, ppb, dsz, osz);
    nc.seed = seed;
    uint32_t nlpn = 3000;
    fg_cfg_t fc = default_fcfg(nlpn);
    fc.ecc_t = t;
    static rig_t r;
    if (rig_open(&r, &nc, &fc) || fg_format(&r.ftl)) { fprintf(stderr, "setup failed\n"); return 2; }
    uint8_t *buf = malloc(dsz);
    for (uint32_t l = 0; l < nlpn; l++) {
        payload_fill(buf, dsz, l, 1);
        if (fg_write(&r.ftl, l, buf)) { fprintf(stderr, "write failed\n"); return 3; }
    }
    r.nand.cfg.rber_base = rber; /* errors appear on reads from here on */
    uint64_t reads = 0, eio = 0, silent = 0, ok = 0;
    uint64_t flips0 = r.nand.st.bits_flipped;
    for (uint32_t p = 0; p < passes; p++)
        for (uint32_t l = 0; l < nlpn; l++) {
            int rc = fg_read(&r.ftl, l, buf);
            reads++;
            if (rc == FG_EIO) eio++;
            else if (rc == FG_OK && payload_check(buf, dsz, l) == 1) ok++;
            else silent++;
        }
    printf("{\"rber\": %g, \"ecc_t\": %d, \"page_bytes\": %u, \"spare_bytes\": %u, \"reads\": %" PRIu64 ", "
           "\"uncorrectable\": %" PRIu64 ", \"uncorrectable_rate\": %.6g, \"silent_corruption\": %" PRIu64 ", "
           "\"good\": %" PRIu64 ", \"raw_bits_flipped\": %" PRIu64 ", \"ecc_corrected_bits\": %" PRIu64 "}\n",
           rber, t, dsz, osz, reads, eio, (double)eio / (double)reads, silent, ok,
           r.nand.st.bits_flipped - flips0, r.ftl.st.ecc_corrected_bits);
    rig_close(&r);
    free(buf);
    return silent ? 1 : 0;
}
