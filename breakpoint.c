#include <stdlib.h>
#include "pico/stdlib.h"
#include "breakpoint.h"
#include "swd.h"
#include "adi.h"

//
// Rushed implementation of software breakpoints ... need to redo
//

struct swbp {
    struct swbp *next;
    uint32_t    addr;
    int         size;
    uint32_t    orig;
};
struct swbp *sw_breakpoints = NULL;

static struct swbp *find_swbp(uint32_t addr) {
    struct swbp *p = sw_breakpoints;
    while (p) {
        if (p->addr == addr) return p;
    }
    return NULL;
}

int sw_bp_set(uint32_t addr, int size) {
    if (!find_swbp(addr)) {
        struct swbp *bp = malloc(sizeof(struct swbp));
        bp->addr = addr;
        bp->size = size;
        CHECK_OK(mem_read_block(addr, size, (uint8_t *)&(bp->orig)));
        uint32_t code = 0xbe11be11;
        mem_write_block(addr, size, (uint8_t *)&code);
        bp->next = sw_breakpoints;
        sw_breakpoints = bp;
    }
    return SWD_OK;
}
int sw_bp_clr(uint32_t addr, int size) {
    struct swbp *bp = find_swbp(addr);
    if (bp) {
        CHECK_OK(mem_write_block(bp->addr, bp->size, (uint8_t *)&bp->orig));
        if (sw_breakpoints == bp) {
            sw_breakpoints = bp->next;
        } else {
            struct swbp *p = sw_breakpoints;
            while (p) {
                if (p->next == bp) {
                    p->next = bp->next;
                    break;
                }
                p = p->next;
            }
        }
        free(bp);
    }
    return SWD_OK;
}
