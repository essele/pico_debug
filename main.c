#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "pico/printf.h"

#include "lerp/task.h"
#include "lerp/circ.h"
#include "lerp/debug.h"

#include "swd.h"
#include "adi.h"
#include "flash.h"
#include "uart.h"
#include "tusb.h"
#include "filedata.h"
#include "io.h"
#include "gdb.h"

/**
 * @brief Main polling function called regularly by lerp_task
 * 
 * We need to do time sensitive things here and we mustn't block.
 * 
 */
void main_poll() {
    // make sure usb is running and we're processing data...
    io_poll();

    // make sure the PIO blocks are managed...
    swd_pio_poll();

    // see if we need to transfer any uart data
    dbg_uart_poll();

    // handle any debug output...
    debug_poll();
}


int main() {
    // Take us to 150Mhz (for future rmii support)
    set_sys_clock_khz(150 * 1000, true);

    // Initialise the USB stack...
    tusb_init();

    // Initialise the PIO SWD system...
    if (swd_init() != SWD_OK)
        lerp_panic("unable to init SWD");

    // Initialise the UART
    dbg_uart_init();

    // Create the GDB server task..
    gdb_init();

    // And start the scheduler...
    leos_init(main_poll);
    leos_start();
}
