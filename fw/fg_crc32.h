#ifndef FG_CRC32_H
#define FG_CRC32_H
#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Table lives in caller-provided RAM. */
void fg_crc32_init(uint32_t table[256]);
uint32_t fg_crc32_update(const uint32_t table[256], uint32_t crc, const uint8_t *p, size_t n);
static inline uint32_t fg_crc32(const uint32_t table[256], const uint8_t *p, size_t n) {
    return fg_crc32_update(table, 0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}
#endif
