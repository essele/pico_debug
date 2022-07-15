
#include "pico/stdlib.h"
#include "pico/printf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "gdb.h"
#include "swd.h"
#include "adi.h"
#include "lerp/io.h"
#include "lerp/task.h"
#include "utils.h"
#include "flash.h"
#include "breakpoint.h"

#include "lerp/debug.h"
#include "lerp/io.h"

#include "filedata.h"



#define GDB_BUFFER_SIZE 16384

static char gdb_buffer[GDB_BUFFER_SIZE + 1];
static char *gdb_bp;
static int gdb_blen;
static int gdb_noack = 0;
static int gdb_intr = 0;        // have we received an interrupt?

struct io *gdb_io = NULL;       // the IO structure for GDB

// -------------------------------------------------------------------------------------
// We look at the gdb packet and use the first letter to work out what to do, where this
// could be one of many items we use lookup tables to match the first part of the packet
// to a given function.
// -------------------------------------------------------------------------------------
typedef void (*gdbfunc)(char *packet, int packet_size, void *ptr, int num);

struct gdbitem {
    char        *match;         // string to match
    int         matchlen;       // how long it that
    gdbfunc     function;       // what function to call
    void        *ptr;           // optional pointer argument
    int         num;            // optional int argument
};

// NOTE: the tables are after the function sections...
#define UNUSED              __attribute__ ((unused))
#define GDBFUNC(name)      void function_##name(UNUSED char *packet, UNUSED int packet_size, UNUSED void *ptr, UNUSED int num)

// --------------------------------------------------------------------------
// This is a state machine for processing the incoming GDB packet data
// --------------------------------------------------------------------------

// State variables and return variables for build_packet
enum {
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
    BP_DISCONNECT,
    BP_RUNNING,     // just running, all ok
};


static int build_packet() {
    static int state = BP_INIT;
    static uint8_t checksum;
    int ch;

    // For checking checkum...
    static int supplied_sum;
    int digit;

    while ((ch = io_get_byte(gdb_io)) >= 0) {
        switch (state) {
            case BP_INIT:
                gdb_bp = gdb_buffer;
                gdb_blen = checksum = 0;
                // fall through...

            case BP_START:
                if (ch == '+') return BP_ACK;
                if (ch == '-') return BP_NACK;
                if (ch == '$') { state = BP_DATA; break; }
                if (ch == 0x3) return BP_INTR;
                return BP_GARBAGE;

            case BP_DATA:
                if (ch == '#') { state = BP_CHK1; break; }
                checksum += ch;
                if (ch == '}') { state = BP_ESC; break; }
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
                if (digit == -1) { state = BP_INIT; return BP_CORRUPT; }
                supplied_sum = (digit << 4);
                state = BP_CHK2;
                break;

            case BP_CHK2:
                digit = hex_digit(ch);
                if (digit == -1) { state = BP_INIT; return BP_CORRUPT; }
                supplied_sum |= digit;
                if (supplied_sum != checksum) { state = BP_INIT; return BP_CHKSUM_FAIL; }
                state = BP_INIT;
                return BP_PACKET;
        }
        if (gdb_blen == GDB_BUFFER_SIZE) {
            debug_printf("BUFFER OVERFLOW\r\n");
            return BP_OVERFLOW;
        }
    }
    if (ch == IO_DISCONNECT) {
        state = BP_INIT;
        return BP_DISCONNECT;
    }
    return BP_RUNNING;
}


// -----------------------------------------------------------------------------
// The Various Reply Functions (using io)
// -----------------------------------------------------------------------------

int reply(char *text, uint8_t *hex, int hexlen) {
    uint8_t sum = 0;

    io_put_byte(gdb_io, '$');

    if (text) {
        char *p = text;
        while (*p) {
            sum += *p;
            io_put_byte(gdb_io, *p++);
        }
    }
    if (hex) {
        uint8_t *p = hex;
        while (hexlen--) {
            sum += io_put_hexbyte(gdb_io, *p++);
        }
    }
    io_put_byte(gdb_io, '#');
    io_put_hexbyte(gdb_io, sum);
    return 0;
}
int reply_part(char ch, char *text, int len) {
    uint8_t sum = ch;

    io_put_byte(gdb_io, '$');
    io_put_byte(gdb_io, ch);
    while (len--) {
        sum += *text;
        io_put_byte(gdb_io, *text++);
    }
    io_put_byte(gdb_io, '#');
    io_put_hexbyte(gdb_io, sum);
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
    io_put_byte(gdb_io, ch);
}

int reply_printf(char *format, ...) {
    uint8_t sum = 0;
    int len;

    io_put_byte(gdb_io, '$');

    va_list args;
    va_start(args, format);
    len = vfctprintf(_reply_out, &sum, format, args);
    va_end(args);

    io_put_byte(gdb_io, '#');
    io_put_hexbyte(gdb_io, sum);
    return len;
}

/**
 * @brief Decode a "filename:offset,length.." construct
 *
 * Returns 0 on failure, and 1 on success
 *
 * @param packet
 * @param offset
 * @param length
 * @return int
 */
int decode_xfer_read(char *packet, int *offset, int *length) {
    // Get past the "filename"...
    char *p = strchr(packet, ':');
    if (!p) return 0;

    char *sep;
    *offset = strtoul(p + 1, &sep, 16);
    if (*sep != ',') return 0;
    *length = strtoul(sep + 1, NULL, 16);
    return 1;
}


void function_get_sys_regs() {
    static char *buf = NULL;

    if (!buf) {     // one-off malloc the first time (this allows us to potentally be more size flexible)
        buf = malloc(18 * 8);
        if (!buf) lerp_panic("out of memory");
    }
    char *p = buf;

    for (int i = 0; i <= 16; i++) {
        uint32_t rval;
        int rc;

        rc = reg_read(i, &rval);
        if (rc != SWD_OK) { reply_err(1); return; }
        sprintf(p, "%02x%02x%02x%02x", (uint8_t)(rval & 0xff), (uint8_t)((rval & 0xff00) >> 8), 
                                                (uint8_t)((rval & 0xff0000) >> 16), (uint8_t)(rval >> 24));
        p += 8;
    }
    reply(buf, NULL, 0);
}

void function_get_reg(char *packet) {
    uint32_t rval;
    int reg = strtoul(packet, NULL, 16);
    int rc = reg_read(reg, &rval);
    if (rc != SWD_OK) { reply_err(1); return; }
    reply_printf("%02x%02x%02x%02x", (uint8_t)(rval & 0xff), (uint8_t)((rval & 0xff00) >> 8), 
                                            (uint8_t)((rval & 0xff0000) >> 16), (uint8_t)(rval >> 24));
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


void function_memread(char *packet) {
    uint8_t small_buf[10];
    uint32_t addr, len;
    int rc;

    if (!get_two_hex_numbers(packet, ',', &addr, &len)) {
        reply_null();
        return;
    }

    // If we are a small request then use the small buffer...
    if (len <= 4) {
        rc = mem_read_block(addr, len, small_buf);
        if (rc != SWD_OK) { reply_err(1); return; }
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
    if (rc != SWD_OK) { reply_err(1); return; }
    reply(NULL, (uint8_t *)buffer, len);
    free(buffer);
    return;
}

void function_memwrite(char *packet)
{
    uint32_t addr, length;
    int rc;
    char *p = get_two_hex_numbers(packet, ',', &addr, &length);

    if (!p || *p != ':' || !length) {
        reply_null();
        return;
    }
    packet = p + 1;

    // Process the hex into our gdb_buffer so we don't have to malloc
    // (we're well before the hex so will fit fine)
    uint8_t *bp = (uint8_t *)gdb_buffer;
    // Now process our data...
    for (int i = 0; i < length; i++) {
        *bp++ = hex_byte(packet);
        packet += 2;
    }

    // And now write it...
    rc = mem_write_block(addr, length, (uint8_t *)gdb_buffer);
    if (rc != SWD_OK) { reply_err(1); return; }
    reply_ok();
}

int reason_to_stopcode(int reason) {
    switch (reason) {
        case REASON_DBGRQ:          return (0x02);
        case REASON_BREAKPOINT:
        case REASON_WATCHPOINT:
        case REASON_WPTANDBKPT: 
        case REASON_SINGLESTEP:
        case REASON_EXC_CATCH:      return (0x05);
        case REASON_NOTHALTED:      return (0x00);
    }
    lerp_panic("unknown reason code %d\r\n", reason);
    return 0;
}

/**
 * @brief Called when we detect a stopped core, reports status back
 * 
 * This also keeps an eye on whether we've received an interrupt request
 * so we can adjust the reason accordingly (stops runaway stepping sessions.)
 * 
 * @param thread 
 * @param reason 
 */
void send_stop_packet(int thread, int reason) {
    if (gdb_intr) {
        reason = REASON_DBGRQ;
        gdb_intr = 0;
    }
    reply_printf("T%02dthread:%d;", reason_to_stopcode(reason), thread);
}


// -----------------------------------------------------------------------------------------------
// "v"" Related Packets (v)
// -----------------------------------------------------------------------------------------------



// New vCont thinking ...
//
// This is actually a blocking process, so it should start/step the cores
// and then enter a loop to detect if one of them has stopped, then stop
// the other one.
//
// At the same time we need to check for a CTRL-C coming in
//



GDBFUNC(vCont) {
    enum { CORE_STEP=0, CORE_RUN };
    int cur = core_get();
    int other = 1 - cur;
    int action[2];

    if (*packet == '?') {
        static const char vcont[] = "vCont;c;C;s;S";
        reply((char *)vcont, NULL, 0);
        return;
    }
    if (strncmp(packet, ";s:1;c", 6) == 0 || strncmp(packet, ";s:1", 4) == 0) {
        action[0] = CORE_STEP;
        action[1] = CORE_RUN;
    } else if (strncmp(packet, ";s:2;c", 6) == 0 || strncmp(packet, ";s:2", 4) == 0) {
        action[0] = CORE_RUN;
        action[1] = CORE_STEP;
    } else if (strncmp(packet, ";c", 2) == 0) {
        action[0] = CORE_RUN;
        action[1] = CORE_RUN;
    } else {
        debug_printf("UNRECOGNISED vCONT: %s\r\n", packet);
        return;
    }

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

    // We now loop waiting for a core to stop ... during this we need to check for INTR input
    // or a loss of connection...
    while(1) {
        int rc = check_cores();
        if (rc != -1) {
            debug_printf("CORE %d has halted\r\n", rc);
            send_stop_packet(rc+1, core_get_reason(rc));
            break;
        }
        if (!io_is_connected(gdb_io)) {
            debug_printf("LOST CONNECTION\r\n");
            core_halt();
            return;
        }
        // A simple yield here potentially doesn't give enough time to output
        // pending debug etc. So nicer to have a small sleep. A few ms really shouldn't
        // impact performance.
        task_sleep_ms(2);;
        // If we have CTRL-C then we need to stop ourselves...
        if (io_peek_byte(gdb_io) == 0x03) {
            debug_printf("Have CTRL-C\r\n");
            core_halt();
            continue;
        }
    }
}


GDBFUNC(vFlashWrite) {
    uint32_t start;
    int len = packet_size;

    char *sep;
    start = strtoul(packet, &sep, 16);
    if (*sep != ':') {
        debug_printf("expecting colon\r\n");
        return;
    }
    sep++;
    int delta = (int)(sep - packet);

    packet += delta;
    len -= delta;

    // TODO: look at whether we really need to do this ... does it actually make a
    // difference ... if it does, then maybe a memmove back a few bytes will be better.

    // Make sure our source data is word aligned...
    char *p = malloc(len);
    if (!p)
        lerp_panic("aarrgg");
    memcpy(p, packet, len);
    rp2040_add_flash_bit(start & 0x00ffffff, (uint8_t *)p, len);
    free(p);

    reply_ok();
}

GDBFUNC(vFlashDone) {
    // Flush anything left...
    rp2040_add_flash_bit(0xffffffff, NULL, 0);
    reply_ok();
}

GDBFUNC(null)   { reply_null(); }
GDBFUNC(ok)     { reply_ok(); }

static const struct gdbitem gdb_v_items[] = {
    { "vMustReplyEmpty", 15, function_null, NULL, 0 },
    { "vCont", 5, function_vCont, NULL, 0 },
    { "vFlashErase:", 12, function_ok, NULL, 0 },
    { "vFlashWrite:", 12, function_vFlashWrite, NULL, 0 },
    { "vFlashDone", 10, function_vFlashDone, NULL, 0 },
    { NULL, 0, NULL, NULL, 0 },
};

// -----------------------------------------------------------------------------------------------
// Thread Related Packets (q)
// -----------------------------------------------------------------------------------------------

int get_threadid(char *packet) {
    return strtol(packet, NULL, 16); 
}
int thread_to_core(int thread) {
    return thread-1;
}

void function_thread_valid(char *packet, int packet_size) {
    int tid = get_threadid(packet);
    if (tid == 1 || tid == 2) reply_ok();
    reply_err(1);
}

GDBFUNC(Hc) {       // This is really deprecated (replaced by vCont) so just reply ok.
    reply_ok();
}
GDBFUNC(Hg) {       // Actually do a core switch
    int rc;
    int tid = get_threadid(packet);
    if (tid == 0) tid = 1;
    if ((tid < 1) || (tid > 2)) { reply_err(1); return; }
    rc = core_select(thread_to_core(tid));
    if (rc != SWD_OK) { reply_err(1); return; }
    reply_ok();
}

static const struct gdbitem gdb_H_items[] = {
    { "Hg", 2, function_Hg, NULL, 0 },
    { "Hc", 2, function_Hc, NULL, 0 },
    { NULL, 0, NULL, NULL, 0 },
};

// -----------------------------------------------------------------------------------------------
// General Query Packets (q)
// -----------------------------------------------------------------------------------------------

typedef char *(*xfer_func)(int *len);

char *xfer_features(int *len) {
    *len = sizeof(rp2040_features_xml);
    return (char *)rp2040_features_xml;
}
char *xfer_memory_map(int *len) {
    *len = sizeof(rp2040_memory_map_xml);
    return (char *)rp2040_memory_map_xml;
}
char *xfer_threads(int *len) {
    static const char *states[] = { "debug-request", "breakpoint", "watchpoint", 
                                    "breakpoint-and-watchpoint", "single-step", 
                                    "target-not-halted", "program-exit", "exception-catch",
                                    "undefined" };
    static char *out = NULL;
    int r0 = core_get_reason(0);
    int r1 = core_get_reason(1);
    
    if (!out) out = malloc(1024);       // TODO: fix this
    if (!out) lerp_panic("no memory");
    *len = sprintf(out, "<?xml version=\"1.0\"?>\n<threads>\n"
                 "<thread id=\"1\">Name: rp2040.core0, state: %s</thread>\n"
                 "<thread id=\"2\">Name: rp2040.core1, state: %s</thread>\n"
                 "</threads>\n",
            states[r0], states[r1]);
    return out;
}

GDBFUNC(qC) { reply_printf("QC%08x", core_get() + 1); }
GDBFUNC(qAttached) { reply("1", NULL, 0); }
GDBFUNC(qSupported) {
    reply_printf("PacketSize=%x;qXfer:memory-map:read+;qXfer:features:read+;"
                                "qXfer:threads:read+;QStartNoAckMode+;vContSupported+",
                                        GDB_BUFFER_SIZE);
}
GDBFUNC(qOffsets) { reply("Text=0;Data=0;Bss=0", NULL, 0); }

// TODO: this is a temporary hack to support run_to_main
static uint32_t symbol_main = 0;

GDBFUNC(qSymbol) {
    // This is GDB either telling us it's prepared to serve symbols or a response to a previous
    // reuqest. Format is qSymbol:: or qSymbol:hex_value:hex_name
    if (*packet == ':' && packet_size == 1) {
        // Initial qSymbol:: notification ... we want the value of main
        reply("qSymbol:", (uint8_t *)"main", 4);
        return;
    }
    if (*packet == ':') {
        // We didn't get a value
        reply_ok();
        return;
    }
    // This is a response... we'll assume it's for what we asked...
    uint32_t value = strtoul(packet, NULL, 16);
    symbol_main = value;
    debug_printf("HAVE VALUE: 0x%08x\r\n", value);

    reply_ok();
}
GDBFUNC(qXfer) {
    xfer_func   func = (xfer_func)ptr;
    int         content_len;

    // Func will build/return the content...
    char        *content = func(&content_len);

    // Now process the packet to see what bit we want...
    int offset, length;
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
GDBFUNC(qRcmd) {
    char *p = packet;
    int len = packet_size;

    // Must be an even number of chars...
    if (len & 1) goto error;
    len >>= 1;

    // Decode the hex in place (so we don't need more buffers)
    for (int i = 0; i < len; i++)
    {
        int b = hex_byte(p);
        if (b < 0) goto error;
        p += 2;
        packet[i] = b;
    }
    debug_printf("HAVE RCMD [%.*s]\r\n", len, packet);

    if (strncmp(packet, "reset halt", 10) == 0) {
        core_reset_halt();
        reply_ok();
        return;
    } else if (strncmp(packet, "get_to_main", 11) == 0) {
        int did_bp = 0;

        if (!bp_is_set(symbol_main)) {
        // TODO: check if we fail to add the breakpoint
            bp_set(symbol_main);
            did_bp = 1;
        }
        core_unhalt();
        for (int i=0; i < 200; i++) {
            if (core_is_halted()) break;
            task_sleep_ms(2);
        }
        if (!core_is_halted()) {
            debug_printf("ERROR: failed to stop at main, stoppping now\r\n");
            core_halt();
        }
        if (did_bp) bp_clr(symbol_main);
        reply_ok();
        return;
    }
error:
    reply_err(1);
}


struct gdbitem gdb_q_items[] = {
    { "qC", 2, function_qC, NULL, 0 },
    { "qAttached", 9, function_qAttached, NULL, 0 },
    { "qSupported", 10, function_qSupported, NULL, 0 },
    { "qOffsets", 10, function_qOffsets, NULL, 0 },
    { "qRcmd,", 6, function_qRcmd, NULL, 0 },
    { "qSymbol:", 8, function_qSymbol, NULL, 0 },
    { "qXfer:features:read:", 20, function_qXfer, (void *)xfer_features, 0 },
    { "qXfer:memory-map:read:", 22, function_qXfer, (void *)xfer_memory_map, 0 },
    { "qXfer:threads:read:", 19, function_qXfer, (void *)xfer_threads, 0 },
    { NULL, 0, NULL, NULL, 0 },
};

// -----------------------------------------------------------------------------------------------
// Breakpoint Related Packets (z/Z)
// -----------------------------------------------------------------------------------------------

GDBFUNC(z_hw) {
    int add = num;
    uint32_t addr, size;
    if (!get_two_hex_numbers(packet, ',', &addr, &size)) { reply_err(1); return; }
    if (add) {
        if (bp_set(addr) != SWD_OK) reply_err(1);
    } else {
        if (bp_clr(addr) != SWD_OK) reply_err(1);
    }
    reply_ok();
}
GDBFUNC(z_sw) {
    int add = num;
    uint32_t addr, size;
    if (!get_two_hex_numbers(packet, ',', &addr, &size)) { reply_err(1); return; }
    if (add) {
        if (sw_bp_set(addr, size) != SWD_OK) reply_err(1);
    } else {
        if (sw_bp_clr(addr, size) != SWD_OK) reply_err(1);
    }
    reply_ok();
}

static const struct gdbitem gdb_z_items[] = {
    { "z0,", 3, function_z_sw, NULL, 0 },
    { "Z0,", 3, function_z_sw, NULL, 1 },
    { "z1,", 3, function_z_hw, NULL, 0 },
    { "Z1,", 3, function_z_hw, NULL, 1 },
    { NULL, 0, NULL, NULL, 0 },
};




void process_table(const struct gdbitem *table, char *packet, int packet_size) {
    struct gdbitem *f = (struct gdbitem *)table;

    while (f->match) {
        if (strncmp(packet, f->match, f->matchlen) == 0) {
            f->function(packet + f->matchlen, packet_size - f->matchlen, f->ptr, f->num);
            return;
        }
        f++;
    }
    reply_null();
}

void debug_packet(char *packet, int packet_size) {
    if (strncmp(packet, "vFlashWrite", 11) == 0) {
        char *p = packet + 12; // get past colon
        while (*p++ != ':'); // get past final colon

        int hlen = p - packet;
        debug_printf("PKT [%.*s<%d bytes>]\r\n", hlen, packet, packet_size-hlen);
    } else {
        debug_printf("PKT [%.*s]\r\n", packet_size, packet);
    }
}


void process_packet(char *packet, int packet_size)
{
    // TODO: checksum
    if (!gdb_noack) io_put_byte(gdb_io, '+');

    debug_packet(packet, packet_size);

    switch(*packet) {
        case 'm':   function_memread(packet+1); return;
        case 'M':   function_memwrite(packet+1); return;
        case 'p':   function_get_reg(packet+1); return;
        case 'P':   function_put_reg(packet+1); return;
        case 'g':   function_get_sys_regs(); return;
        case 'T':   function_thread_valid(packet+1, packet_size-1); return;
        case 'H':   process_table(gdb_H_items, packet, packet_size); return;
        case 'q':   process_table(gdb_q_items, packet, packet_size); return;
        case 'z':   process_table(gdb_z_items, packet, packet_size); return;
        case 'Z':   process_table(gdb_z_items, packet, packet_size); return;
        case 'v':   process_table(gdb_v_items, packet, packet_size); return;
        case '?':   reply("S00", NULL, 0); return;  // TODO
    }

    // the odd strange case left...
    if (strncmp(packet, "QStartNoAckMode", 15) == 0) {
        reply_ok();
        gdb_noack = 1;
        return;
    }

    // Else not supported...
    reply_null();
}


// TODO: this isn't really a polling function ... more of a server!
int gdb_poll() {
    static int was_connected = 0;
    int rc;

    if (!io_is_connected(gdb_io)) {
        // We need to let the idle task do it's thing...
        task_sleep_ms(5);
        return 0;
    }

    if (!was_connected) {
        // This is a new connection...
        debug_printf("NEW CONNECTION\r\n");
        // What other state do we care about?
        gdb_noack = 0;

        if (dp_init() != SWD_OK) {
            debug_printf("unable to connect to target, trying again...\r\n");
            task_sleep_ms(250);
            return 0;
        }
        was_connected = 1;

        core_select(0);
        core_reset_halt();
        core_select(1);
        core_reset_halt();
        core_select(0);
    }


    //tud_task();
    //refill_from_usb();
    rc = build_packet();
    if (rc != BP_RUNNING) {
        switch (rc) {
            case BP_PACKET:
                process_packet(gdb_buffer, gdb_blen);
                break;
            case BP_INTR:
                debug_printf("Interrupt Received\r\n");
                gdb_intr = 1;
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
                break;
            case BP_DISCONNECT:
                debug_printf("DISCONNNECT\r\n");
                was_connected = 0;
                break;
            default:
                debug_printf("RC=%d\r\n", rc);
        }
    }
    return 0;
}



DEFINE_TASK(gdbsvr, 1024);


void func_gdbsvr(void *arg) {

    // Initialise the IO mechanism for GDB (both CDC and TCP)...
    gdb_io = io_init(GDB_CDC, GDB_TCP, 4096);

    debug_printf("HERE\r\n");
    while(1) {
        gdb_poll();
    }
}

void gdb_init() {
        CREATE_TASK(gdbsvr, func_gdbsvr, NULL);
}
