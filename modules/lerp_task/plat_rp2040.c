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

#include "pico/stdlib.h"

#include "hardware/regs/m0plus.h"
#include "hardware/structs/scb.h"
#include "hardware/irq.h"
#include "hardware/exception.h"
#include "hardware/clocks.h"

#include <stdio.h>

#define NVIC_INT_CTRL_REG               (*(volatile uint32_t *)(PPB_BASE + M0PLUS_ICSR_OFFSET))
#define NVIC_PENDSVSET_BIT              M0PLUS_ICSR_PENDSVSET_BITS

#define NVIC_SHPR3_REG                  (*(volatile uint32_t *)(PPB_BASE + M0PLUS_SHPR3_OFFSET))
#define MIN_INTERRUPT_PRIORITY          (255UL)
#define NVIC_PENDSV_PRI                 (MIN_INTERRUPT_PRIORITY << 16UL)

#define INITIAL_XPSR                    (0x01000000)

#define NAKED                           __attribute__( ( naked ) )

/**
 * @brief Yield the current task by raising a PendSV exception...
 * 
 */
void task_yield( void )
{
    
    /* Set a PendSV to request a context switch. */
    NVIC_INT_CTRL_REG = NVIC_PENDSVSET_BIT;
    //*(volatile uint32_t *)0xe000ed04 = NVIC_PENDSVSET_BIT;

    /* Barriers are normally not required but do ensure the code is completely
     * within the specified behaviour for the architecture. */
    __asm volatile ( "dsb" ::: "memory" );
    __asm volatile ( "isb" );
}

// NOTE: I've removed the disabling of IRQs aroudn the switchContext function as
// we are now fully co-operative, and no scheduling happens from an IRQ so theres
// no reason to disable anything.
NAKED void __time_critical_func(PendSV_Handler)(void) {
    __asm volatile
    (
        "	.syntax unified						\n"
        "	mrs r0, psp							\n"
        "										\n"
        "	ldr	r3, =currentTCB      			\n"/* Get the location of the current TCB. */
        "	ldr	r2, [r3]						\n"
        "										\n"
        "	subs r0, r0, #32					\n"/* Make space for the remaining low registers. */
        "	str r0, [r2]						\n"/* Save the new top of stack. */
        "	stmia r0!, {r4-r7}					\n"/* Store the low registers that are not saved automatically. */
        " 	mov r4, r8							\n"/* Store the high registers. */
        " 	mov r5, r9							\n"
        " 	mov r6, r10							\n"
        " 	mov r7, r11							\n"
        " 	stmia r0!, {r4-r7}					\n"
        "										\n"
        "	push {r3, r14}						\n"
//        "	cpsid i								\n"
        "	bl switchContext	   			    \n"
//        "	cpsie i								\n"
        "	pop {r2, r3}						\n"/* lr goes in r3. r2 now holds tcb pointer. */
        "										\n"
        "	ldr r1, [r2]						\n"
        "	ldr r0, [r1]						\n"/* The first item in pxCurrentTCB is the task top of stack. */
        "	adds r0, r0, #16					\n"/* Move to the high registers. */
        "	ldmia r0!, {r4-r7}					\n"/* Pop the high registers. */
        " 	mov r8, r4							\n"
        " 	mov r9, r5							\n"
        " 	mov r10, r6							\n"
        " 	mov r11, r7							\n"
        "										\n"
        "	msr psp, r0							\n"/* Remember the new top of stack for the task. */
        "										\n"
        "	subs r0, r0, #32					\n"/* Go back for the low registers that are not automatically restored. */
        " 	ldmia r0!, {r4-r7}					\n"/* Pop low registers.  */
        "										\n"
        "	bx r3								\n"
        "										\n"
        "	.align 4							\n"
    );
}

NAKED void leos_platform_start_first(void)
{
    /* The MSP stack is not reset as, unlike on M3/4 parts, there is no vector
     * table offset register that can be used to locate the initial stack value.
     * Not all M0 parts have the application vector table at address 0. */
    __asm volatile (
        "	.syntax unified				\n"
        "	ldr  r2, =currentTCB    	\n"/* Obtain location of pxCurrentTCB. */
        "	ldr  r3, [r2]				\n"
        "	ldr  r0, [r3]				\n"/* The first item in pxCurrentTCB is the task top of stack. */
        "	adds r0, #32				\n"/* Discard everything up to r0. */
        "	msr  psp, r0				\n"/* This is now the new top of stack to use in the task. */
        "	movs r0, #2					\n"/* Switch to the psp stack. */
        "	msr  CONTROL, r0			\n"
        "	isb							\n"
        "	pop  {r0-r5}				\n"/* Pop the registers that are saved automatically. */
        "	mov  lr, r5					\n"/* lr is now in r5. */
        "	pop  {r3}					\n"/* Return address is now in r3. */
        "	pop  {r2}					\n"/* Pop and discard XPSR. */
        "	cpsie i						\n"/* The first task has its context and interrupts can be enabled. */
        "	bx   r3						\n"/* Finally, jump to the user defined task code. */
        "								\n"
        "	.align 4					\n"
        );
}

/*
 * This has got to be platform specific...
 */
uint32_t * leos_platform_init_stack(uint32_t *sp, void (*main)(void *), void *param, void (*taskexit)(void)) {
    /* Simulate the stack frame as it would be created by a context switch
     * interrupt. */
    
    // Shouldn't this actually be ...
    // *--sp = <value> ... if we do that we don't need the "offset"
    //
    
    sp--;                               // Offset because of the way the MCU uses the stack on entry/exit of IRQ's
    *sp-- = INITIAL_XPSR;               // xPSR
    *sp-- = (uint32_t)main;             // PC
    *sp-- = (uint32_t)taskexit;         // If we exit
    sp -= 4;                            // R12, R3, R2, and R1.
    *sp-- = (uint32_t)param;            // R0
    sp -= 7;                            // R11, R10, R9, R8, R7, R6, R5, E4
    return sp;
}

void leos_platform_init() {
    NVIC_SHPR3_REG |= NVIC_PENDSV_PRI;

    // Setup the handler for the PendSV call...
    exception_set_exclusive_handler(PENDSV_EXCEPTION, PendSV_Handler);
}
