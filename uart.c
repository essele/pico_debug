/**
 * @file uart.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-07-12
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "lerp/debug.h"
#include "uart.h"
#include "tusb.h"

/**
 * @brief Simply transfer data between the UART and CDC port 1
 * 
 * Is there a reason why we should consume the fifo even if there's no connection?
 * I'm not sure what there would be? It will just be overritten?
 */
void dbg_uart_poll() {
    int need_cdc_flush = 0;

    // We only do stuff if we have a connection...
    if (!tud_cdc_n_connected(UART_CDC)) return;

    // From UART to CDC...
    while (uart_is_readable(UART_INTERFACE) && tud_cdc_n_write_available(UART_CDC)) {
        tud_cdc_n_write_char(UART_CDC, uart_get_hw(UART_INTERFACE)->dr);
        need_cdc_flush = 1;
    }
    if (need_cdc_flush) tud_cdc_n_write_flush(UART_CDC);

    // From CDC to UART...
    while (tud_cdc_n_available(UART_CDC) && uart_is_writable(UART_INTERFACE)) {
       uart_get_hw(UART_INTERFACE)->dr = tud_cdc_n_read_char(UART_CDC);
    }
}

/**
 * @brief Handle baud rate changes...
 * 
 * @param itf 
 * @param line_coding 
 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* line_coding) {
    if (itf == UART_CDC) {
        debug_printf("UART baud rate change: %d\r\n", line_coding->bit_rate);
        uart_init(UART_INTERFACE, line_coding->bit_rate);
    }
}

/**
 * @brief Initialise the UART interface to the default settings
 * 
 */
void dbg_uart_init() {
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    uart_init(UART_INTERFACE, UART_BAUD);
}