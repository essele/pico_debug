/**
 * @file flash.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-05-04
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <stdlib.h>
#include <string.h>
#include "lerp/flash.h"
#include "lerp/debug.h"
#include "hardware/flash.h"
#include "pico/bootrom.h"

// We use dynamic memory for re-writing the superblock
// #include <umm_malloc.h>

// We provide a custom file opener for the LWIP httpd
//#include "lwip/apps/fs.h"

//
// Define the area that we use for our flash filesystem...
//
#define FLASH_SIZE          (2048 * 1024)
#define FS_SIZE             (1024 * 1024)
#define FS_OFFSET           (FLASH_SIZE - FS_SIZE)
#define FS_BASE             (XIP_BASE + FS_OFFSET)
#define FS_END              (XIP_BASE + FLASH_SIZE)

//
// We can erase different size blocks...
//
#define FLASH_ERASE256          0x81
#define FLASH_ERASE4K           0x20
#define FLASH_ERASE32K          0x52
#define FLASH_ERASE64K          0xd8

/**
 * @brief Rounds a value up to the nearest blocksize
 * 
 * @param value 
 * @param blksize 
 * @return uint32_t 
 */
static inline uint32_t blk_up(uint32_t value, uint32_t blksize){
    return ((value-1) & ~(blksize-1)) + blksize;
}

// Given an address in cached flash, what would the uncached version be
#define UNCACHED(addr)          ((void *)((uint32_t)(addr) | 0x03000000))
#define CACHED(addr)            ((void *)((uint32_t)(addr) & ~0x03000000))

// We have the really horrible stuff that we have to duplicate so we can get
// access to the flash/bootrom etc.
#define BOOT2_SIZE_WORDS 64

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static bool boot2_copyout_valid = false;

static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {
    if (boot2_copyout_valid)
        return;
    for (int i = 0; i < BOOT2_SIZE_WORDS; ++i)
        boot2_copyout[i] = ((uint32_t *)XIP_BASE)[i];
    __compiler_memory_barrier();
    boot2_copyout_valid = true;
}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    ((void (*)(void))boot2_copyout+1)();
}

/**
 * @brief Program the flash
 * 
 * Firstly this uses address space addresses to make pointers and other things easier
 * to work with (it translates to offsets), and it also erases whatever is needed in
 * the most optimal way it can (up to 32K blocks), and tries to be efficient with a
 * cache flush.
 */
void __no_inline_not_in_flash_func(flash_program)(void *ptr, void *buffer, size_t len) {
    // STAGE 0: setup all of the rom function values...
    rom_connect_internal_flash_fn connect_internal_flash = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn flash_range_erase = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_flush_cache_fn flash_flush_cache = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash && flash_exit_xip && flash_range_erase && flash_flush_cache);

    uint32_t offset = (uint32_t)ptr & 0x00ffffff;

    // STAGE 1: check everything is sensible
    assert((uint32_t)ptr % 256 == 0);         // must be page aligned
    assert((uint32_t)ptr >= FS_BASE);         // must be in the right bit of flash
    assert((uint32_t)ptr + len < FS_END);     // must all fit in the right bit of flash
    assert(len % 256 == 0);         // must be multiple of a page size

    // STAGE 2: check if we need to erase (this is probably slow) and ideally
    // would be block by block based, but this is simple at least.
    uint32_t *p = UNCACHED(ptr);
    uint32_t *end = UNCACHED(ptr + len);
    int need_erase = 0;
    while (p < end) {
        if (*p++ != 0xffffffff) {
            need_erase = 1; 
            break;
        }
    }

    // STAGE 3: Turn of XIP
    flash_init_boot2_copyout();
    __compiler_memory_barrier();
    connect_internal_flash();
    flash_exit_xip();

    // STAGE 4: erase (if we need to)
    if (need_erase) {
        uint32_t erase_offset = offset;
        uint32_t erase_size = len;

        // While we are not 
        while (erase_size && (erase_offset % 4096)) {
            flash_range_erase(erase_offset, 256, 256, FLASH_ERASE256);
            erase_offset += 256; erase_size -=256;
        }
        while ((erase_size >= 4096) && (erase_offset % 32768)) {
            flash_range_erase(erase_offset, 4096, 4096, FLASH_ERASE4K);
            erase_offset += 4096; erase_size -= 4096;
        }
        while (erase_size >= 32768) {
            flash_range_erase(erase_offset, 32768, 32768, FLASH_ERASE32K);
            erase_offset += 32768; erase_size -= 32768;
        }
        while (erase_size >= 4096) {
            flash_range_erase(erase_offset, 4096, 4096, FLASH_ERASE4K);
            erase_offset += 4096; erase_size -= 4096;
        }
        while (erase_size >= 256) {
            flash_range_erase(erase_offset, 256, 256, FLASH_ERASE256);
            erase_offset += 256; erase_size -= 256;
        }
    }

    // STAGE 5: program
    flash_range_program(offset, buffer, len);

    // STAGE 6: invalidate cache (for our programmed bit only)
    /*
    p = ptr;
    end = ptr + len;
    while (p < end) {
        *p++ = 0x0;       // write to 0x10... space causes cache flush
    }
    */
   flash_flush_cache();

    // STAGE 7: turn XIP back on
    flash_enable_xip_via_boot2();
}


// We keep the current superblock as a global, saves constantly searching for it...
struct superblock *superblock = NULL;


//#define MAGIC_S1        0x52455448
#define MAGIC_S1        0x0000000c
#define MAGIC_S2        0x494E4B31
#define MAGIC_E1        0x12983476
#define MAGIC_E2        0x99335577

//#define IS_END_RECORD(p)        (*((uint32_t *)(p)) == MAGIC_E1 && *((uint32_t *)(p+1)) == MAGIC_E2)
#define IS_END_RECORD(p)        ((p)->u.magic.m1 == MAGIC_E1 && (p)->u.magic.m2 == MAGIC_E2)

/**
 * @brief See if a given superblock (first in a page) is a valid start of superblock record
 * 
 * To be valid the two magic numbers need to be there, the first pointer needs to match the address
 * and we need to be able to find the end record (within len)
 * 
 * NOTE: the pointer check is only done on the lower 24 bits so we can do it cached or uncached
 * 
 * @param p 
 * @return int 
 */
static int is_valid_superblock(struct superblock *p) {
    if (p->u.magic.m1 != MAGIC_S1 || p->u.magic.m2 != MAGIC_S2) return 0;
    if (((uint32_t)p->ptr & 0xffffff) != ((uint32_t)p & 0xffffff)) return 0;
    if (p->len > 65536) return 0;       // arbitary max limit
    
    int count = p->len / sizeof(struct superblock);
    p += count - 1;
    if (p->u.magic.m1 != MAGIC_E1 || p->u.magic.m2 != MAGIC_E2) return 0;

    return 1;
}

/**
 * @brief Search the filesystem for the highest version superblock
 * 
 * We need to cater for the version number wrapping so we keep track of both a high
 * value and a low value (top half and lower half of int range) if we have both we use
 * the low one.
 * 
 * @return struct superblock* 
 */
static struct superblock *find_superblock() {
    struct superblock *hi_candidate = NULL;
    struct superblock *lo_candidate = NULL;
    struct superblock *p = UNCACHED(FS_BASE);

    while ((void *)p < UNCACHED(FS_END)) {
        if (is_valid_superblock(p)) {
            if (p->flags & 0x8000000) {
                // We are in the high range...
                if (!hi_candidate || p->flags > hi_candidate->flags) hi_candidate = p;
            } else {
                // The lower range...
                if (!lo_candidate || p->flags > lo_candidate->flags) lo_candidate = p;
            }
        }
        // Move onto the next full page...
        p += 256/sizeof(struct superblock);
    }
    // If we have both then use lo (since we have wrapped), otherwise use whichever
    // we have
    if (hi_candidate && lo_candidate) return lo_candidate;
    superblock = CACHED((struct superblock *)((uint32_t)hi_candidate | (uint32_t)lo_candidate));
    return superblock;
}



static void *find_space(uint32_t size) {
    assert(superblock);
    assert(size % 256 == 0);
    struct superblock *sbp = superblock;

    // We know that the superblock lists files in order, starting from after
    // the SB. So put a ptr at the potentially first block of free space
    void *ptr = (void *)blk_up((uint32_t)superblock + superblock->len, 256);
    debug_printf("FINDSPACE: ptr=%08x\r\n", ptr);

    sbp++;      // get past first record
    while (!IS_END_RECORD(sbp)) {
        if (sbp->ptr < ptr) {
            // the file is before where we were, so this is a loop, so we care about
            // space to the end
            int space = (void *)FS_END - ptr;
            if (space >= size) return ptr;
            ptr = (void *)FS_BASE;
        } else {
            int space = sbp->ptr - ptr;
            if (space >= size) return ptr;
            ptr = sbp->ptr + blk_up(sbp->len, 256);
        }
        sbp++;
    }
    return NULL;
}




/**
 * @brief Find the first (address wise) file in the superblock that is
 * at or above the given address. If inc_sb then include the superblock
 * as if it were a file.
 * 
 * @param addr 
 * @return struct superblock* 
 */
static struct superblock *find_lowest_file_from(void *addr, int inc_sb) {
    assert(superblock);
    struct superblock *l = NULL;
    struct superblock *p = superblock;

    if (!inc_sb) p++;       // get past the master record

    while (!IS_END_RECORD(p)) {
        if (p->ptr >= addr) {
            if (!l || (p->ptr < l->ptr)) l = p;
        }
        p++;
    }
    return l;
}
static struct superblock *find_lowest_file_after(void *addr, int inc_sb) {
    assert(superblock);
    struct superblock *l = NULL;
    struct superblock *p = superblock;

    if (!inc_sb) p++;       // get past the master record

    while (!IS_END_RECORD(p)) {
        if (p->ptr > addr) {
            if (!l || (p->ptr < l->ptr)) l = p;
        }
        p++;
    }
    return l;
}

// ------------------------------------------------------------------------
// For writing new superblocks to the flash, we buffer 4 entries and write
// them as we fill the buffer.
// ------------------------------------------------------------------------

static void add_sbitem(struct superblock *start_addr, struct superblock *sb) {
    static struct superblock *sbimg = NULL;
    static void *sb_write_addr;
    static int count;

    if (count > 3 || !sb) {
        assert(sbimg);      // can't write something we don't have!
        debug_printf("Writing superblock block at %08x\r\n", sb_write_addr);
        //dump((uint8_t *)sbimg, 256);
        flash_program(sb_write_addr, (void *)sbimg, 256);
        sb_write_addr += 256;
        count = 0;
        free(sbimg);
        sbimg = NULL;
    }

    if (!sb) return;        // was just a flush

    // If we don't have a memory block then allocate one...
    if (!sbimg) {
        sbimg = malloc(sizeof(struct superblock) * 4);
        if (!sbimg) panic("unable to malloc for sbimg");
        memset(sbimg, 0xff, sizeof(struct superblock) * 4);
    }

    // A valid start address means this is a new batch...
    if (start_addr) {
        count = 0;
        sb_write_addr = (void *)start_addr;
    }
    memcpy(&sbimg[count], sb, sizeof(struct superblock));
    count++;
    // Flush may happen next time (or if called as a flush)
}

struct superblock *find_file(const char *name) {
    struct superblock *sb = superblock;

    if (!sb) return NULL;
    sb++;       // move past the initial record
    while (!IS_END_RECORD(sb)) {
        debug_printf("FIND FILE: Looking at %08x\r\n", sb);
        //dump((uint8_t *)sb, sizeof(struct superblock));
        if (strcmp(name, sb->u.name) == 0) return sb;
        sb++;
    }
    return NULL;
}


/**
 * @brief Add, overwrite, or remove an entry from the superblock
 * 
 * name determins if we know about the file, if we do and data is set
 * then we overwrite it, NULL data means delete, otherwise we add.
 * 
 * @param nsb New Superblock location
 * @param name 
 * @param data 
 * @param len 
 */
static void update_superblock(struct superblock *nsb, char *name, void *file_pos, int len) {
    static struct superblock file;

    debug_printf("update_superblock: newpos=%08x name=%s filepos=%08x filelen=%d\r\n", nsb, name, file_pos, len);

    // See if we already know about this file...
    struct superblock *oldsb = find_file(name);

    debug_printf("old superblock for file is at %08x\r\n", oldsb);
    debug_printf("actual main superblock currently: %08x\r\n", superblock);

    // Work out a new length for the superblock (are we adding, overwriting, or removing)
    int newlen = superblock ? superblock->len : sizeof(struct superblock) * 2;
    if (oldsb && !file_pos) {
        newlen -= sizeof(struct superblock);    // removing an entry
    } else if (!oldsb && file_pos) {
        newlen += sizeof(struct superblock);
    }
    // Otherwise this is an overwrite, so we are the same...

    // Build master record...
    file.u.magic.m1 = MAGIC_S1;
    file.u.magic.m2 = MAGIC_S2;
    file.flags = superblock ? superblock->flags + 1 : 1;
    file.ptr = (void *)nsb;
    file.len = newlen;
    add_sbitem(nsb, &file);

    if (superblock) {
        // Now put in all the records after us, until the end of the FS
        struct superblock *f = find_lowest_file_after(nsb, 0);
        while (f) {
            if (f != oldsb) add_sbitem(NULL, f);
            f = find_lowest_file_after(f->ptr, 0);
        }
        // Now we need to worry about the files that are before us...
        f = find_lowest_file_after(NULL, 0);
        while (f) {
            if (f->ptr >= file_pos) break;
            if (f != oldsb) add_sbitem(NULL, f);
            f = find_lowest_file_after(f->ptr, 0);
        }
    }

    // Now add the record for this file...    
    strncpy(file.u.name, name, sizeof(file.u.name));
    file.u.name[sizeof(file.u.name)-1] = 0;
    file.flags = 0;
    file.ptr = file_pos;
    file.len = len;
    add_sbitem(NULL, &file);

    // Now add th end record...
    file.u.magic.m1 = MAGIC_E1;
    file.u.magic.m2 = MAGIC_E2;
    file.ptr = (void *)FS_END;
    add_sbitem(NULL, &file);

    // And flush to make sure it's written
    add_sbitem(NULL, NULL);

    // Update the global superblock address...
    superblock = nsb;
}


void write_file(char *name, uint8_t *data, int len) {
    void *file_pos;
    void *nsb_pos;

    debug_printf("write_file: Superblock is at %08x\r\n", superblock); debug_flush();

    if (!superblock) {
        file_pos = (void *)FS_BASE;
    } else {
        int nsb_size = superblock->len;
        if (!find_file(name)) nsb_size += sizeof(struct superblock);
        file_pos = find_space(blk_up(nsb_size, 256) + blk_up(len, 256));
    }
    flash_program(file_pos, data, blk_up(len, 256));
    nsb_pos = file_pos + blk_up(len, 256);
    update_superblock((struct superblock *)nsb_pos, name, file_pos, len);
}

// State for the file write sequence...
static void         *write_file_addr = NULL;         // where does the file start
static void         *write_curr_addr = NULL;         // current block writing position
static uint32_t     write_size = 0;                  // how much have we written
static uint32_t     write_remaining = 0;             // how much more can we write (max)
static char         write_name[SB_NAME_LEN];


int file_start_write(char *name, int max_size) {
    if (!superblock) {
        write_file_addr = (void *)FS_BASE;
    } else {
        int nsb_size = superblock->len;
        if (!find_file(name)) nsb_size += sizeof(struct superblock);
        write_file_addr = find_space(blk_up(nsb_size, 256) + blk_up(max_size, 256));
        debug_printf("FIND SPACE found %08x\r\n", write_file_addr);
    }
    // Keep the name, safely null terminate...
    strncpy(write_name, name, SB_NAME_LEN-1);
    write_name[SB_NAME_LEN-1] = 0;

    // Setup our address and sizes...
    write_curr_addr = write_file_addr;
    write_size = 0;
    write_remaining = max_size;
    return 0;
}

int file_write_block(uint8_t *data, int len, int last) {
    assert(last || (len % 256) == 0);
    assert(write_curr_addr);

    if (write_remaining < len) {
        debug_printf("attempting to write more than max\r\n");
        return 0;
    }

    int blksize = blk_up(len, 256);
    flash_program(write_curr_addr, data, blksize);
    write_curr_addr += blksize;
    write_size += len;
    write_remaining -= len;

    if (last) {
        update_superblock((struct superblock *)write_curr_addr, write_name, write_file_addr, write_size);
        write_curr_addr = NULL;
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Custom file interface for the LWIP httpd
// ---------------------------------------------------------------------------
static const char flash_prefix[] = "/flash/";

/*
int fs_open_custom(struct fs_file *file, const char *name) {
    struct superblock *sb;

    debug_printf("OPEN CUSTOM(%s)\r\n", name); debug_flush();

    // First see if we should look at the flash...
    if (strncmp(name, flash_prefix, sizeof(flash_prefix)-1) != 0) return 0;

    name += sizeof(flash_prefix) - 1;
    sb = find_file(name);
    if (!sb) return 0;

    // We've found it...
//    file->data = sb->ptr;
    file->data = UNCACHED(sb->ptr);
    file->len = sb->len;
    file->index = sb->len;      // ???
//    file->pextension = NULL;

    file->flags = 0;

    debug_printf("FOUND ON FLASH %08x (len=%d)\r\n", sb->ptr, sb->len);
    //dump(sb->ptr, sb->len);
    return 1;
}
void fs_close_custom(__unused struct fs_file *file) {   
    // Do nothing
}
*/

void debug_file_list() {
    if (!superblock) {
        debug_printf("no files\r\n");
        return;
    }
    void *p = (void *)FS_BASE;
    while (1) {
        struct superblock *s = find_lowest_file_from(p, 1);
        if (s) {
            // We have a file....
            if (s->ptr > p) {
                // Some space first...
                debug_printf("addr=%08x space=%d\r\n", p, s->ptr - p);
            }
            if (s == superblock) {
                debug_printf("addr=%08x SUPERBLOCK size=%d\r\n", s->ptr, s->len);                
            } else {
                debug_printf("addr=%08x file=[%s] size=%d\r\n", s->ptr, s->u.name, s->len);
            }
            p = s->ptr + s->len;
            p = (void *)blk_up((uint32_t)p, 256);
        } else {
            // Space to the end...
            debug_printf("addr=%08x space=%d\r\n", p, FS_END - (uint32_t)p);
            break;
        }
    }
}

void *file_addr(const char *name, int *len) {
    struct superblock *sb = find_file(name);

    if (!sb) return NULL;
    if (len) *len = sb->len;
    return CACHED(sb->ptr);
}



uint8_t buf[256];

void flash_init() {
    superblock = find_superblock();
    debug_printf("flash_init: Superblock at %08x\r\n", superblock); debug_flush();


    strcpy((char *)buf, "This\nIs\nA flash based file ... will it work\n");
    for (int i=0; i < 2; i++) {
        strcat((char *)buf, "A line of test &blah <ok> following\n");
    }


    write_file("lee.txt", buf, strlen((char *)buf));
    
    debug_printf("File written\r\n");
    debug_printf("Flash_init: Superblock is now at %08x\r\n", superblock); debug_flush();
}

