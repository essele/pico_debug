/**
 * @file leos_debug.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-04-15
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <stdarg.h>
#include <string.h>
#include "lerp/circ.h"
#include "lerp/debug.h"

#if defined DEBUG_UART || defined DEBUG_CDC || defined DEBUG_BUF

#ifdef DEBUG_CDC
#include "tusb.h"
#endif

CIRC_DEFINE(circ_debug, 4096);

/**
 * @brief Send output to the debug UART (if debugging is enabled)
 * 
 * This sends to the circular buffer and requires debug_poll() to be
 * regularly called to ensure it's transmitted.
 * 
 * @param format 
 * @param ... 
 */
int debug_printf(char *format, ...) {
    int len = 0;
    va_list args;

    va_start(args, format);
    len = circ_aprintf(circ_debug, format, args);
    va_end(args);
    return len;
}

/**
 * @brief We may want to mirror something on a char by char basis
 * 
 * @param ch 
 * @return int 
 */
void debug_putch(char ch) {
    circ_add_byte(circ_debug, ch);
}

/**
 * @brief Send any contents of the debug circular buffer to the UART
 * 
 * This is non blocking so will only send if there is space to do so.
 * 
 * @return int zero if no more chars are waiting to be sent
 */
int debug_poll() {
#ifdef DEBUG_UART
    while (circ_has_data(circ_debug)) {
        if (!uart_is_writable(DEBUG_PORT)) return 1;
        uart_get_hw(DEBUG_PORT)->dr = circ_get_byte(circ_debug);
    }
#endif
#ifdef DEBUG_CDC
    int need_flush = 0;
    if (!tud_cdc_n_connected(DEBUG_CDC)) return 0;

    while (circ_has_data(circ_debug)) {
        if (!tud_cdc_n_write_available(DEBUG_CDC)) return 1;
        tud_cdc_n_write_char(DEBUG_CDC, circ_get_byte(circ_debug));
        need_flush = 1;
    }
    if (need_flush) tud_cdc_n_write_flush(DEBUG_CDC);
#endif
    return 0;
}

/**
 * @brief Initialise the debug UART and setup the circular buffer
 * 
 */
void debug_init() {
#ifdef DEBUG_UART
    uart_init(DEBUG_UART, DEBUG_UART_BAUD);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(DEBUG_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(DEBUG_UART_RX_PIN, GPIO_FUNC_UART);
#endif
}

/**
 * @brief Imediately flush any debug output, waiting until we have sent it
 * 
 */
void debug_flush() {
    while (debug_poll());
}

/**
 * @brief Called in a fatal situation with useful debug information
 * 
 * Will push the info into the circ buffer and then loop running flush
 * so that we output the data if a connection is made. Used in conjunction
 * with the lerp_panic() macro to get file and line information
 * 
 * @param file 
 * @param line 
 * @param format 
 * @param ... 
 */
void _lerp_panic(char *file, int line, char *format, ...) {
    va_list args;

    circ_printf(circ_debug, "PANIC: %s:%d: ", file, line);
    va_start(args, format);
    circ_aprintf(circ_debug, format, args);
    va_end(args);

    while(1) {
        debug_poll();
    }
}

#endif // DEBUG_PORT
