/**
 * @file flash.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-05-04
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef __FLASH_H
#define __FLASH_H

#include <stdint.h>

/**
 * @brief A superblock record, needs to be 64 bytes in length
 * 
 */
struct superblock {
    union {
        char name[64-12];           // filename
        struct {
            uint32_t    m1;
            uint32_t    m2;
        } magic;
    } u;
    uint32_t    flags;              // flags (overloaded as version for first item)
    void        *ptr;               // where is it
    uint32_t    len;
};
#define SB_NAME_LEN         (sizeof((struct superblock *){0}->u.name))


void flash_init();
void debug_file_list();

struct superblock *find_file(const char *name);

void *file_addr(const char *name, int *len);

void write_file(char *name, uint8_t *data, int len);
int file_start_write(char *name, int max_size);
int file_write_block(uint8_t *data, int len, int last);

#endif
