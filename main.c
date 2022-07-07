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

volatile int ret;

static volatile uint32_t xx;
static volatile uint32_t id;
static volatile uint32_t rc;

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

static int hex_digit(char ch)
{
    static const char hex_digits[] = "0123456789abcdef";
    char *i = index(hex_digits, tolower(ch));
    if (!i)
        return -1;
    return (int)(i - hex_digits);
}

int hex_byte(char *packet)
{
    int rc;
    int v;

    v = hex_digit(*packet++);
    if (v == -1)
        return -1;
    rc = v << 4;
    v = hex_digit(*packet);
    if (v == -1)
        return -1;
    rc |= v;
    return rc;
}

/**
 * @brief Read in 8 chars and convert into a litte endian word
 *
 */
uint32_t hex_word_le32(char *packet)
{
    uint32_t rc = 0;

    for (int i = 0; i < 4; i++)
    {
        rc >>= 8;
        int v = hex_byte(packet);
        if (v == -1)
            return 0xffffffff;
        packet += 2;
        rc |= (v << 24);
    }
    return rc;
}

uint32_t Xhex_word_be32(char *packet)
{
    uint32_t rc = 0;
    for (int i = 0; i < 8; i++)
    {
        rc <<= 4;
        int v = hex_digit(*packet++);
        if (v == -1)
            return 0xffffffff;
        rc |= v;
    }
    return rc;
}


// State variables and return variables for build_packet
enum
{
    BP_INIT = 0,    // get ready for a new packet
    BP_START,       // waiting for the first char
    BP_DATA,        // we've recevied the $ and are processing
    BP_ESC,         // we've received the escape symbol (next is escaped)
    BP_CHK1,        // first char of checksum
    BP_CHK2,        // second char of checksum

    // Return codes...
    BP_ACK,
    BP_NACK,
    BP_INTR,
    BP_PACKET,
    BP_CORRUPT,
    BP_GARBAGE,
    BP_CHKSUM_FAIL,
    BP_OVERFLOW,
    BP_RUNNING, // just running, all ok
};

#define GDB_BUFFER_SIZE 16384

static char gdb_buffer[GDB_BUFFER_SIZE + 1];
static char *gdb_bp;
static char *gdb_mkr; // end marker location
static int gdb_blen;

static int gdb_noack = 0;

static int gdb_check_for_halt = 0;

static int build_packet()
{
    static int state = BP_INIT;
    static uint8_t checksum;
    int ch;

    // For checking checkum...
    static int supplied_sum;
    int digit;

//    while ((ch = circ_get_byte(xfer)) != -1) {
    while ((ch = io_get_byte()) >= 0) {
        switch (state) {
            case BP_INIT:
                gdb_bp = gdb_buffer;
                gdb_blen = 0;
                checksum = 0;
                // fall through...

            case BP_START:
                if (ch == '+')
                    return BP_ACK;
                if (ch == '-')
                    return BP_NACK;
                if (ch == '$') {
                    state = BP_DATA;
                    break;
                }
                debug_printf("ch=%d\r\n", ch);
                if (ch == 0x3)
                    return BP_INTR;
                return BP_GARBAGE;

            case BP_DATA:
                if (ch == '#') {
                    state = BP_CHK1;
                    break;
                }
                checksum += ch;
                if (ch == '}') {
                    state = BP_ESC;
                    break;
                }
                *gdb_bp++ = ch;
                gdb_blen++;
                break;

            case BP_ESC:
                checksum += ch;
                *gdb_bp++ = ch ^ 0x20;
                gdb_blen++;
                state = BP_DATA;
                break;

            case BP_CHK1:
                *gdb_bp++ = 0; // zero terminate for ease later
                digit = hex_digit(ch);
                if (digit == -1) {
                    state = BP_INIT;
                    return BP_CORRUPT;
                }
                supplied_sum = (digit << 4);
                state = BP_CHK2;
                break;

            case BP_CHK2:
                digit = hex_digit(ch);
                if (digit == -1) {
                    state = BP_INIT;
                    return BP_CORRUPT;
                }
                supplied_sum |= digit;
                if (supplied_sum != checksum) {
                    state = BP_INIT;
                    return BP_CHKSUM_FAIL;
                }
                state = BP_INIT;
                return BP_PACKET;
        }
        if (gdb_blen == GDB_BUFFER_SIZE) {
            debug_printf("BUFFER OVERFLOW\r\n");
            return BP_OVERFLOW;
        }
    }
    if (ch < 0) {
        // TODO: connections etc.
    }
    return BP_RUNNING;
}

static char gen_buffer[1024];


enum
{
    USB_OK = 0, // running normally
    USB_PACKET, // we have a packet
    USB_ERROR,  // some error has occurred
};

/**
 * @brief Decode a "filename:offset:length.." construct
 *
 * Returns 0 on failure, and 1 on success
 *
 * @param packet
 * @param offset
 * @param length
 * @return int
 */
int decode_xfer_read(char *packet, int *offset, int *length)
{
    // Get past the "filename"...
    char *p = strchr(packet, ':');
    if (!p)
        return 0;

    char *sep;
    *offset = strtoul(p + 1, &sep, 16);
    if (*sep != ',')
        return 0;
    *length = strtoul(sep + 1, NULL, 16);
    return 1;
}

void function_qSupported()
{
    int len = sprintf(gen_buffer, "PacketSize=%x;qXfer:memory-map:read+;qXfer:features:read+;qXfer:threads:read+;QStartNoAckMode+;vContSupported+",
                      GDB_BUFFER_SIZE);
    reply(gen_buffer, NULL, 0);
}

/**
 * @brief This will be called when we want a part of something
 *
 * @param packet
 * @param len
 */
void function_xfer_thing(char *packet, char *content, int content_len)
{
    int offset;
    int length;
    char symbol;
    char *p;

    if (!decode_xfer_read(packet, &offset, &length)) {
        reply_err(1);
        return;
    }

    p = content + offset;
    if (offset + length > content_len) {
        length = content_len - offset;
        symbol = 'l';
    } else {
        symbol = 'm';
    }
    reply_part(symbol, p, length);
}

void function_xfer_threads(char *packet)
{
    char *out = malloc(1024);
    if (!out)
    {
        reply_err(1);
        return;
    }
    sprintf(out, "<?xml version=\"1.0\"?>\n<threads>\n"
                 "<thread id=\"1\">Name: rp2040.core0, state: %s</thread>\n"
                 "<thread id=\"2\">Name: rp2040.core1, state: %s</thread>\n"
                 "</threads>\n",
            "breakpoint", "debug-request");
    function_xfer_thing(packet, out, strlen(out));
    free(out);
}

void function_get_sys_regs() {
    char buf[18 * 8];
    char *p = buf;

    for (int i = 0; i <= 16; i++) {
        uint32_t rval;
        int rc;

        rc = reg_read(i, &rval);
        if (rc != SWD_OK)
            panic("reg read failed");
        sprintf(p, "%02x%02x%02x%02x", (uint8_t)(rval & 0xff), (uint8_t)((rval & 0xff00) >> 8), 
                                                (uint8_t)((rval & 0xff0000) >> 16), (uint8_t)(rval >> 24));
        p += 8;
    }
    reply(buf, NULL, 0);
}

void function_get_reg(char *packet) {
    char buf[9];
    uint32_t rval;
    int reg = strtoul(packet, NULL, 16);
    int rc = reg_read(reg, &rval);
    if (rc != SWD_OK)
        panic("reg read failed");
    sprintf(buf, "%02x%02x%02x%02x", (uint8_t)(rval & 0xff), (uint8_t)((rval & 0xff00) >> 8), 
                                            (uint8_t)((rval & 0xff0000) >> 16), (uint8_t)(rval >> 24));
    reply(buf, NULL, 0);
}

void function_put_reg(char *packet) {
    int reg;
    char *sep;
    uint32_t value;
    int rc;

    reg = strtoul(packet, &sep, 16);
    if (*sep != '=') {
        reply_null();
        return;
    }
    value = hex_word_le32(sep + 1);
    rc = reg_write(reg, value);
    if (rc != SWD_OK) {
        reply_err(1);
    } else {
        reply_ok();
    }
}

char *get_two_hex_numbers(char *packet, char sepch, uint32_t *one, uint32_t *two) {
    char *sep;
    char *end;

    *one = strtoul(packet, &sep, 16);
    if (*sep != sepch)
        return NULL;
    *two = strtoul(sep + 1, &end, 16);
    return end;
}

void function_memread(char *packet) {
    uint8_t small_buf[10];
    char *sep;
    uint32_t addr;
    uint32_t len;

    addr = strtoul(packet, &sep, 16);
    if (*sep != ',') {
        reply_null();
        return;
    }
    len = strtoul(sep + 1, NULL, 16);
    if (!len) {
        reply_null();
        return;
    }

    // If we are a small request then use the small buffer...
    if (len <= 4) {
        rc = mem_read_block(addr, len, small_buf);
        reply(NULL, small_buf, len);
        return;
    }

    // Otherwise we'll need to malloc one...
    char *buffer = malloc(len); // make sure we have enough space
    if (!buffer) {
        reply_null();
        return;
    }
    rc = mem_read_block(addr, len, (uint8_t *)buffer);
    reply(NULL, buffer, len);
    free(buffer);
    return;
}

void function_memwrite(char *packet)
{
    uint32_t addr, length;
    int rc;
    char *p = get_two_hex_numbers(packet, ',', &addr, &length);

    if (!p || *p != ':' || !length)
    {
        reply_null();
        return;
    }
    packet = p + 1;

    // Allocate enough space to process our incoming buffer...
    uint8_t *buf = malloc(length);
    if (!buf)
        panic("mem");

    uint8_t *bp = buf;
    // Now process our data...
    for (int i = 0; i < length; i++)
    {
        *bp++ = hex_byte(packet);
        packet += 2;
    }

    // And now write it...
    rc = mem_write_block(addr, length, buf);
    free(buf);
    if (rc != SWD_OK)
    {
        debug_printf("mem write failed\r\n");
        reply_null();
        return;
    }
    reply_ok();
}

void send_stop_packet(int thread, int reason) {
    char buf[16];

    sprintf(buf, "T%02dthread:%d;", reason, thread);
    reply(buf, NULL, 0);
}

// New vCont thinking ...
//
// This is actually a blocking process, so it should start/step the cores
// and then enter a loop to detect if one of them has stopped, then stop
// the other one.
//
// At the same time we need to check for a CTRL-C coming in
//



void function_vcont(char *packet)
{
    enum { CORE_STEP=0, CORE_RUN };
    int cur = core_get();
    int other = 1 - cur;
    int action[2];

    if (*packet == '?')
    {
        static const char vcont[] = "vCont;c;C;s;S";
        reply((char *)vcont, NULL, 0);
        return;
    }
    if (strncmp(packet, ";s:1;c", 6) == 0) {
        action[0] = CORE_STEP;
        action[1] = CORE_RUN;
    } else if (strncmp(packet, ";s:2;c", 6) == 0) {
        action[0] = CORE_RUN;
        action[1] = CORE_STEP;
    } else if (strncmp(packet, ";c", 2) == 0) {
        action[0] = CORE_RUN;
        action[1] = CORE_RUN;
    } else {
        debug_printf("UNRECOGNISED vCONT: %s\r\n", packet);
        return;
    }

    // Something will be running...
    gdb_check_for_halt = 1;

    // We need to ensure the non-stepping core is running first
    // otherwise things like timers may not function properly.
    if (action[other] == CORE_RUN) {
        core_select(other);
        debug_printf("unhalting core\r\n");
        core_unhalt();
    }
    if (action[cur] == CORE_RUN) {
        core_select(cur);
        debug_printf("unhalting core\r\n");
        core_unhalt();
    }
    if (action[other] == CORE_STEP) {
        core_select(other);
        debug_printf("stepping core\r\n");
        core_step();
    }
    if (action[cur] == CORE_STEP) {
        core_select(cur);
        debug_printf("stepping core\r\n");
        core_step();
    }

    core_select(cur);

    while(1) {
        task_sleep_ms(200);
        debug_printf("in loop\r\n");
        int rc = check_cores();
        if (rc != -1) {
            debug_printf("CORE %d has halted\r\n", rc);
            send_stop_packet(rc+1, 5);
            break;
        }
        int ch = io_peek_byte();
        debug_printf("io peek = %d\r\n", ch);
        // If we have CTRL-C then we need to stop ourselves...
        if (io_peek_byte() == 0x03) {
            debug_printf("Have CTRL-C\r\n");
            core_halt();
            continue;
        }
    }
}

static char hex_digits[] = "0123456789abcdef";

void function_rcmd(char *packet, int len)
{
    char *p = packet;

    // Must be an even number of chars...
    if (len & 1)
        goto error;
    len >>= 1;

    // Decode the hex in place (so we don't need more buffers)
    for (int i = 0; i < len; i++)
    {
        int v;
        char *x = index(hex_digits, tolower(*p++));
        if (!x)
            goto error;
        v = ((int)(x - hex_digits)) << 4;
        x = index(hex_digits, tolower(*p++));
        if (!x)
            goto error;
        v |= ((int)(x - hex_digits));
        packet[i] = v;
    }
    debug_printf("HAVE RCMD [%.*s]\r\n", len, packet);

    if (strncmp(packet, "reset halt", 10) == 0)
    {
        core_reset_halt();
        reply_ok();
        return;
    }
    reply_err(1);
    return;

error:
    reply_err(1);
}

void function_XX()
{
    uint32_t addr;
    int rc;

    addr = rp2040_find_rom_func('I', 'F'); // connect_internal_flash
    if (!addr)
    {
        debug_printf("unable to lookup IF\r\n");
        return;
    }
    rc = rp2040_call_function(addr, NULL, 0);
    if (rc != SWD_OK)
    {
        debug_printf("execute IF failed\r\b");
        return;
    }

    addr = rp2040_find_rom_func('F', 'C'); // flush cache
    if (!addr)
    {
        debug_printf("unable to lookup FC\r\n");
        return;
    }
    rc = rp2040_call_function(addr, NULL, 0);
    if (rc != SWD_OK)
    {
        debug_printf("execute FC failed\r\b");
        return;
    }

    addr = rp2040_find_rom_func('C', 'X'); // enter_cmd_xip
    if (!addr)
    {
        debug_printf("unable to lookup CX\r\n");
        return;
    }
    rc = rp2040_call_function(addr, NULL, 0);
    if (rc != SWD_OK)
    {
        debug_printf("execute CX failed\r\b");
        return;
    }
    reply_ok();
}

void function_vflash_erase(char *packet)
{
    uint32_t start, length;
    char *p = get_two_hex_numbers(packet, ',', &start, &length);

    if (!p || !length)
    {
        reply_err(1);
        return;
    }
    reply_ok();
    return;
}

// The write process will just write the data to the memory as a bit of a hack...
// the done will actually process it...
uint32_t Xvflash_ram = 0x20000000;
uint32_t Xvflash_start = 0x10000000;
uint32_t Xvflash_len = 0;

void function_vflash_write(char *packet, int len)
{
    uint32_t start;

    char *sep;
    start = strtoul(packet, &sep, 16);
    if (*sep != ':')
    {
        debug_printf("expecting colon\r\n");
        return;
    }
    sep++;
    int delta = (int)(sep - packet);

    packet += delta;
    len -= delta;
    debug_printf("writing %d bytes to flash at 0x%08x\r\n", len, start);


    // Make sure our source data is word aligned...
    char *p = malloc(len);
    if (!p)
        panic("aarrgg");
    memcpy(p, packet, len);

    debug_printf("writing %d bytes into memory at %08x\r\n", len, p);

    rp2040_add_flash_bit(start & 0x00ffffff, (uint8_t *)p, len);
    free(p);

    reply_ok();
    return;
}

void function_vflash_done() {
    // Flush anything left...
    rp2040_add_flash_bit(0xffffffff, NULL, 0);

    reply_ok();
    return;
}

void function_add_breakpoint(char *packet) {
    uint32_t addr, size;
    if (!get_two_hex_numbers(packet, ',', &addr, &size)) {
        debug_printf("bp set syntax\r\n");
        return;
    }
    bp_set(addr);
    reply_ok();
}
void function_remove_breakpoint(char *packet) {
    uint32_t addr, size;
    if (!get_two_hex_numbers(packet, ',', &addr, &size)) {
        debug_printf("bp clr syntax\r\n");
        return;
    }
    bp_clr(addr);
    reply_ok();
}

int get_threadid(char *packet) {
    return strtol(packet, NULL, 10);
}

void function_thread(char *packet, int packet_size) {
    int tid;

    if (*packet == 'T') {
        // Check for thread existance...
        packet++;
        tid = get_threadid(packet);
        if (tid == 1 || tid == 2) {
            reply_ok();
        } else {
            reply_err(1);
        }        
    } else if (*packet == 'H') {
        int num;
        int rc;
        packet++;

        if (*packet == 'c') {
            // Continue....
            tid = get_threadid(packet+1);
            reply_ok();
            return;
        }
        if (*packet == 'g') { 
            tid = get_threadid(packet+1);
            if (tid == 0 || tid == 1) {
                num = 0;
            } else if (tid == 2) {
                num = 1;
            } else {
                reply_err(1);
                return;
            }
            rc = core_select(num);
            if (rc != SWD_OK) {
                debug_printf("CORE SELECT FAILED\r\n");
                reply_err(1);
            } else {
                reply_ok();
            }
            return;
        }
        reply_err(1);
    }
}

void process_packet(char *packet, int packet_size)
{
    // TODO: checksum
    if (!gdb_noack)
        gdb_printf("+");

    if (strncmp(packet, "vFlashWrite", 11) == 0)
    {
        debug_printf("FLASH WRITE PACKET\r\n");
    }
    else
    {
        debug_printf("PKT [%.*s]\r\n", packet_size, packet);
    }

    if (strncmp(packet, "qSupported:", 11) == 0)
    {
        function_qSupported();
    }
    else if (strncmp(packet, "QStartNoAckMode", 15) == 0)
    {
        reply_ok();
        gdb_noack = 1;
    }
    else if (strncmp(packet, "vMustReplyEmpty", 15) == 0)
    {
        reply_null();
    }
    else if (strncmp(packet, "qXfer:features:read:", 20) == 0)
    {
        function_xfer_thing(packet + 20, (char *)rp2040_features_xml, sizeof(rp2040_features_xml));
    }
    else if (strncmp(packet, "qXfer:memory-map:read:", 22) == 0)
    {
        function_xfer_thing(packet + 22, (char *)rp2040_memory_map_xml, sizeof(rp2040_memory_map_xml));
    }
    else if (strncmp(packet, "qXfer:threads:read:", 19) == 0)
    {
        // function_xfer_thing(packet+19, (char *)rp2040_threads_xml, sizeof(rp2040_threads_xml));
        function_xfer_threads(packet + 19);
    }
    else if (strncmp(packet, "qAttached", 9) == 0)
    {
        reply("1", NULL, 0);
    }
    else if (strncmp(packet, "qSymbol::", 9) == 0)
    {
        reply_ok();
    }
    else if (strncmp(packet, "?", 1) == 0)
    {
        // TODO:!!!!!
        reply("S00", NULL, 0);
    }
    else if (strncmp(packet, "H", 1) == 0)
    {
        // This is thread stuff...
        function_thread(packet, packet_size);
    }
    else if (strncmp(packet, "qC", 2) == 0)
    {
        // RTOS thread?
        if (core_get() == 0) {
            reply("QC0000000000000001", NULL, 0);
        } else {
            reply("QC0000000000000002", NULL, 0);
        }
    }
    else if (strncmp(packet, "qOffsets", 8) == 0)
    {
        static const char offsets[] = "Text=0;Data=0;Bss=0";
        reply((char *)offsets, NULL, 0);
    }
    else if (strncmp(packet, "g", 1) == 0)
    {
        // Get all registers...
        function_get_sys_regs();
    }
    else if (strncmp(packet, "m", 1) == 0)
    {
        function_memread(packet + 1);
    }
    else if (strncmp(packet, "M", 1) == 0)
    {
        function_memwrite(packet + 1);
    }
    else if (strncmp(packet, "p", 1) == 0)
    {
        // Read single register...
        function_get_reg(packet + 1);
    }
    else if (strncmp(packet, "P", 1) == 0)
    {
        // Write single reg...
        function_put_reg(packet + 1);
    }
    else if (strncmp(packet, "Z1,", 3) == 0)
    {
        function_add_breakpoint(packet + 3);
    }
    else if (strncmp(packet, "z1,", 3) == 0)
    {
        function_remove_breakpoint(packet + 3);
    }
    else if (strncmp(packet, "vCont", 5) == 0)
    {
        function_vcont(packet + 5);
    }
    else if (strncmp(packet, "qRcmd,", 6) == 0)
    {
        function_rcmd(packet + 6, packet_size - 6);
    }
    else if (strncmp(packet, "vFlashErase:", 12) == 0)
    {
        function_vflash_erase(packet + 12);
    }
    else if (strncmp(packet, "vFlashWrite:", 12) == 0)
    {
        function_vflash_write(packet + 12, packet_size - 12);
    }
    else if (strncmp(packet, "vFlashDone", 10) == 0)
    {
        function_vflash_done();
    }
    else if (strncmp(packet, "T", 1) == 0) {
        function_thread(packet, packet_size);
    }
    else
    {
        // Not supported...
        reply_null();
    }

    // Really need to copy buffer along if there's any left

    gdb_bp = gdb_buffer;
    gdb_blen = 0;
    gdb_mkr = NULL;
}

void handle_intr()
{
    core_halt();
    gdb_check_for_halt = 1;
}

int usb_poll()
{
    int rc;

    //tud_task();
    //refill_from_usb();
    rc = build_packet();
    if (rc != BP_RUNNING)
    {
        switch (rc)
        {
        case BP_PACKET:
            process_packet(gdb_buffer, gdb_blen);
            break;
        case BP_INTR:
            handle_intr();
            break;
        case BP_CORRUPT:
            debug_printf("CORRUPT\r\n");
            break;
        case BP_GARBAGE:
            debug_printf("GARBAGE [%.*s]\r\n", gdb_blen, gdb_buffer);
            break;
        case BP_ACK:
            debug_printf("ACK\r\n");
            break;
        case BP_NACK:
            debug_printf("NACK\r\n");
            break;
        case BP_CHKSUM_FAIL:
            debug_printf("CHKSUM FAIL\r\n");
        default:
            debug_printf("RC=%d\r\n", rc);
        }
    }
    //    if (usb_read_poll() == USB_PACKET) {
    //        int length = gdb_mkr - gdb_buffer;
    //        process_packet(gdb_buffer, length);
    //    }
    return 0;
}

void gdb_init()
{
    gdb_bp = gdb_buffer;
    gdb_blen = 0;
}



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

DEFINE_TASK(xtest1, 1024);

void func_test(void *arg) {
    debug_printf("HERE\r\n");
    while(1) {
        //task_sleep_ms(10);
        usb_poll();
    }
}

int main() {
    // Take us to 150Mhz (for future rmii support)
    set_sys_clock_khz(150 * 1000, true);

    if (swd_init() != SWD_OK)
        panic("unable to init SWD");
    if (dp_init() != SWD_OK)
        panic("unable to init DP");

    // This will be core 0...
//    core_enable_debug();
    core_reset_halt();

    // Now core 1...
    core_select(1);
//    core_enable_debug();
    core_reset_halt();

    // Back to the first one again...
    core_select(0);
//    core_unhalt();
//    sleep_ms(200);
//    core_halt();

    tusb_init();
    gdb_init();

    CREATE_TASK(xtest1, func_test, NULL);

    leos_init(main_poll);
    leos_start();

    //    sleep_ms(100);
    //    if (core_reset_halt() != SWD_OK) panic("failed reset");
    //    sleep_ms(10);
    //
    //    swd_test();

    //    uint32_t revaddr = rp2040_find_rom_func('R', '3');
    //    uint32_t args[1] = { 0x11002233 };
    //
    //    uint32_t xx = rp2040_call_function(revaddr, args, 1);

    while (1)
    {
        // busy_wait_ms(10);
        usb_poll();

        // Not right in here ... needs to be somewhere else.
        if (gdb_check_for_halt)
        {
            int rc = check_cores();
            if (rc != -1) {
                debug_printf("CORE %d has halted\r\n");
                send_stop_packet(rc+1, 5);
                gdb_check_for_halt = 0;
            }
        }
    }
}
