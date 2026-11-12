#include "fg_bch.h"
#include "fg_util.h"

#define PRIM_POLY 0x201Bu /* x^13 + x^4 + x^3 + x + 1 */

size_t fg_bch_ram_bytes(int t) {
    int rbytes = (FG_BCH_M * t) / 8;
    return (size_t)(2 * FG_BCH_N) * 2 + (size_t)(FG_BCH_N + 1) * 2 + (size_t)256 * (size_t)rbytes;
}

static inline uint16_t gf_mul(const fg_bch_t *b, uint16_t x, uint16_t y) {
    if (!x || !y) return 0;
    return b->exp_[b->log_[x] + b->log_[y]];
}

int fg_bch_init(fg_bch_t *b, int t, void *ram) {
    if (t <= 0 || t > FG_BCH_TMAX || ((FG_BCH_M * t) % 8) != 0) return -1;
    b->t = t;
    b->r = FG_BCH_M * t;
    b->rbytes = b->r / 8;
    b->exp_ = (uint16_t *)ram;
    b->log_ = b->exp_ + 2 * FG_BCH_N;
    b->enc = (uint8_t *)(b->log_ + FG_BCH_N + 1);

    uint32_t x = 1;
    for (uint32_t i = 0; i < FG_BCH_N; i++) {
        b->exp_[i] = (uint16_t)x;
        b->exp_[i + FG_BCH_N] = (uint16_t)x;
        b->log_[x] = (uint16_t)i;
        x <<= 1;
        if (x & 0x2000u) x ^= PRIM_POLY;
    }
    b->log_[0] = 0;

    /* g(x) = lcm of minimal polynomials of alpha^1 .. alpha^2t, binary coefficients. */
    uint8_t g[FG_BCH_M * FG_BCH_TMAX + 1];
    int gdeg = 0;
    fg_memset(g, 0, sizeof g);
    g[0] = 1;
    uint8_t covered[2 * FG_BCH_TMAX + 1];
    fg_memset(covered, 0, sizeof covered);
    for (int i = 1; i <= 2 * t; i++) {
        if (covered[i]) continue;
        uint16_t mp[FG_BCH_M + 1]; /* minimal polynomial, GF(2^13) coefficients */
        int mdeg = 0;
        fg_memset(mp, 0, sizeof mp);
        mp[0] = 1;
        uint32_t e = (uint32_t)i;
        do {
            if (e <= (uint32_t)(2 * t)) covered[e] = 1;
            /* mp *= (x + alpha^e) */
            uint16_t root = b->exp_[e];
            for (int k = mdeg + 1; k >= 1; k--) mp[k] = (uint16_t)(mp[k - 1] ^ gf_mul(b, mp[k], root));
            mp[0] = gf_mul(b, mp[0], root);
            mdeg++;
            e = (e * 2u) % FG_BCH_N;
        } while (e != (uint32_t)i && mdeg <= FG_BCH_M);
        /* g *= mp (coefficients of a minimal polynomial are 0/1) */
        uint8_t ng[FG_BCH_M * FG_BCH_TMAX + 1];
        fg_memset(ng, 0, sizeof ng);
        for (int a = 0; a <= gdeg; a++)
            if (g[a])
                for (int c = 0; c <= mdeg; c++)
                    if (mp[c]) ng[a + c] ^= 1;
        gdeg += mdeg;
        fg_memcpy(g, ng, sizeof g);
    }
    if (gdeg != b->r) return -2;

    /* enc[v] = (v(x) * x^r) mod g(x), packed MSB-first (byte 0 bit 7 = x^(r-1)). */
    uint8_t coef[FG_BCH_M * FG_BCH_TMAX + 8];
    for (int v = 0; v < 256; v++) {
        fg_memset(coef, 0, sizeof coef);
        for (int j = 0; j < 8; j++) coef[b->r + j] = (uint8_t)((v >> j) & 1);
        for (int d = b->r + 7; d >= b->r; d--)
            if (coef[d])
                for (int k = 0; k <= b->r; k++) coef[d - b->r + k] ^= g[k];
        uint8_t *out = b->enc + (size_t)v * (size_t)b->rbytes;
        fg_memset(out, 0, (size_t)b->rbytes);
        for (int idx = 0; idx < b->r; idx++)
            if (coef[b->r - 1 - idx]) out[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7));
    }
    return 0;
}

static void lfsr(const fg_bch_t *b, const uint8_t *data, size_t len, uint8_t *reg) {
    const int rb = b->rbytes;
    fg_memset(reg, 0, (size_t)rb);
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(reg[0] ^ data[i]);
        const uint8_t *tv = b->enc + (size_t)idx * (size_t)rb;
        for (int k = 0; k < rb - 1; k++) reg[k] = (uint8_t)(reg[k + 1] ^ tv[k]);
        reg[rb - 1] = tv[rb - 1];
    }
}

void fg_bch_encode(const fg_bch_t *b, const uint8_t *data, size_t len, uint8_t *parity) {
    lfsr(b, data, len, parity);
}

int fg_bch_decode(const fg_bch_t *b, uint8_t *data, size_t len, uint8_t *parity) {
    const int t = b->t, r = b->r, rb = b->rbytes;
    uint8_t rem[FG_BCH_MAX_PARITY];
    lfsr(b, data, len, rem);
    int nz = 0;
    for (int k = 0; k < rb; k++) { rem[k] ^= parity[k]; nz |= rem[k]; }
    if (!nz) return 0;

    /* Syndromes S_j = rem(alpha^j); g(alpha^j) = 0 so this equals c(alpha^j). */
    uint16_t S[2 * FG_BCH_TMAX];
    fg_memset(S, 0, sizeof S);
    for (int idx = 0; idx < r; idx++) {
        if (!(rem[idx >> 3] & (0x80u >> (idx & 7)))) continue;
        uint32_t deg = (uint32_t)(r - 1 - idx);
        for (int j = 1; j <= 2 * t; j++) S[j - 1] ^= b->exp_[((uint32_t)j * deg) % FG_BCH_N];
    }

    /* Berlekamp-Massey. */
    uint16_t C[2 * FG_BCH_TMAX + 2], B[2 * FG_BCH_TMAX + 2], T[2 * FG_BCH_TMAX + 2];
    const int CL = 2 * FG_BCH_TMAX + 2;
    fg_memset(C, 0, sizeof C);
    fg_memset(B, 0, sizeof B);
    C[0] = B[0] = 1;
    int L = 0, m = 1;
    uint16_t bb = 1;
    for (int n = 0; n < 2 * t; n++) {
        uint16_t d = S[n];
        for (int i = 1; i <= L; i++) d ^= gf_mul(b, C[i], S[n - i]);
        if (!d) { m++; continue; }
        uint16_t coef = b->exp_[(b->log_[d] + FG_BCH_N - b->log_[bb]) % FG_BCH_N];
        if (2 * L <= n) {
            fg_memcpy(T, C, sizeof C);
            for (int i = 0; i + m < CL; i++) C[i + m] ^= gf_mul(b, coef, B[i]);
            L = n + 1 - L;
            fg_memcpy(B, T, sizeof B);
            bb = d;
            m = 1;
        } else {
            for (int i = 0; i + m < CL; i++) C[i + m] ^= gf_mul(b, coef, B[i]);
            m++;
        }
    }
    if (L > t) return -1;
    for (int i = L + 1; i < CL; i++) if (C[i]) return -1;

    /* Chien search over the shortened length: error at degree e iff C(alpha^-e) = 0. */
    const uint32_t kbits = (uint32_t)len * 8u;
    const uint32_t nbits = kbits + (uint32_t)r;
    uint32_t pos[FG_BCH_TMAX];
    int found = 0;
    for (uint32_t e = 0; e < nbits && found <= L; e++) {
        uint16_t acc = C[0];
        for (int i = 1; i <= L; i++) {
            if (!C[i]) continue;
            uint32_t ex = (uint32_t)b->log_[C[i]] + (FG_BCH_N - ((e * (uint32_t)i) % FG_BCH_N));
            acc ^= b->exp_[ex % FG_BCH_N];
        }
        if (!acc) {
            if (found == L) return -1;
            pos[found++] = e;
        }
    }
    if (found != L) return -1;
    for (int i = 0; i < found; i++) {
        uint32_t e = pos[i];
        if (e >= (uint32_t)r) {
            uint32_t s = (uint32_t)r + kbits - 1u - e;
            data[s >> 3] ^= (uint8_t)(0x80u >> (s & 7));
        } else {
            uint32_t idx = (uint32_t)r - 1u - e;
            parity[idx >> 3] ^= (uint8_t)(0x80u >> (idx & 7));
        }
    }
    return found;
}
