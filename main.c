#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "pico/printf.h"

#include "lerp/task.h"
#include "lerp/circ.h"

#include "swd.h"
#include "adi.h"
#include "flash.h"

#include "tusb.h"
#include "filedata.h"

#include "io.h"

#include "gdb.h"

/**
 * @brief Output routine for usb_cdc_printf
 *
 * This blocks if we have no space to write into the usb buffer
 * also doesn't do anything if the connection is gone.
 *
 * @param ch
 * @param arg
 */
static void _cdc_out(char ch, void *arg)
{
    int n = (int)arg;

    // We could become unconnected mid-proint...
    if (tud_cdc_n_connected(n))
    {
        while (!tud_cdc_n_write_available(n))
            tud_task();
        tud_cdc_n_write_char(n, ch);
    }
}
int usb_n_printf(int n, char *format, ...)
{
    va_list args;
    int len;

    va_start(args, format);
    len = vfctprintf(_cdc_out, (void *)n, format, args);
    va_end(args);
    tud_cdc_n_write_flush(n);
    tud_task();
    return len;
}

#define debug_printf(...) usb_n_printf(1, __VA_ARGS__)
#define gdb_printf(...) usb_n_printf(0, __VA_ARGS__)




enum
{
    USB_OK = 0, // running normally
    USB_PACKET, // we have a packet
    USB_ERROR,  // some error has occurred
};




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
}


int main() {
    // Take us to 150Mhz (for future rmii support)
    set_sys_clock_khz(150 * 1000, true);

    if (swd_init() != SWD_OK)
        panic("unable to init SWD");

    // Initialise the USB stack...
    tusb_init();

    // Create the GDB server task..
    gdb_init();

    // And start the scheduler...
    leos_init(main_poll);
    leos_start();
}
