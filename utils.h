
#ifndef __UTILS_H
#define __UTILS_H

#include <stdint.h>

int hex_digit(char ch);
int hex_byte(char *packet);
uint32_t hex_word_le32(char *packet);

#endif
