/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/

#ifndef _LERP_TASK_H
#define _LERP_TASK_H

#include "pico/stdlib.h"

// ---------------------------------------------------------------------
// Main struct and values for the task
// ---------------------------------------------------------------------

enum state {
    READY = 0,
    BLOCKED = 1,
    IRQWAIT = 2,                    // special case, can't be woken by anything else
    RUNNING = 3,
};

// TODO ... remove this, we don't want to expose this
enum wake_reason {
    WAKE_UNKNOWN = 0,
    WAKE_IRQ,
    WAKE_READY,                    // we yieled as READY, so just rescheduled.
    WAKE_GENERIC,
    WAKE_TIMEOUT,
    WAKE_FLAG,
    WAKE_MUTEX,
};
    
struct task {
    uint32_t            *sp;
    uint32_t            *stack_end;
    enum state          state;
    int                 wake_reason;
    uint32_t            wait_time;              // used when blocked for time

    struct task 	*task_next;
    struct task 	*task_prev;
    struct task 	*time_next;
    struct task 	*time_prev;

// Defined above so we don't have to include list.h
//    LIST_REF(task, struct task *);
//    LIST_REF(time, struct task *);
};

#define DEFINE_TASK(name, stacksize)    struct task name; uint8_t _stack_##name[stacksize]
#define CREATE_TASK(name, f, arg)       task_create(&name, f, arg, _stack_##name, sizeof(_stack_##name))
    
//
// We expose a couple of otherwise internal variables so that we have efficient
// access to them rather than needing a function call
//
extern volatile struct task *currentTCB;
//extern volatile uint32_t current_tick;

#define current_task()      ((struct task *)currentTCB)
//#define task_current_tick() (current_tick)

//
// Work out how many ticks have elasped, but taking into account overflow
// This assumes a uint32_t to ensure correct overflow
//
#define ELAPSED(then,now)       (uint32_t)((then) < (now) ? (now) - (then) : ~(then)+1+(now))
#define TIME_EXPIRED            0xffffffff
#define TIME_FOREVER            0xffffffff

//
// Main return values for success and failure...
//
#define LE_OK                   0
#define LE_ERR                  1
#define LE_ABORT                2


//
// Fundamental functions....
//
//struct task *current_task();
int task_block();
void task_halt();
int task_block_with_time(uint32_t time);
void task_wake(struct task *task, int reason);
int task_wake_reason();

static inline int task_sleep_us(uint32_t us) { return task_block_with_time(us); }
static inline int task_sleep_ms(uint32_t ms) { return task_block_with_time(ms * 1000); }
static inline int task_sleep(uint32_t s) { return task_block_with_time(s * 1000 * 1000); }

void leos_init(void (*poll_func)(void));

void leos_start();
void task_create(struct task *task, void (*function)(void *), void *arg, uint8_t *stack, int stack_size);
void task_yield( void );
void task_yield_if_required();

#endif      // _LERP_TASK_H

