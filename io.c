

#include "lerp/task.h"
#include "lerp/circ.h"
#include "io.h"
#include "tusb.h"

/**
 * Input mechanism -- needs to support both USB and Ethernet, and unfortunately
 * one of those works as a pull and one as a push.
 *
 * So... use a circular buffer for input which we refill from usb and the ethernet
 * stack fills as packets come in.
 *
 */

#define XFER_CIRC_SIZE 4096
CIRC_DEFINE(xfer, XFER_CIRC_SIZE);

// The task waiting for input (or NULL if there isn't one)
struct task *waiting_on_input = NULL;


static int refill_from_usb()
{
    int count = 0;

    if (!tud_cdc_connected())
        return 0;

    // If we go around the end we'll need to call this twice...
    int space = circ_space_before_wrap(xfer);
    if (space)
    {
        int avail = tud_cdc_available();
        if (avail)
        {
            int size = MIN(space, avail);
            count = tud_cdc_read(xfer->head, size);
            circ_advance_head(xfer, count);
        }
    }
    if (count && waiting_on_input) {
        task_wake(waiting_on_input, IO_DATA);
        waiting_on_input = NULL;
    }
    return count;
}

int io_get_byte() {
    int reason;

    if (circ_is_empty(xfer)) {
        waiting_on_input = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    return circ_get_byte(xfer);
}

void io_poll() {
    tud_task();
    refill_from_usb();
}
