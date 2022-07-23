/**
 * @file tokeniser.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-03-21
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef __TOKENISER_H
#define __TOKENISER_H

#include "lerp/circ.h"
#include <stdint.h>


#define MAX_TOK_SIZE        128

enum tokens {
    TOK_ERROR = 0,
    TOK_END,
    TOK_EQUALS,
    TOK_COLON,
    TOK_COMMA,
    TOK_PLUS,
    TOK_MINUS,
    TOK_INTEGER,
    TOK_STRING,
    TOK_MACADDR,
    TOK_IPADDR,
    TOK_WORD,
};

int token_get(struct circ *);
int token_is_last(struct circ *);
uint32_t token_ip_address();
char *token_mac_address();
char *token_string();
int token_int();

#endif