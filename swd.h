/**
 * @file swd.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-06-27
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef __SWD_H
#define __SWD_H

#define SWD_OK              0
#define SWD_WAIT            1
#define SWD_FAULT           2
#define SWD_ERROR           3
#define SWD_PARITY          4


int swd_init();
int dp_init();
int swd_test();

int mem_read_block(uint32_t addr, uint32_t count, uint32_t *dest);
int mem_read(uint32_t addr, uint32_t *res);
int mem_write(uint32_t addr, uint32_t value);

int core_enable_debug();
int core_halt();
int core_unhalt();
int core_step();
int core_is_halted();
int core_reset_halt();

uint32_t rp2040_find_rom_func(char ch1, char ch2);
int rp2040_call_function(uint32_t addr, uint32_t args[], int argc);

int reg_read(int reg, uint32_t *res);
int reg_write(int reg, uint32_t value);

#endif
