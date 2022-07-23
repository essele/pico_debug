/**
 * @file tokeniser.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-03-21
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <stdio.h>
#include <strings.h>
#include <ctype.h>

#include "lerp/tokeniser.h"


static char token_str[MAX_TOK_SIZE];
static int  token_ints[6];


// Some helper routines to extract mac and ipaddresses in more useful forms
uint32_t token_ip_address() {
    return (token_ints[0] << 24) | (token_ints[1] << 16) | (token_ints[2] << 8) | (token_ints[3]);
}

char *token_mac_address() {
    for (int i=0; i < 6; i++) {
        token_str[i] = (char)token_ints[i];
    }
    return token_str;
}

char *token_string() {
    return token_str;
}

int token_int() {
    return token_ints[0];
}


static const char hex_digits[] = "0123456789abcdef";
static const char digits[] = "0123456789";
static const char valid_chars[] = "abcdefghijklmnopqrstuvwxyz0123456789_.";

static int get_int(uint8_t **ptr, int base) {
    uint8_t *p = *ptr;
    int     rc = 0;

    if (!*p) return -1;     // we need at least one char
    while (*p) {
        char *x = index(hex_digits, tolower(*p));
        if (!x) break;
        int v = (int)(x - hex_digits);
        rc *= base;
        rc += v;
        p++;
    }
    *ptr = p;
    return rc;
}

/**
 * @brief Make sure we are at the end of the input without overwriting the last token
 * 
 * Returns 1 if there are no more tokens, or 0 if we're not at the end
 * 
 * @param ptr 
 * @return int 
 */
int token_is_last(struct circ *circ) {
    uint8_t *p = circ->tail;

    while(*p == ' ' || *p == '\t') p++;

    if (!*p) return 1;
    return 0;
}


int token_get(struct circ *circ) {
    uint8_t *p = circ->tail;

    // First we get past any whitespace
    while(*p == ' ' || *p == '\t') p++;

    // Are we at the end?
    if (!*p) return TOK_END;

    // Simple case of "="
    if (*p == '=') {
        circ->tail = p + 1;
        return TOK_EQUALS;
    }
    if (*p == ',') {
        circ->tail = p + 1;
        return TOK_COMMA;
    }


    // ------------------------------------------------------------------------------
    // If we have a quote then it should be a quoted string...
    // ------------------------------------------------------------------------------
    if (*p == '\"') {
        int esc = 0;
        int i = 0;

        p++;
        while(*p) {
            if (*p == '\\') {
                p++;
                esc = 1;
                continue;
            }
            if (*p == '\"' && !esc) {
                circ->tail = p + 1;
                token_str[i] = '\0';
                return TOK_STRING;
            }
            token_str[i++] = *p;
            esc = 0;
            p++;
        }
        return TOK_ERROR;           // no terminating quote
    }

    // ------------------------------------------------------------------------------
    // Simple Hex number... i.e. 0x12345
    // ------------------------------------------------------------------------------
    if (*p == '0' && *(p+1) == 'x') {
//        *ptr = p + 2;
        p += 2;
        token_ints[0] = get_int(&p, 16);
        if (token_ints[0] == -1) return TOK_ERROR;
        circ->tail = p;
        return TOK_INTEGER;
    }

    // ------------------------------------------------------------------------------
    // See if we are a mac address ... if we start with a hex digit, then look for
    // six hex numbers (0-255) separated by colons.
    // ------------------------------------------------------------------------------
    if (index(hex_digits, tolower(*p))) {
        uint8_t *new_ptr = p;
        int i = 0;
        while(1) {
            int v = get_int(&new_ptr, 16);
            if (v >= 0 && v < 256) {
                token_ints[i] = v;
                i++;
            } else {
                // Not a mac address...
                break;
            }
            // Do we have a colon...
            if (i >= 6 || *new_ptr != ':') break;
            new_ptr++;
        };
        if (i == 6) {
//            *ptr = new_ptr;
            circ->tail = new_ptr;
            return TOK_MACADDR;
        }
    }

    // ------------------------------------------------------------------------------
    // See if we are an IP address ... if we start with a digit, then look for
    // four numbers (0-255) separated by dots.
    // ------------------------------------------------------------------------------
    if (index(digits, *p)) {
        uint8_t *new_ptr = p;
        int i = 0;
        while(1) {
            int v = get_int(&new_ptr, 10);
            if (v >= 0 && v < 256) {
                token_ints[i] = v;
                i++;
            } else {
                // Not an IP address...
                break;
            }
            // Do we have a dot...
            if (i >= 4 || *new_ptr != '.') break;
            new_ptr++;
        };
        if (i == 4) {
//            *ptr = new_ptr;
            circ->tail = new_ptr;
            return TOK_IPADDR;
        }
    }

    // ------------------------------------------------------------------------------
    // If we match a number at this point, then we're probably an ordinary integer
    // otherwise we must be an error... but there are three variants ...
    // base number, +number, and -number
    // ------------------------------------------------------------------------------
    int is_digit = 0;

    if (*p == '+' && index(digits, *(p+1))) { is_digit = 1; p++; }
    else if (*p == '-' && index(digits, *(p+1))) { is_digit = -1; p++; }
    else if (index(digits, *p)) { is_digit = 1; }
    
    if (is_digit) {
        token_ints[0] = get_int(&p, 10);
        if (token_ints[0] == -1) return TOK_ERROR;
        token_ints[0] *= is_digit;      // handle negatives
        circ->tail = p;
        return TOK_INTEGER;
    }

    // ------------------------------------------------------------------------------
    // If we get here, then we are most likely some kind of string, either a command
    // or an identified (config item) so we just eat anything that isn't a separator
    // ------------------------------------------------------------------------------
    if (index(valid_chars, tolower(*p))) {
        int i=0;
        while(*p && index(valid_chars, tolower(*p))) {
            token_str[i++] = *p++;
            if (i == MAX_TOK_SIZE) {
                return TOK_ERROR;
            }
        }
        token_str[i] = '\0';
        circ->tail = p;
        return TOK_WORD;
    }

    // ------------------------------------------------------------------------------
    // Some specfic tokens we care about...
    // ------------------------------------------------------------------------------
    if (*p == ':') {
        circ->tail = p + 1;
        return TOK_COLON;
    }
    if (*p == '+') {
        circ->tail = p + 1;
        return TOK_PLUS;
    }
    if (*p == '-') {
        circ->tail = p + 1;
        return TOK_MINUS;
    }

    // ------------------------------------------------------------------------------
    // Theoretically anything here is a separator of some kind...
    // ------------------------------------------------------------------------------
    circ->tail = p;
    return TOK_ERROR;
}



