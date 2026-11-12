/*
 * Binary BCH code over GF(2^13), shortened to the sector length.
 * Corrects up to t bit errors per codeword, the classic SLC/early-MLC NAND ECC
 * (e.g. "BCH-8 per 512 bytes"). Parity is m*t = 13*t bits; t must be a multiple
 * of 8 so the parity is a whole number of bytes (t = 8 -> 13 parity bytes).
 *
 * Decode pipeline: remainder of the received word mod g(x) (fast zero check),
 * syndromes evaluated from that remainder, Berlekamp-Massey for the error
 * locator, Chien search restricted to the shortened code length.
 */
#ifndef FG_BCH_H
#define FG_BCH_H
#include <stddef.h>
#include <stdint.h>

#define FG_BCH_M 13
#define FG_BCH_N 8191u /* 2^13 - 1 */
#define FG_BCH_TMAX 16
#define FG_BCH_MAX_PARITY ((FG_BCH_M * FG_BCH_TMAX) / 8)

typedef struct {
    int t;            /* correctable bits per codeword */
    int r;            /* parity bits = 13 * t */
    int rbytes;       /* parity bytes */
    uint16_t *exp_;   /* alpha^i, i in [0, 2N) */
    uint16_t *log_;   /* log_alpha(x), x in [1, N] */
    uint8_t *enc;     /* 256 * rbytes byte-wise LFSR table */
} fg_bch_t;

/* Bytes of RAM the code needs (tables). */
size_t fg_bch_ram_bytes(int t);
/* 0 on success. ram must be fg_bch_ram_bytes(t) bytes, 2-byte aligned. */
int fg_bch_init(fg_bch_t *b, int t, void *ram);
void fg_bch_encode(const fg_bch_t *b, const uint8_t *data, size_t len, uint8_t *parity);
/* Corrects data/parity in place. Returns number of corrected bits, or -1 if uncorrectable. */
int fg_bch_decode(const fg_bch_t *b, uint8_t *data, size_t len, uint8_t *parity);

#endif
