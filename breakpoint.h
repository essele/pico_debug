

#ifndef __BREAKPOINT_H
#define __BREAKPOINT_H

int sw_bp_set(uint32_t addr, int size);
int sw_bp_clr(uint32_t addr, int size);

#endif
