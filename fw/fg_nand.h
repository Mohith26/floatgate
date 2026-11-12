/*
 * NAND hardware abstraction the firmware is written against. On a real
 * controller these would issue ONFI commands (00h/30h read, 80h/10h program,
 * 60h/D0h erase) and poll the status register; here they are function
 * pointers so the same firmware runs against the simulator in sim/.
 */
#ifndef FG_NAND_H
#define FG_NAND_H
#include <stdint.h>

enum { FG_NAND_OK = 0, FG_NAND_FAIL = 1 /* status register FAIL bit set */ };

typedef struct fg_nand {
    uint32_t nblk;  /* erase blocks */
    uint32_t ppb;   /* pages per block (must be programmed in order) */
    uint32_t dsz;   /* data bytes per page */
    uint32_t osz;   /* spare (OOB) bytes per page */
    void *ctx;
    /* data may be NULL to transfer only the spare area */
    int (*read)(void *ctx, uint32_t blk, uint32_t pg, uint8_t *data, uint8_t *oob);
    int (*program)(void *ctx, uint32_t blk, uint32_t pg, const uint8_t *data, const uint8_t *oob);
    int (*erase)(void *ctx, uint32_t blk);
} fg_nand_t;

#endif
