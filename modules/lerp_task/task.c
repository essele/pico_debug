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

#include "lerp/task.h"
#include "list.h"

//#include "lerp/debug.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

#ifndef IDLE_STACK_SIZE
#define IDLE_STACK_SIZE 4096
#endif

#define NAKED                           __attribute__( ( naked ) )

//
// We expect these items to be in the platform specific file...
//
extern void leos_platform_init(void);
uint32_t *leos_platform_init_stack( uint32_t *sp, void (*main)(void *), void *param, void (*taskexit)(void));
extern NAKED void leos_platform_start_first(void);


//
// A few queues to handle different use cases...
//
DEFINE_LIST(ready_list, struct task);       // tasks ready to run
DEFINE_LIST(timeout_list, struct task);     // tasks waiting for a specified time

//
// Time as updated by the idle thread, so we don't call time_us_64() multiple times
//
volatile uint64_t _idle_now = 0;

// To optimise the scheduler we keep track of when we last did a timewait
// update, and also the smallest timewait in the queue, that way we only
// need to run the update after that amount of time, and only if there is
// something in the queue.
//
static uint32_t last_timewait_update = 0;
static uint32_t min_timewait_time = TIME_FOREVER;

//
// Structure and stack for the IDLE process
//
static uint8_t      idle_stack[IDLE_STACK_SIZE];
struct task         idle_TCB;

//
// Our current task
//
volatile struct task    *currentTCB = NULL;

//
// Add a task onto the timeout list and update the min_timewait
// accordingly.
//
// ISSUE: if we add something then the ELAPSED() time is going to not be from
// the point we added it so we effectively need to re-zero the timebase
// by updating the timeouts first...

static void update_timewait_tasks(uint32_t delta);

static inline void add_timewait_task(struct task *task) {
    uint32_t time = task->wait_time;
    uint32_t delta = ELAPSED(last_timewait_update, time_us_32());

    if (timeout_list.head) {
        if (delta) update_timewait_tasks(delta);
    } else {
        last_timewait_update = time_us_32();
    }
    LIST_ADD(&timeout_list, (struct task *)currentTCB, time);
    if (time < min_timewait_time) min_timewait_time = time;
}

//
// This removed an unexpired task from the timeout list and updates the time
// so that it reflects reality...
//
static inline void remove_timewait_task(struct task *task) {
    uint32_t delta = ELAPSED(last_timewait_update, time_us_32());

    LIST_REMOVE(&timeout_list, task, time);
    if (task->wait_time > delta) {
        task->wait_time -= delta;
    } else {
        task->wait_time = 0;
    }
}


//
// Block a task with no timeout, just a straight block... return value
// is the wake reason
//
inline int task_block() {
    currentTCB->state = BLOCKED;
    task_yield();
    return currentTCB->wake_reason;
}
void task_halt() __attribute__((alias("task_block")));

//
// Main method of blocking a process, takes time as an argument. If time is zero
// then this is the same as yeild() staying in the READY state. If time is 
// TIME_FOREVER then we block without a timeout, otherwise we setup a timeout
// so we are woken by that if nothing else.
//
// The return value is the wake reason.
//
inline int task_block_with_time(uint32_t time) {
    if (time == 0) {
        task_yield();
        return WAKE_READY;
    }    
    currentTCB->state = BLOCKED;
    currentTCB->wait_time = time;
    if (time == TIME_FOREVER) {
        task_yield();
    } else {
        add_timewait_task((struct task *)currentTCB);
        task_yield();
        if (currentTCB->wake_reason != WAKE_TIMEOUT) {
            remove_timewait_task((struct task *)currentTCB);
        }
    }
    return currentTCB->wake_reason;
}
//int task_sleep(uint32_t time) __attribute__((alias("task_block_with_time")));

//
// Waking up a task needs a reason, and the task must be BLOCKED
// (you can't wake an IRQWAIT task, that's ignored.)
//
inline void task_wake(struct task *task, int reason) {
    if (task->state == IRQWAIT) return;
    
    assert(task->state == BLOCKED);
    task->wake_reason = reason;
    task->state = READY;
    LIST_ADD(&ready_list, task, task);
}

//
// Before a task is awoken the reason is stored, this returns it.
//
int task_wake_reason() {
    return currentTCB->wake_reason;
}

// ---------------------------------------------------------------------
// TIME WAIT related
// ---------------------------------------------------------------------

//
// Remove delta from each waiting timer and see if any need to be unblocked
// as a result.
//
static void update_timewait_tasks(uint32_t delta) {
    uint32_t min_time = UINT_MAX;
    struct task *t = timeout_list.head;
    while(t) {
        struct task *next = t->time_next;        // so we can remove while traversing
        
        if (t->wait_time > delta) {
            t->wait_time -= delta;
            if (t->wait_time < min_time) {
                min_time = t->wait_time;
            }
        } else {
            // If we are BLOCKED here then our timer has expired ahead of anything else
            // so we can mark WAKE_TIMEOUT and unblock as normal.
            // If we are not blocked, then we need to rely on the blocking process to
            // remove us from the list as something else unblocked us.
            t->wait_time = 0;
            if (t->state == BLOCKED) {
                LIST_REMOVE(&timeout_list, t, time);
                task_wake(t, WAKE_TIMEOUT);
            }
        }
        t = next;
    }
    min_timewait_time = min_time;
    last_timewait_update = time_us_32();
}


/**
 * @brief Work out how much stack is left available for a given task (roughly in bytes)
 * 
 * This is a debug capability as it's slow, and it's not perfect
 * but it should give some indication of where there might be problems.
 * 
 * @param t 
 * @return int 
 */
static int calc_stack_free(struct task *t) {
    uint32_t *p = t->stack_end;
    int i = 0;

    while(*p == 0x55555555) {
        i++;
        p++;
    }
    return i * 4;
}

//
// This is called by the SVC processor and needs to end with a new task set
// in currentTCB so that it can execute.
//
// We currently round-robin the task list, but we probablt want to also look
// at the previous one and see if it needs to be removed from the active list
// if it's blocked on something.
//
void __time_critical_func(switchContext)() {
    struct task *prior = (struct task *)currentTCB;
    
    #if LEOS_CHECK_STACK > 0
    // See if we have a sensible amount of stack free...
    if (calc_stack_free(prior) < LEOS_CHECK_STACK) {
        debug_printf(D_GEN|D_INF, "WARNING: unused stack in %p is %d\r\n", prior, calc_stack_free(prior));
    }
    #endif

    // If the prior task isn't blocked then we can add it to the end of
    // the ready list (unless it was the IDLE task)...
    //
    // Theoretically we could also get here as "READY" in the case of a bit
    // of a race condition in the IRQ system, but that's not an issue since
    // we will be picked up by the irq_triggered section, we just need to
    // ensure we don't add it to the ready list.
    if (prior->state == RUNNING) {
        if (prior != &idle_TCB) {
            LIST_ADD(&ready_list, prior, task);
        }
        prior->state = READY;
    }
    
    // See if we need to update our timewait clocks...
    // TODO: don't like this as we are interrupts disabled while we
    // are in this function... is there a better way? (Do it in idle?)
//    if (timeout_list.head) {
//        uint32_t delta = ELAPSED(last_timewait_update, time_us_32());
//        if (delta >= min_timewait_time) {
//            update_timewait_tasks(delta);
//        }
//    }    
    
    // Pop the next item from the task list... and run IDLE if there aren't
    // any.
    struct task *task = LIST_POP(&ready_list, task, struct task *);
    if (!task) {
        task = &idle_TCB;
    }
    task->state = RUNNING;
    currentTCB = task;
}

/**
 * @brief Yield the current process if there's something else ready to run
 * 
 * This does similar checks to the scheduler to see if something needs to be
 * run. Namely is there something on the time-wait list, or is something
 * with an IRQ or otherwise ready.
 * 
 * It should be fairly quick.
 * 
 */
void task_yield_if_required() {
    if (timeout_list.head) {
        uint32_t delta = ELAPSED(last_timewait_update, time_us_32());
        if (delta >= min_timewait_time) {
            // We need to update times and yield...
            update_timewait_tasks(delta);
        }
    } 
    if (ready_list.head) {
        // We need to yield...
        task_yield();
    }

    // If we get here then we don't need to yield...
    return;
}



static void task_exit_error( void ) {
//    debug_printf(D_GEN|D_INF, "TASK EXIT ERROR\r\n");
//    debug_flush();
    while(1);
}


//
// Setup stack for trying to detect overflows...
//
static void setup_stack(struct task *task, uint8_t *stack, int size) {
    uint32_t *top = (uint32_t *)(stack + size);
    uint32_t *bot = (uint32_t *)stack;
    
    task->sp = top;
    task->stack_end = bot;
}

/**
 * @brief Create a task
 * 
 * @param task 
 * @param function 
 * @param arg 
 * @param stack 
 * @param stack_size 
 */
void task_create(struct task *task, void (*function)(void *), void *arg, uint8_t *stack, int stack_size) {
    memset(stack, 0x55, stack_size);            // for stack usage detection
    setup_stack(task, stack, stack_size);
    task->sp = leos_platform_init_stack(task->sp, function, arg, task_exit_error);
    task->state = READY;
    
    if (task != &idle_TCB) {
        LIST_ADD(&ready_list, task, task);
    }
}

/**
 * @brief Idle Process -- run when nothing else needs to, so it can poll!
 *
 * NOTE: This must not block!
 *  
 * @param param 
 */
void idle_task(void *param) {
    void (*func)(void) = param;     // ToDO: need to cast this properly?
    
    while(1) {
        task_yield_if_required();

        // Call this one for each loop, so that each polling function doesn't
        // need to do it again...
        _idle_now = time_us_64();

        // Call the app dependent polling function
        if (func) func();

//        debug_poll();
    }
}

void leos_start() {
    currentTCB = &idle_TCB;
    last_timewait_update = time_us_32();
    leos_platform_start_first();
    while(1);               // shouldn't get here at all
}

void leos_init(void (*poll_func)(void)) {
//    debug_init();
    leos_platform_init();
    task_create(&idle_TCB, idle_task, (void *)poll_func, idle_stack, IDLE_STACK_SIZE);
}

