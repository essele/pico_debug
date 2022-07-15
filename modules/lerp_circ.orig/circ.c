/**
 * @file circ.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-03-31
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include "lerp/circ.h"
#include "pico/printf.h"

void circ_init(struct circ *c, uint8_t *data, int len) {
    c->data = data;
    c->end = data + len;
    c->head = c->tail = c->data;
    c->last = 0;
    c->size = len;
    c->flush = 0;
}



static void _circ_out(char ch, void *arg) {
    struct circ *c = (struct circ *)arg;

    circ_add_byte(c, ch);
}

/**
 * @brief printf to a circ buffer, will discard the oldest data if full
 * 
 * @param c 
 * @param format 
 * @param ... 
 * @return int 
 */
int circ_printf(struct circ *c, char *format, ...) {
    int len;
    va_list args;

    va_start(args, format);
    len = vfctprintf(_circ_out, (void *)c, format, args);
    va_end(args);
    return len;
}

/**
 * @brief printf to a circ buffer, using a va_list
 * 
 * @param c 
 * @param format 
 * @param va 
 * @return int 
 */
int circ_aprintf(struct circ *c, char *format, va_list va) {
    return vfctprintf(_circ_out, (void *)c, format, va);
}


