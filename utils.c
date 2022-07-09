
#include "utils.h"

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return (ch - '0');
    if (ch >= 'a' && ch <= 'f') return (ch - ('a'-10));
    if (ch >= 'A' && ch <= 'F') return (ch - ('A'-10));
    return -1;
}

int hex_byte(char *packet) {
    int rc;
    int v;

    v = hex_digit(*packet++);
    if (v == -1)
        return -1;
    rc = v << 4;
    v = hex_digit(*packet);
    if (v == -1)
        return -1;
    rc |= v;
    return rc;
}

/**
 * @brief Read in 8 chars and convert into a litte endian word
 *
 */
uint32_t hex_word_le32(char *packet) {
    uint32_t rc = 0;

    for (int i = 0; i < 4; i++) {
        rc >>= 8;
        int v = hex_byte(packet);
        if (v == -1)
            return 0xffffffff;
        packet += 2;
        rc |= (v << 24);
    }
    return rc;
}


