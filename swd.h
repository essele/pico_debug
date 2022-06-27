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

#endif
