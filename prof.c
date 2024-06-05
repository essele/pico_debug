#include "pico/stdlib.h"
#include "pico/printf.h"
#include "pico/bootrom.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

#include "gdb.h"
#include "swd.h"
#include "adi.h"
#include "lerp/io.h"
#include "lerp/task.h"
#include "utils.h"
#include "flash.h"
#include "breakpoint.h"

#include "lerp/debug.h"
#include "lerp/io.h"

#define START_SAMPLING_CMD 0xa0
#define STOP_SAMPLING_CMD 0xa1
#define SAMPLE_CMD 0xb0

#define FLAG_CORES_MASK (1)
#define FLAG_BOTH_CORES (0 << 0)
#define FLAG_ONE_CORE (1 << 0)

// Number of bytes the actual address is before the content of the LR
// register. Because on ARM LR points to the instruction to be executed
// on return, we want to point to the actual branch -- which will be either
// 2 or 4 bytes before. We don't know which one (Thumb insn could be either),
// but setting it to 2 bytes back will cause the profiler to figure out the right
// instruction.
#define LR_OFFSET 2

// These are correct for Pico. The Flash in particular
// will differ based on what size flash chips are installed
#define FLASH_LO 0x10000000
#define FLASH_HI 0x10200000
#define MEM_LO   0x20000000
#define MEM_HI   0x20042000

struct io *prof_io = NULL;

// gcc says that on thumb, reg 7 is used as the frame pointer register?
#define REG_THUMB_FP 7
#define REG_ARM_FP 11
#define REG_LR 14
// this is technically DebugReturnAddress, but it's effectively accurate
// (next instruction to be executed after return from debug)
#define REG_PC 15

// from adi.c
int core_update_status();

// code can be executed from flash or RAM
static bool valid_pc(uint32_t addr) {
    if (addr >= FLASH_LO && addr < FLASH_HI)
        return true;
    if (addr >= MEM_LO && addr < MEM_HI)
        return true;
    return false;
}

// but frame pointers can only be on the stack, in ram
static bool valid_mem(uint32_t addr) {
    if (addr >= MEM_LO && addr < MEM_HI)
        return true;

    return false;
}

static void reset_halt() {
    core_select(0);
    core_reset_halt();
    core_select(1);
    core_reset_halt();
    core_select(0);
}

static int was_connected = 0;
static int profiling_active = 0;
static int profiling_interval_ms = 100;

static int max_cores = 2;

#define MAX_FRAMES 64
#define NUM_RESERVED_FRAMES 2
static uint32_t frame_buf[MAX_FRAMES];

static int prof_task_io();
static int sample_core(int core_num, uint32_t* frame_buf, int max_frames, uint32_t* timestamp, uint32_t* us_out);
static int write_frames(int core_num, uint32_t timestamp, uint32_t* frame_buf, int num_frames);

static inline void dump_sampling_time_stats_real();

// convenient for avg shift
// must be a power of 2
#define US_COST_POW2 5
#define US_COST_LEN (1<<(US_COST_POW2))
static int us_cost_data[US_COST_LEN];
static int us_cost_next = 0;

// helpers to time how long each sample sequence takes
#if true
#define dump_sampling_time_stats() do { } while(0)
#define record_sampling_time_stats(x) do { } while(0)
#else
#define dump_sampling_time_stats dump_sampling_time_stats_real
static inline void record_sampling_time_stats(uint32_t elapsed) {
    us_cost_data[us_cost_next++] = elapsed;
}
#endif

//
// The main profiler task loop
//
void prof_task() {
    int rc;

    if (!prof_task_io()) {
        return;
    }

    if (profiling_active) {
        int frame_cnt;
        uint32_t timestamp = 0;
        uint32_t elapsed;

        for (int core = 0; core < max_cores; core++) { 
            frame_cnt = sample_core(core,
                frame_buf+NUM_RESERVED_FRAMES,
                MAX_FRAMES-NUM_RESERVED_FRAMES,
                &timestamp, &elapsed);

            if (frame_cnt > 0) {
                write_frames(core, timestamp, frame_buf, frame_cnt);
            }

            record_sampling_time_stats(elapsed);
        }

        dump_sampling_time_stats();
    }

    task_sleep_ms(profiling_interval_ms);
}

void dump_sampling_time_stats_real() {
    if (us_cost_next == US_COST_LEN) {
        uint64_t total_us = 0;
        uint32_t min_us = UINT32_MAX, max_us = 0;

        for (int i = 0; i < US_COST_LEN; i++) {
            uint32_t us = us_cost_data[i];
            min_us = us < min_us ? us : min_us;
            max_us = us > max_us ? us : max_us;
            total_us += max_us;
        }

        uint32_t avg_us = (uint32_t) (total_us >> US_COST_POW2);

        debug_printf("Sampling time: min: %d max: %d avg: %d us\r\n", min_us, max_us, avg_us);

        us_cost_next = 0;
    }
}

int prof_task_io() {
    if (!io_is_connected(prof_io)) {
        task_sleep_ms(profiling_interval_ms);
        return false;
    }

    if (!was_connected) {
        // This is the first time a connection happened
        if (dp_init() != SWD_OK) {
            debug_printf("unable to connect to target, trying again...\r\n");
            task_sleep_ms(250);
            return false;
        }

        debug_printf("SWD initialized for profiler\r\n");
        was_connected = 1;
    }

    int ch = 0;
    while ((ch = io_get_byte_nb(prof_io)) > 0) {
        // TODO -- move this into cmdline
        switch (ch) {
            // these are hacks
            case 'r':
                reset_halt();
                profiling_active = false;
                break;
            case 'g':
                core_select(1);
                core_unhalt();
                core_select(0);
                core_unhalt();
                profiling_active = true;
                break;
            case 'q':
                reset_usb_boot(0, 0);
            case ' ':
                profiling_active = !profiling_active;
                break;
            case '1':
                profiling_active = !profiling_active;
                max_cores = 1;
                break;

            // these are the actual commands
            case START_SAMPLING_CMD: {
                profiling_active = false;

                int byte2 = io_get_byte_nb(prof_io);
                int byte3 = io_get_byte_nb(prof_io);
                int byte4 = io_get_byte_nb(prof_io);
                if (byte2 < 0 || byte3 < 0 || byte4 < 0) {
                    debug_printf("got error reading start command bytes: %d %d %d\r\n", byte2, byte3, byte4);
                    break;
                }

                uint32_t sampling_interval = (byte3 << 8) | byte2;
                uint32_t flags = byte4;

                if (sampling_interval == 0) {
                    debug_printf("got 0 sampling interval\r\n");
                    break;
                }

                profiling_interval_ms = sampling_interval;
                if ((flags & FLAG_CORES_MASK) == FLAG_ONE_CORE) {
                    max_cores = 1;
                } else {
                    max_cores = 2;
                }

                debug_printf("Profiling started: %d ms interval, %d cores\r\n", sampling_interval, max_cores);
                profiling_active = true;
                break;
            }
            case STOP_SAMPLING_CMD: {
                profiling_active = false;
                break;
            }
        }
    }

    return true;
}

//
// sampling function
//
int sample_core(
    int core_num,
    uint32_t* frame_buf,
    int max_frames,
    uint32_t* sample_time,
    uint32_t* us_out)
{
    int rc;

    uint32_t tstart = time_us_32();

    core_select(core_num);
    core_halt();

    uint32_t pc, lr, fp;
    uint32_t fplr, fpsp, fpfp;

    rc  = reg_read(REG_LR, &lr);
    rc |= reg_read(REG_PC, &pc);
    rc |= reg_read(REG_THUMB_FP, &fp);
    if (rc != SWD_OK) {
        debug_printf("SWD NOT OK -- CORE(s) HALTED\r\n");
        return -1;
    }

    int framenum = 0;

    // always trust PC
    frame_buf[framenum++] = pc;

#if false
    if (core_num == 1) {
        if (valid_mem(fp)) {
            mem_read32(fp-4, &fplr);
            mem_read32(fp-12, &fpfp);
        } else {
            fplr = 0;
            fpfp = 0;
        }
        debug_printf("core %d: pc=0x%08x lr=0x%08x fplr=0x%08x fp=0x%08x fpfp=0x%08x @ %u\r\n", core_num, pc, lr, fplr, fp, fpfp, tstart);
    }
#endif

    if (!valid_mem(fp) || !valid_mem(fp-12)) {
        //debug_printf("core %d: 0x%08x is not a valid FP address\r\n", core_num, fp);

        // YOLO lr and give up
        frame_buf[framenum++] = lr - LR_OFFSET;
        goto FINISH;
    }

    // Now just trust the frame pointers.
    while (valid_mem(fp) && valid_mem(fp-12) && framenum < max_frames) {
        mem_read32(fp-4, &fplr);
        //mem_read32(fp-8, &fpsp);
        mem_read32(fp-12, &fpfp);

        if (fp == fpfp) {
            // don't get stuck in a loop. also, if fp points back to itself, then
            // we don't want to include this LR
            break;
        }

        //debug_printf("core %d: %d: fp=0x%08x fplr=0x%08x fpsp=0x%08x fpfp=0x%08x\r\n", core_num, framenum, fp, fplr, fpsp, fpfp);
        if (!valid_pc(fplr)) {
            //debug_printf("fplr 0x%08x is not valid!\r\n", fplr);
            break;
        }

        frame_buf[framenum++] = fplr - LR_OFFSET;
        fp = fpfp;
    }

FINISH:
    core_select(core_num);
    core_unhalt();

    // Note: without this, the core remains halted. Unclear why this is needed,
    // someone with more SWD experience will know.
    rc = core_update_status();
    if (rc != SWD_OK) {
        debug_printf("core_update_status failed: %d\r\n", rc);
    }

    *sample_time = tstart;
    *us_out = time_us_32() - tstart;

    //if (core_num == 1) { debug_printf("core %d: pc: 0x%08x lr: 0x%08x final fp: 0x%08x\r\n", pc, lr, fp); }

    return framenum;
}

//
// data writing function
//
int write_frames(int core_num, uint32_t timestamp, uint32_t* frame_buf, int num_frames) {
    uint32_t header = 0;
    header |= (SAMPLE_CMD | core_num) << 24; // 0xb0 | core_num = sample indicator
    header |= num_frames & 0xfff; // num frames to follow

    // 0 slot is reserved for this purpose
    frame_buf[0] = header;
    frame_buf[1] = timestamp;

    // TODO -- this blocks, I think we'd rather drop samples?
#if true
    io_put_bytes(prof_io, (uint8_t*) frame_buf, sizeof(uint32_t) * (num_frames + NUM_RESERVED_FRAMES));

#if false
    if (core_num == 1) {
    debug_printf("core[%d]: write: 0x%08x 0x%08x 0x%08x", core_num, frame_buf[0], frame_buf[1], frame_buf[2]);
    for (int i = 1; i < num_frames; i++)
        debug_printf(" 0x%08x", frame_buf[NUM_RESERVED_FRAMES+i]);
    debug_printf("\r\n");
    }
#endif
#else
    uint32_t *fb = frame_buf+NUM_RESERVED_FRAMES;
    io_printf(prof_io, "core %d pc 0x%08x lr 0x%08x next 0x%08x\r\n", core_num, fb[0], fb[1], fb[2]);
#endif

    return 0;
}

DEFINE_TASK(prof, 1024);

void func_prof(void *arg) {
    // Initialise the IO mechanism for the profiler; CDC only
    prof_io = io_init(PROF_CDC, 0, 4096);

    debug_printf("PROF initialized\r\n");

    while (1) {
        prof_task();
    }
}

void prof_init() {
    CREATE_TASK(prof, func_prof, NULL);
}