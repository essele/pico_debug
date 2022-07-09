
#include "pico/printf.h"
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
CIRC_DEFINE(incoming, XFER_CIRC_SIZE);
CIRC_DEFINE(outgoing, XFER_CIRC_SIZE);

// The task waiting for input (or NULL if there isn't one)
struct task *waiting_on_input = NULL;
struct task *waiting_on_output = NULL;


static int refill_from_usb()
{
    int count = 0;

    // Track disconnects...
    if (!tud_cdc_connected()) {
        if (waiting_on_input) {
            task_wake(waiting_on_input, IO_DISCONNECT);
            waiting_on_input = NULL;
        }
        return 0;
    }

    // If we go around the end we'll need to call this twice...
    int space = circ_space_before_wrap(incoming);
    if (space)
    {
        int avail = tud_cdc_available();
        if (avail)
        {
            int size = MIN(space, avail);
            count = tud_cdc_read(incoming->head, size);
            circ_advance_head(incoming, count);
        }
    }
    if (count && waiting_on_input) {
        task_wake(waiting_on_input, IO_DATA);
        waiting_on_input = NULL;
    }
    return count;
}

int io_is_connected() {
    return tud_cdc_connected();
}

int io_get_byte() {
    int reason;

    if (circ_is_empty(incoming)) {
        waiting_on_input = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    return circ_get_byte(incoming);
}

int io_peek_byte() {
    if (circ_is_empty(incoming)) return -1;
    return *(incoming->tail);
}

int io_put_byte(uint8_t ch) {
    int reason;

    if (circ_is_full(outgoing)) {
        waiting_on_output = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    // debug
    if (tud_cdc_n_connected(1)) {
        tud_cdc_n_write_char(1, ch);
    }
    circ_add_byte(outgoing, ch);
    return 0;
}


static void send_to_usb() {
    int have = circ_bytes_before_wrap(outgoing);
    if (!have) return;
    int space = tud_cdc_write_available();
    if (!space) return;

    while (space && have) {
        int size = MIN(have, space);
        int count = tud_cdc_write(outgoing->tail, size);
        circ_advance_tail(outgoing, count);
        space -= count;
        have = circ_bytes_before_wrap(outgoing);
    }
    tud_cdc_write_flush();
    // If we get here then we must have sent something so we
    // will have some space...
    if (waiting_on_output) {
        task_wake(waiting_on_output, IO_DATA);
        waiting_on_output = NULL;
    }
}

int io_put_hexbyte(uint8_t b) {
    static const char hexdigits[] = "0123456789ABCDEF";
    uint8_t sum = 0;
    uint8_t ch;
    
    ch = hexdigits[b >> 4];
    sum += ch;
    io_put_byte(ch);
    ch = hexdigits[b & 0xf];
    sum += ch;
    io_put_byte(ch);
    return sum;
}

int reply(char *text, uint8_t *hex, int hexlen) {
    uint8_t sum = 0;

    io_put_byte('$');

    if (text) {
        char *p = text;
        while (*p) {
            sum += *p;
            io_put_byte(*p++);
        }
    }
    if (hex) {
        uint8_t *p = hex;
        while (hexlen--) {
            sum += io_put_hexbyte(*p++);
        }
    }
    io_put_byte('#');
    io_put_hexbyte(sum);
    // For debug (remove this)
    if (tud_cdc_n_connected(1)) { tud_cdc_n_write_char(1, '\r'); tud_cdc_n_write_char(1, '\n'); }
    return 0;
}
int reply_part(char ch, char *text, int len) {
    uint8_t sum = ch;

    io_put_byte('$');
    io_put_byte(ch);
    while (len--) {
        sum += *text;
        io_put_byte(*text++);
    }
    io_put_byte('#');
    io_put_hexbyte(sum);
    // For debug (remove this)
    if (tud_cdc_n_connected(1)) { tud_cdc_n_write_char(1, '\r'); tud_cdc_n_write_char(1, '\n'); }
    return 0;
}
int reply_null() {
    return reply(NULL, NULL, 0);
}

int reply_ok() {
    return reply("OK", NULL, 0);
}

int reply_err(uint8_t err) {
    return reply("E", &err, 1);
}

static void _reply_out(char ch, void *arg) {
    uint8_t *sp = (uint8_t *)arg;

    *sp += ch;
    io_put_byte(ch);
}

int reply_printf(char *format, ...) {
    uint8_t sum = 0;
    int len;

    io_put_byte('$');

    va_list args;
    va_start(args, format);
    len = vfctprintf(_reply_out, &sum, format, args);
    va_end(args);

    io_put_byte('#');
    io_put_hexbyte(sum);

    // For debug (remove this)
    if (tud_cdc_n_connected(1)) { tud_cdc_n_write_char(1, '\r'); tud_cdc_n_write_char(1, '\n'); }

    return len;
}


void io_poll() {
    tud_task();
    refill_from_usb();
    send_to_usb();
}
