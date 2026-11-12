#include "fg_crc32.h"

void fg_crc32_init(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
}

uint32_t fg_crc32_update(const uint32_t table[256], uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}
