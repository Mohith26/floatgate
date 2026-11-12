/* Unit and integration tests. Built with AddressSanitizer + UBSan (see Makefile). */
#include <math.h>
#include "../fw/fg_crc32.h"
#include "../tools/rig.h"

static int g_run, g_fail;
#define CHECK(cond) do { if (!(cond)) { printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)
#define RUN(fn) do { g_run++; int _r = fn(); printf("%s %s\n", _r ? "FAIL" : "ok  ", #fn); if (_r) g_fail++; } while (0)

static uint64_t trng = 12345;
static uint32_t rnd(void) { trng = mix64(trng); return (uint32_t)(trng >> 16); }

/* ---------------------------------------------------------------- ECC */

static void flip(uint8_t *d, size_t len, uint8_t *p, int rbytes, uint32_t bit) {
    if (bit < len * 8) d[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7));
    else { bit -= (uint32_t)len * 8; (void)rbytes; p[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7)); }
}

static int bch_roundtrip(int t, size_t len, int trials) {
    fg_bch_t b;
    void *ram = malloc(fg_bch_ram_bytes(t));
    CHECK(fg_bch_init(&b, t, ram) == 0);
    uint8_t d[512], o[512], p[FG_BCH_MAX_PARITY], p0[FG_BCH_MAX_PARITY];
    for (int it = 0; it < trials; it++) {
        for (size_t i = 0; i < len; i++) d[i] = o[i] = (uint8_t)rnd();
        fg_bch_encode(&b, d, len, p);
        memcpy(p0, p, (size_t)b.rbytes);
        int w = it % (t + 1);
        uint32_t nbits = (uint32_t)len * 8u + (uint32_t)b.r, used[FG_BCH_TMAX];
        for (int k = 0; k < w; k++) {
            uint32_t bit;
            int dup;
            do { bit = rnd() % nbits; dup = 0; for (int j = 0; j < k; j++) dup |= used[j] == bit; } while (dup);
            used[k] = bit;
            flip(d, len, p, b.rbytes, bit);
        }
        int rc = fg_bch_decode(&b, d, len, p);
        CHECK(rc == w);
        CHECK(memcmp(d, o, len) == 0);
        CHECK(memcmp(p, p0, (size_t)b.rbytes) == 0);
    }
    free(ram);
    return 0;
}

static int test_bch8_sector_corrects_up_to_t(void) { return bch_roundtrip(8, 512, 900); }
static int test_bch16_sector_corrects_up_to_t(void) { return bch_roundtrip(16, 512, 500); }
static int test_bch8_short_codeword(void) { return bch_roundtrip(8, 28, 900); }

static int test_bch_beyond_t_is_flagged_or_caught(void) {
    /* t+1..t+4 errors: decoder must report failure or (rarely) miscorrect; it must never claim 0 errors */
    fg_bch_t b;
    void *ram = malloc(fg_bch_ram_bytes(8));
    CHECK(fg_bch_init(&b, 8, ram) == 0);
    uint8_t d[512], p[13];
    int flagged = 0;
    for (int it = 0; it < 400; it++) {
        for (int i = 0; i < 512; i++) d[i] = (uint8_t)rnd();
        fg_bch_encode(&b, d, 512, p);
        int w = 9 + it % 4;
        uint32_t used[16];
        for (int k = 0; k < w; k++) {
            uint32_t bit; int dup;
            do { bit = rnd() % (512 * 8 + 104); dup = 0; for (int j = 0; j < k; j++) dup |= used[j] == bit; } while (dup);
            used[k] = bit;
            flip(d, 512, p, 13, bit);
        }
        int rc = fg_bch_decode(&b, d, 512, p);
        CHECK(rc != 0);
        flagged += rc < 0;
    }
    CHECK(flagged >= 390);
    free(ram);
    return 0;
}

static int test_bch_rejects_bad_t(void) {
    fg_bch_t b;
    uint8_t ram[16];
    CHECK(fg_bch_init(&b, 5, ram) != 0);
    CHECK(fg_bch_init(&b, 0, ram) != 0);
    return 0;
}

static int test_crc32_known_vector(void) {
    uint32_t tab[256];
    fg_crc32_init(tab);
    CHECK(fg_crc32(tab, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    return 0;
}

/* ------------------------------------------------------------- simulator */

static int test_nand_program_once_and_in_order(void) {
    nandsim_cfg_t c = nandsim_default_cfg(8, 8, 64, 64);
    nandsim_t s;
    CHECK(nandsim_create(&s, &c) == 0);
    uint8_t d[64], o[64], rd[64], ro[64];
    memset(d, 0x5A, 64); memset(o, 0xFF, 64);
    CHECK(s.hal.program(s.hal.ctx, 1, 0, d, o) == FG_NAND_OK);
    CHECK(s.hal.program(s.hal.ctx, 1, 0, d, o) == FG_NAND_FAIL); /* reprogram */
    CHECK(s.hal.program(s.hal.ctx, 1, 3, d, o) == FG_NAND_OK);
    CHECK(s.hal.program(s.hal.ctx, 1, 2, d, o) == FG_NAND_FAIL); /* out of order */
    CHECK(s.st.violations == 2);
    s.hal.read(s.hal.ctx, 1, 0, rd, ro);
    CHECK(memcmp(rd, d, 64) == 0);
    CHECK(s.hal.erase(s.hal.ctx, 1) == FG_NAND_OK);
    s.hal.read(s.hal.ctx, 1, 0, rd, ro);
    CHECK(rd[0] == 0xFF && ro[5] == 0xFF);
    CHECK(s.hal.program(s.hal.ctx, 1, 0, d, o) == FG_NAND_OK);
    nandsim_destroy(&s);
    return 0;
}

static int test_nand_rber_grows_with_wear_and_retention(void) {
    nandsim_cfg_t c = nandsim_default_cfg(4, 4, 64, 64);
    c.rber_base = 1e-6; c.rber_wear = 1e-3; c.rber_retention = 1e-7;
    nandsim_t s;
    CHECK(nandsim_create(&s, &c) == 0);
    uint8_t d[64], o[64];
    memset(d, 0, 64); memset(o, 0, 64);
    s.hal.program(s.hal.ctx, 0, 0, d, o);
    double fresh = nandsim_rber(&s, 0, 0);
    s.ec[0] = 3000;
    double worn = nandsim_rber(&s, 0, 0);
    nandsim_age(&s, 1000);
    double aged = nandsim_rber(&s, 0, 0);
    CHECK(worn > fresh * 100);
    CHECK(aged > worn);
    nandsim_destroy(&s);
    return 0;
}

/* --------------------------------------------------------------- FTL */

static int mk(rig_t *r, uint32_t nblk, uint32_t ppb, uint32_t dsz, double fill, fg_cfg_t *out) {
    nandsim_cfg_t nc = nandsim_default_cfg(nblk, ppb, dsz, dsz >= 2048 ? 128 : 64);
    nc.factory_bad_frac = 0.03;
    fg_cfg_t fc = default_fcfg((uint32_t)(nblk * ppb * fill));
    if (out) fc = *out, fc.nlpn = (uint32_t)(nblk * ppb * fill);
    if (rig_open(r, &nc, &fc)) return -1;
    return fg_format(&r->ftl);
}

static int test_write_read_overwrite(void) {
    static rig_t r;
    CHECK(mk(&r, 32, 16, 512, 0.6, NULL) == 0);
    uint8_t b[512];
    CHECK(fg_read(&r.ftl, 3, b) == FG_ENOENT);
    payload_fill(b, 512, 3, 1);
    CHECK(fg_write(&r.ftl, 3, b) == FG_OK);
    payload_fill(b, 512, 3, 2);
    CHECK(fg_write(&r.ftl, 3, b) == FG_OK);
    memset(b, 0, 512);
    CHECK(fg_read(&r.ftl, 3, b) == FG_OK);
    CHECK(payload_check(b, 512, 3) == 2);
    CHECK(fg_write(&r.ftl, r.ftl.cfg.nlpn, b) == FG_EINVAL);
    CHECK(fg_check(&r.ftl) == 0);
    rig_close(&r);
    return 0;
}

static int churn(rig_t *r, uint32_t dsz, uint64_t n, uint32_t *ver) {
    uint8_t b[2048];
    uint32_t nlpn = r->ftl.cfg.nlpn;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t l = (rnd() & 3) ? rnd() % (nlpn / 8 + 1) : rnd() % nlpn;
        payload_fill(b, dsz, l, ++ver[l]);
        if (fg_write(&r->ftl, l, b) != FG_OK) return 1;
    }
    for (uint32_t l = 0; l < nlpn; l++) {
        int rc = fg_read(&r->ftl, l, b);
        if (ver[l] == 0) { if (rc != FG_ENOENT) return 2; continue; }
        if (rc != FG_OK || payload_check(b, dsz, l) != (int64_t)ver[l]) return 3;
    }
    return fg_check(&r->ftl) ? 4 : 0;
}

static int gc_policy_case(int policy) {
    static rig_t r;
    fg_cfg_t c = default_fcfg(0);
    c.gc_policy = policy;
    CHECK(mk(&r, 64, 32, 512, 0.75, &c) == 0);
    uint32_t *ver = calloc(r.ftl.cfg.nlpn, 4);
    CHECK(churn(&r, 512, 20000, ver) == 0);
    CHECK(r.ftl.st.gc_runs > 100);
    CHECK(r.nand.st.violations == 0);
    free(ver);
    rig_close(&r);
    return 0;
}
static int test_gc_greedy_preserves_data(void) { return gc_policy_case(FG_GC_GREEDY); }
static int test_gc_cost_benefit_preserves_data(void) { return gc_policy_case(FG_GC_COST_BENEFIT); }

static int test_clean_remount_with_and_without_checkpoint(void) {
    for (int ck = 0; ck < 2; ck++) {
        static rig_t r;
        fg_cfg_t c = default_fcfg(0);
        c.ckpt_interval = ck ? 200 : 0;
        CHECK(mk(&r, 64, 32, 2048, 0.7, &c) == 0);
        uint32_t *ver = calloc(r.ftl.cfg.nlpn, 4);
        CHECK(churn(&r, 2048, 5000, ver) == 0);
        CHECK(rig_reboot(&r) == FG_OK);
        CHECK((int)r.ftl.st.mount_used_ckpt == ck);
        CHECK(churn(&r, 2048, 0, ver) == 0);
        CHECK(churn(&r, 2048, 3000, ver) == 0); /* keeps working after mount */
        free(ver);
        rig_close(&r);
    }
    return 0;
}

static int test_checkpoint_reduces_mount_reads(void) {
    uint64_t reads[2];
    for (int ck = 0; ck < 2; ck++) {
        static rig_t r;
        fg_cfg_t c = default_fcfg(0);
        c.ckpt_interval = ck ? 256 : 0;
        CHECK(mk(&r, 128, 64, 2048, 0.7, &c) == 0);
        uint32_t *ver = calloc(r.ftl.cfg.nlpn, 4);
        CHECK(churn(&r, 2048, 12000, ver) == 0);
        uint64_t r0 = r.nand.st.reads + r.nand.st.oob_reads;
        CHECK(rig_reboot(&r) == FG_OK);
        reads[ck] = r.nand.st.reads + r.nand.st.oob_reads - r0;
        CHECK(churn(&r, 2048, 0, ver) == 0);
        free(ver);
        rig_close(&r);
    }
    CHECK(reads[1] * 4 < reads[0]);
    return 0;
}

static int cut_run(int filter, uint32_t cuts, uint32_t ckpt) {
    static rig_t r;
    fg_cfg_t c = default_fcfg(0);
    c.ckpt_interval = ckpt;
    CHECK(mk(&r, 64, 32, 2048, 0.7, &c) == 0);
    uint32_t nlpn = r.ftl.cfg.nlpn;
    static uint32_t *acked, *next;
    acked = calloc(nlpn, 4); next = calloc(nlpn, 4);
    static uint8_t b[2048];
    static volatile uint32_t fl, fv;
    jmp_buf env;
    r.nand.cut_filter = filter;
    for (uint32_t k = 0; k < cuts; k++) {
        nandsim_arm_cut(&r.nand, 1 + rnd() % (filter == 2 ? 6 : 500), &env);
        if (setjmp(env) == 0) {
            for (;;) {
                uint32_t l = rnd() % nlpn;
                payload_fill(b, 2048, l, ++next[l]);
                fl = l; fv = next[l];
                if (fg_write(&r.ftl, l, b) == FG_OK) acked[l] = next[l];
                fl = 0xFFFFFFFFu;
            }
        }
        nandsim_disarm(&r.nand);
        CHECK(rig_reboot(&r) == FG_OK);
        CHECK(fg_check(&r.ftl) == 0);
        for (uint32_t l = 0; l < nlpn; l++) {
            int rc = fg_read(&r.ftl, l, b);
            int64_t v = rc == FG_OK ? payload_check(b, 2048, l) : (rc == FG_ENOENT ? 0 : -1);
            if (l == fl && v == (int64_t)fv) { acked[l] = fv; continue; }
            CHECK(v == (int64_t)acked[l]);
        }
    }
    CHECK(r.nand.st.violations == 0);
    free(acked); free(next);
    rig_close(&r);
    return 0;
}
static int test_power_cut_torn_programs(void) { return cut_run(1, 120, 64); }
static int test_power_cut_partial_erases(void) { return cut_run(2, 120, 64); }
static int test_power_cut_without_checkpoint(void) { return cut_run(0, 120, 0); }

static int test_cut_during_checkpoint_falls_back(void) {
    /* checkpoint every 8 writes with a 48-page map: most cuts land inside a checkpoint */
    return cut_run(1, 150, 8);
}

static int test_program_and_erase_failures_retire_blocks(void) {
    static rig_t r;
    nandsim_cfg_t nc = nandsim_default_cfg(64, 32, 512, 64);
    nc.fail_prog_base = 0.0005;
    nc.fail_erase_base = 0.005;
    fg_cfg_t fc = default_fcfg(64 * 32 / 2);
    CHECK(rig_open(&r, &nc, &fc) == 0);
    CHECK(fg_format(&r.ftl) == 0);
    uint32_t *ver = calloc(fc.nlpn, 4);
    CHECK(churn(&r, 512, 12000, ver) == 0);
    CHECK(r.ftl.st.program_fails > 0);
    CHECK(r.ftl.st.erase_fails > 0);
    CHECK(r.ftl.st.grown_bad >= r.ftl.st.erase_fails);
    free(ver);
    rig_close(&r);
    return 0;
}

static int test_factory_bad_blocks_never_used(void) {
    static rig_t r;
    nandsim_cfg_t nc = nandsim_default_cfg(64, 16, 512, 64);
    nc.factory_bad_frac = 0.1;
    fg_cfg_t fc = default_fcfg(64 * 16 / 2);
    CHECK(rig_open(&r, &nc, &fc) == 0);
    CHECK(fg_format(&r.ftl) == 0);
    CHECK(r.ftl.st.factory_bad > 0);
    uint32_t *ver = calloc(fc.nlpn, 4);
    CHECK(churn(&r, 512, 6000, ver) == 0);
    for (uint32_t b = 0; b < 64; b++)
        if (r.ftl.bstate[b] == 3) CHECK(r.nand.ec[b] == 0 || r.ftl.st.erase_fails);
    CHECK(r.nand.st.violations == 0);
    free(ver);
    rig_close(&r);
    return 0;
}

static int test_no_ecc_detects_instead_of_returning_bad_data(void) {
    static rig_t r;
    fg_cfg_t c = default_fcfg(0);
    c.ecc_t = 0;
    CHECK(mk(&r, 32, 16, 512, 0.5, &c) == 0);
    uint8_t b[512];
    for (uint32_t l = 0; l < r.ftl.cfg.nlpn; l++) { payload_fill(b, 512, l, 1); CHECK(fg_write(&r.ftl, l, b) == FG_OK); }
    r.nand.cfg.rber_base = 2e-4;
    int eio = 0;
    for (uint32_t l = 0; l < r.ftl.cfg.nlpn; l++) {
        int rc = fg_read(&r.ftl, l, b);
        if (rc == FG_EIO) eio++;
        else CHECK(rc == FG_OK && payload_check(b, 512, l) == 1);
    }
    CHECK(eio > 0);
    rig_close(&r);
    return 0;
}

static int test_ecc_corrects_through_read_path(void) {
    static rig_t r;
    CHECK(mk(&r, 32, 16, 2048, 0.5, NULL) == 0);
    uint8_t b[2048];
    for (uint32_t l = 0; l < r.ftl.cfg.nlpn; l++) { payload_fill(b, 2048, l, 1); CHECK(fg_write(&r.ftl, l, b) == FG_OK); }
    r.nand.cfg.rber_base = 2e-4;
    for (int p = 0; p < 3; p++)
        for (uint32_t l = 0; l < r.ftl.cfg.nlpn; l++) {
            CHECK(fg_read(&r.ftl, l, b) == FG_OK);
            CHECK(payload_check(b, 2048, l) == 1);
        }
    CHECK(r.ftl.st.ecc_corrected_bits > 100);
    rig_close(&r);
    return 0;
}

static double wear_ratio(int wl_dynamic, int wl_static) {
    static rig_t r;
    fg_cfg_t c = default_fcfg(0);
    c.wl_dynamic = wl_dynamic; c.wl_static = wl_static; c.wl_threshold = 16;
    if (mk(&r, 64, 16, 512, 0.7, &c)) return -1;
    uint32_t nlpn = r.ftl.cfg.nlpn;
    uint8_t b[512];
    for (uint32_t l = 0; l < nlpn; l++) { payload_fill(b, 512, l, 1); fg_write(&r.ftl, l, b); }
    for (int i = 0; i < 60000; i++) { uint32_t l = rnd() % (nlpn / 10); payload_fill(b, 512, l, 2); fg_write(&r.ftl, l, b); }
    double sum = 0, mx = 0; int n = 0;
    for (uint32_t k = 0; k < 64; k++) { if (r.ftl.bstate[k] == 3) continue; sum += r.nand.ec[k]; n++; if (r.nand.ec[k] > mx) mx = r.nand.ec[k]; }
    rig_close(&r);
    return mx / (sum / n);
}

static int test_static_wear_leveling_narrows_spread(void) {
    double none = wear_ratio(0, 0), stat = wear_ratio(1, 1);
    CHECK(none > 0 && stat > 0);
    CHECK(stat < none);
    return 0;
}

static int test_deterministic_given_seed(void) {
    uint64_t a[2];
    for (int k = 0; k < 2; k++) {
        trng = 777;
        static rig_t r;
        CHECK(mk(&r, 32, 16, 512, 0.6, NULL) == 0);
        uint32_t *ver = calloc(r.ftl.cfg.nlpn, 4);
        CHECK(churn(&r, 512, 4000, ver) == 0);
        a[k] = r.nand.st.programs * 1000003u + r.nand.st.erases;
        free(ver);
        rig_close(&r);
    }
    CHECK(a[0] == a[1]);
    return 0;
}

static int test_arena_too_small_is_rejected(void) {
    nandsim_cfg_t nc = nandsim_default_cfg(16, 16, 512, 64);
    nandsim_t s;
    CHECK(nandsim_create(&s, &nc) == 0);
    fg_cfg_t fc = default_fcfg(100);
    size_t need = fg_ram_bytes(&fc, &s.hal);
    void *ram = malloc(need);
    fg_ftl_t f;
    CHECK(fg_init(&f, &fc, &s.hal, ram, need / 2) != FG_OK);
    CHECK(fg_init(&f, &fc, &s.hal, ram, need) == FG_OK);
    fc.nlpn = 16 * 16; /* no room left for garbage collection */
    CHECK(fg_init(&f, &fc, &s.hal, ram, need) != FG_OK);
    free(ram);
    nandsim_destroy(&s);
    return 0;
}

int main(void) {
    RUN(test_crc32_known_vector);
    RUN(test_bch8_sector_corrects_up_to_t);
    RUN(test_bch16_sector_corrects_up_to_t);
    RUN(test_bch8_short_codeword);
    RUN(test_bch_beyond_t_is_flagged_or_caught);
    RUN(test_bch_rejects_bad_t);
    RUN(test_nand_program_once_and_in_order);
    RUN(test_nand_rber_grows_with_wear_and_retention);
    RUN(test_write_read_overwrite);
    RUN(test_gc_greedy_preserves_data);
    RUN(test_gc_cost_benefit_preserves_data);
    RUN(test_clean_remount_with_and_without_checkpoint);
    RUN(test_checkpoint_reduces_mount_reads);
    RUN(test_power_cut_torn_programs);
    RUN(test_power_cut_partial_erases);
    RUN(test_power_cut_without_checkpoint);
    RUN(test_cut_during_checkpoint_falls_back);
    RUN(test_program_and_erase_failures_retire_blocks);
    RUN(test_factory_bad_blocks_never_used);
    RUN(test_no_ecc_detects_instead_of_returning_bad_data);
    RUN(test_ecc_corrects_through_read_path);
    RUN(test_static_wear_leveling_narrows_spread);
    RUN(test_deterministic_given_seed);
    RUN(test_arena_too_small_is_rejected);
    printf("%d/%d tests passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
