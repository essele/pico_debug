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
#include "lerp/io.h"
#include "uart.h"
#include "tusb.h"

static struct io *uart_io = NULL;

/**
 * @brief Handle the UART data sending across to USB or Network
 * 
 * The network side doesn't give us the abilty to dynamically change the
 * baud rate, and for some reason it seems to keep switching to 9600, not
 * 100% sure why. So let's reset everytime we get a net connection.
 * 
 */
void dbg_uart_poll() {
    static int uart_is_conected = 0;

    // If we don't have a connection we simply consume...
    if (!io_is_connected(uart_io)) {
        uart_is_conected = 0;
        while (uart_is_readable(UART_INTERFACE)) {
            uart_get_hw(UART_INTERFACE)->dr;
        }
    } else {
        // If we are a new network connection then reset the baud rate...
        if (!uart_is_conected && (uart_io->pcb)) {
            uart_is_conected = 1;
            uart_init(UART_INTERFACE, UART_BAUD);
        }
        while (uart_is_readable(UART_INTERFACE) && circ_space(uart_io->output)) {
            circ_add_byte(uart_io->output, uart_get_hw(UART_INTERFACE)->dr);
        }
        while (uart_is_writable(UART_INTERFACE) && circ_has_data(uart_io->input)) {
                uart_get_hw(UART_INTERFACE)->dr = circ_get_byte(uart_io->input);
        }
    }
}

/**
 * @brief Handle baud rate changes...
 * 
 * @param itf 
 * @param line_coding 
 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* line_coding) {
    if (uart_io->usb_is_connected && (itf == UART_CDC)) {
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

    uart_io = io_init(UART_CDC, UART_TCP, 256);
    uart_io->support_telnet = 1;
}