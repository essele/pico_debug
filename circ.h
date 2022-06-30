/**
 * @file circ.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-03-31
 * 
 * @copyright Copyright (c) 2022
 * 
 * Circular Buffer implementation
 * 
 */

#ifndef ___CIRC_H
#define ___CIRC_H

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "pico/stdlib.h"

struct circ {
    // "constants"
    uint8_t     *data;              // where is the data
    uint8_t     *end;               // position of the end of the buffer (one past)
    uint32_t    size;               // keep the size here for calcs

    // variables
    uint8_t     *head;              // where do we write to
    uint8_t     *tail;              // where to we read from
    
    // Flags
    int         flush;              // flag to signal interactive flush needed
    int         full;               // are we full?
    int         last;               // flag to signal the last byte in the stream is "last"
};

/**
 * @brief Define and initialise a fixed size circular buffer of a given name
 * 
 */
#define CIRC_DEFINE(name, sz)           static uint8_t _c_data_##name[sz]; \
                                        static struct circ _c_circ_##name = { \
                                            .data = _c_data_##name, \
                                            .end = _c_data_##name + sz, \
                                            .size = sz, \
                                            .head = _c_data_##name, \
                                            .tail = _c_data_##name, \
                                            .full = 0, .flush = 0, .last = 0 }; \
                                        struct circ *name = &_c_circ_##name;


/**
 * @brief Is the circular buffer empty?
 * 
 * @param c 
 * @return int 
 */
static inline int circ_is_empty(struct circ *c) {
    return (!c->full && (c->head == c->tail));
}

static inline int circ_is_full(struct circ *c) {
    return c->full;
}

static inline int circ_is_empty_notfull(struct circ *c) {
    return c->head == c->tail;
}

static inline int circ_has_data(struct circ *c) {
    return (c->full || (c->head != c->tail));
}

static inline int circ_space(struct circ *c) {
    if (c->full) return 0;
    return ((c->tail > c->head) ? (c->tail - c->head) : (c->size - (c->head - c->tail)));
}

static inline int circ_space_before_wrap(struct circ *c) {
    return MIN(circ_space(c), c->end - c->head);
}

static inline void circ_advance_head(struct circ *c, int count) {
    c->head += count;
    if (c->head >= c->end) c->head -= c->size;
    c->full = (c->head == c->tail);
}

static inline int circ_used(struct circ *c) {
    if (c->full) return c->size;
    return ((c->tail > c->head) ? (c->size - (c->tail - c->head)) : (c->head - c->tail));
}

static inline int circ_bytes_before_wrap(struct circ *c) {
    return MIN(circ_used(c), c->end - c->tail);
}
static inline int circ_bytes_after_wrap(struct circ *c) {
    if (c->head < c->tail) {
        return c->head - c->data;
    } else return 0;
}

static inline void circ_advance_tail(struct circ *c, int count) {
    c->tail += count;
    if (c->tail >= c->end) c->tail -= c->size;
    c->full = 0;
}

/**
 * @brief Add a byte to a circ, move tail on if already full
 * 
 * This means we will lose the oldest data if we overflow the
 * buffer, which is probably the right approach.
 * 
 * @param c 
 * @param b 
 */
static inline void circ_add_byte(struct circ *c, uint8_t byte) {
    if (c->full) {
        if (++(c->tail) == c->end) c->tail = c->data;
    }
    *(c->head++) = byte;
    if (c->head == c->end) c->head = c->data;
    c->full = (c->head == c->tail);
}

/**
 * @brief Add a set of bytes to the circular buffer
 * 
 * If there's not enough space then we simulate the effect of
 * overriting as per add_byte.
 * 
 * @param c 
 * @param data 
 * @param len 
 */
static inline void circ_add_bytes(struct circ *c, uint8_t *data, int len) {
    int remaining, xfer, left;

    while (len) {
        remaining = c->end - c->head;       // space from head until the end of the buffer
        xfer = MIN(len, remaining);         // how much can we copy this time around?
        left = circ_space(c) - xfer;        // how much space will be left after this?

        memcpy(c->head, data, xfer);
        c->head += xfer; if (c->head >= c->end) c->head -= c->size;

        // Did we overflow the size? (double negative)
        if (left <= 0) {
            c->tail -= left; if (c->tail >= c->end) c->tail -= c->size;
            c->full = 1;
        }
        len -= xfer;
        data += xfer;
    }
}

/**
 * @brief Move the used data from one circ and append to the other
 * 
 * Note: no capacity checking is done, it needs to fit!
 * 
 * @param dst 
 * @param src 
 */
static inline int circ_move(struct circ *dst, struct circ *src) {
    int size;
    int rc = 0;

    size = circ_bytes_before_wrap(src);
    if (size) {
        circ_add_bytes(dst, src->tail, size);
        circ_advance_tail(src, size);
        rc += size;
    }
    size = circ_bytes_before_wrap(src);
    if (size) {
        circ_add_bytes(dst, src->tail, size);
        circ_advance_tail(src, size);
        rc += size;
    }
    return rc;
}


/**
 * @brief Get a byte from the circular buffer, return -1 if none available
 * 
 * @param c 
 * @return int 
 */
static inline int circ_get_byte(struct circ *c) {
    int rc = -1;

    if (circ_has_data(c)) {
        rc = *c->tail++;
        if (c->tail == c->end) c->tail = c->data;
        c->full = 0;
    }
    return rc;
}

/**
 * @brief Copy data out from a circular buffer to a max size
 * 
 * @param c 
 * @param buffer 
 * @param max 
 * @return int 
 */
static inline int circ_get_bytes(struct circ *c, uint8_t *buffer, int max) {
    int remaining = c->end - c->tail;       // bytes before the end of the buffer
    int count = MIN(circ_used(c), max);
    int rc = count;

    // Do we overflow what's remaining...
    if (count > remaining) {
        memcpy(buffer, c->tail, remaining);
        c->tail = c->data;
        count -= remaining;
    }

    // We fit in what's left
    memcpy(buffer, c->tail, count);
    c->tail += count; if (c->tail >= c->end) c->tail -= c->size;
    c->full = 0;
    return rc;
}

/**
 * @brief See if the first count bytes match
 * 
 * Returns 1 on match, 0 on non-match (i.e. not like strcmp)
 * 
 * @param c 
 * @param bytes 
 * @param count 
 */
static inline int circ_compare(struct circ *c, uint8_t *bytes, int count) {
    int n = circ_bytes_before_wrap(c);

    if (n >= count) {
        // Simple one-off comparison...
        return (memcmp(c->tail, bytes, count) == 0);        
    }
    // Otherwise we need to do this in two chunks...
    if (memcmp(c->tail, bytes, n) != 0) return 0;

    count -= n;
    bytes += n;
    return (memcmp(c->data, bytes, count) == 0);
}
static inline int circ_casecompare(struct circ *c, uint8_t *bytes, int count) {
    int n = circ_bytes_before_wrap(c);

    if (n >= count) {
        // Simple one-off comparison...
        return (strncasecmp((const char *)c->tail, (const char *)bytes, count) == 0);
    }
    // Otherwise we need to do this in two chunks...
    if (memcmp(c->tail, bytes, n) != 0) return 0;

    count -= n;
    bytes += n;
    return (strncasecmp((const char *)c->data, (const char *)bytes, count) == 0);
}


/**
 * @brief Clean out a circular buffer, i.e. initialise variable bits
 * 
 * @param c 
 */
static inline void circ_clean(struct circ *c) {
    c->head = c->tail = c->data;
    c->full = 0;
    c->last = 0;
    c->flush = 0;
}


static inline void circ_set_last(struct circ *c, int v) {
    c->last = v;
}

/**
 * @brief Set the flush flag in the given circular buffer
 * 
 * @param c 
 */
static inline void circ_set_flush(struct circ *c, int v) {
    c->flush = v;
}


#endif