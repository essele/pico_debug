/**
 * @file leos_debug.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-04-15
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef __LERP_DEBUG_H
#define __LERP_DEBUG_H

// Simple debug output system, uses a circular buffer to store the output and
// then a polling mechanism that will send the output to the serial port when
// space is avaailable.
//
// We control the output mechanism depending on what define is defined...
//
// DEBUG_UART (=uart0 or =uart1)
// DEBUG_CDC (=0, 1, 2, etc)
// DEBUG_BUF (just buffer it, something else will collect it)

#if defined DEBUG_UART || defined DEBUG_CDC || defined DEBUG_BUF

typedef unsigned long uint32_t;

// Init and poll are used by leos...
void debug_init();
int debug_poll();

// Printf and flush are usable for the app...
int debug_printf(char *format, ...);
void debug_putch(char ch);
void debug_flush();

// Supporting the panic situation...
void _lerp_panic(char *file, int line, char *format, ...);
#define lerp_panic(...)     _lerp_panic(__FILE__, __LINE__, __VA_ARGS__)

#else // DEBUG_UART or DEBUG_CDC

#define debug_printf(...)
#define debug_putch(...)
#define debug_flush()
#define debug_init()
#define debug_poll()
#define lerp_panic(...)     panic("")

#endif // DEBUG_UART or DEBUG_CDC
#endif // __LERP_DEBUG_H

