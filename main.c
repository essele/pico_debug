#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "pico/printf.h"

#include "swd.h"


#include "tusb.h"
#include "filedata.h"

volatile int ret;


static volatile uint32_t xx;
static volatile uint32_t id;
static volatile uint32_t rc;



#define GDB_BUFFER_SIZE         16384


static char gdb_buffer[GDB_BUFFER_SIZE+1];
static char *gdb_bp;
static char *gdb_mkr;       // end marker location
static int gdb_blen;

static int gdb_noack = 0;

static int gdb_check_for_halt = 0;

/**
 * @brief Output routine for usb_cdc_printf
 * 
 * This blocks if we have no space to write into the usb buffer
 * also doesn't do anything if the connection is gone.
 * 
 * @param ch 
 * @param arg 
 */
static void _cdc_out(char ch, void *arg) {
    int n = (int)arg;

    // We could become unconnected mid-proint...
    if (tud_cdc_n_connected(n)) {
        while(!tud_cdc_n_write_available(n)) tud_task();
        tud_cdc_n_write_char(n, ch);
    }
}
static int usb_n_printf(int n, char *format, ...) {
    va_list args;
    int len;

    va_start(args, format);
    len = vfctprintf(_cdc_out, (void *)n, format, args);
    va_end(args);
    tud_cdc_n_write_flush(n);
    return len;
}


#define debug_printf(...)       usb_n_printf(1, __VA_ARGS__)
#define gdb_printf(...)         usb_n_printf(0, __VA_ARGS__)

static char gen_buffer[1024];

static char debug_buf[800];


enum {
    USB_OK = 0,             // running normally
    USB_PACKET,             // we have a packet
    USB_ERROR,              // some error has occurred
};

char *packet_checksum(char *packet, int len) {
    static char ckbuffer[3];
    uint8_t ck = 0;

    for (int i=0; i < len; i++) ck += packet[i];
    sprintf(ckbuffer, "%02x", ck);
    return ckbuffer;
}

volatile int yy;

int send_packet(char *packet, int len) {
    uint8_t sum = 0;
    for (int i=0; i < len; i++) sum += packet[i];
    debug_printf("Sending: $%.*s#%02x\r\n", len, packet, sum);
    gdb_printf("$%.*s#%02x", len, packet, sum);
}

int send_packet_with_leading_char(char ch, char *packet, int len) {
    uint8_t sum = ch;
    for (int i=0; i < len; i++) sum += packet[i];
    debug_printf("Sending: $%c%.*s#%02x\r\n", ch, len, packet, sum);
    gdb_printf("$%c%.*s#%02x", ch, len, packet, sum);
}

int gdb_error(int err) {
    char errstr[4];

    snprintf(errstr, 4, "E%2.2X", err);
    send_packet(errstr, 3);
}

/**
 * @brief Check the CDC input state and build up a input stream until we have a packet
 * 
 * @return int 
 */
int usb_read_poll() {
    int rc = USB_OK;

    // Wait for us to be connected...
    if (!tud_cdc_connected()) return USB_OK;

    // Read the USB data...
    int len = tud_cdc_read(gdb_bp, GDB_BUFFER_SIZE - gdb_blen);
    if (!len) return USB_OK;

    // If we have a '+' at the start then we can ignore it...
    if ((gdb_bp == gdb_buffer)) {
        if (*gdb_buffer == '+') {
            debug_printf("got +\r\n");
        } else if (*gdb_buffer == '-') {
            debug_printf("got -\r\n");
        } else {
            goto xxx;
        }
        len--;
        if (!len) return USB_OK;
        memmove(gdb_buffer+1, gdb_buffer, len);
    }
xxx:
    // See if we have an end packet marker in that last chunk...  (if we haven't already found one)
    if (!gdb_mkr) {
        gdb_mkr = memchr(gdb_bp, '#', len);
    }

    // We need two chars afer the marker to have a valid packet
    if (gdb_mkr) {
        char *end = gdb_bp + len;
        if (end-gdb_mkr > 2) {
            // We have a valid packet...
            rc = USB_PACKET;
        }
    }

    // Update our buffer pointer...
    gdb_bp += len;
    gdb_blen += len;
    return rc;
}

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

void function_qSupported() {
    int len = sprintf(gen_buffer, "PacketSize=%x;qXfer:memory-map:read+;qXfer:features:read+;qXfer:threads:read+;QStartNoAckMode+;vContSupported+",
        GDB_BUFFER_SIZE);
    send_packet(gen_buffer, len);
}

/**
 * @brief This will be called when we want a part of something
 * 
 * @param packet 
 * @param len 
 */
void function_xfer_thing(char *packet, char *content, int content_len) {
    int offset;
    int length;
    char symbol;
    char *p;

    if (!decode_xfer_read(packet, &offset, &length)) {
        gdb_error(1);
        return;
    }

    p = content + offset;
    if (offset + length > content_len) {
        length = content_len - offset;
        symbol = 'l';
    } else {
        symbol = 'm';
    }
    send_packet_with_leading_char(symbol, p, length);
}

void function_xfer_threads(char *packet) {
    char *out = malloc(1024);
    if (!out) {
        gdb_error(1);
        return;
    }
    sprintf(out,    "<?xml version=\"1.0\"?>\n<threads>\n"
                    "<thread id=\"1\">Name: rp2040.core0, state: %s</thread>\n"
                    "<thread id=\"2\">Name: rp2040.core1, state: %s</thread>\n"
                    "</threads>\n",
                    "breakpoint", "debug-request");
    function_xfer_thing(packet, out, strlen(out));
    free(out);
}

void function_get_sys_regs(int reg) {
    char buf[18*8];
    char *p = buf;
    
    for (int i=0; i <= 16; i++) {
            uint32_t rval;
            int rc;

            if(reg != -1 && reg != i) continue;

            rc = reg_read(i, &rval);
            if (rc != SWD_OK) panic("reg read failed");
            sprintf(p, "%02x%02x%02x%02x", rval&0xff, (rval&0xff00)>>8, (rval&0xff0000)>>16, rval>>24);
            p += 8;
    }
    send_packet(buf, strlen(buf));
}

void function_put_reg(char *packet) {
    int reg;
    char *sep;
    uint32_t value;
    int rc;

    reg = strtoul(packet, &sep, 16);
    if (*sep != '=') {
        send_packet(NULL, 0);
        return;
    }
    value = strtoul(sep+1, NULL, 16);
    rc = reg_write(reg, value);
    if (rc != SWD_OK) {
        gdb_error(1);
    } else {
        send_packet("OK", 2);
    }
}

char *get_two_hex_numbers(char *packet, char sepch, uint32_t *one, uint32_t *two) {
    char *sep;
    char *end;

    *one = strtoul(packet, &sep, 16);
    if (*sep != sepch) return NULL;
    *two = strtoul(sep+1, &end, 16);
    return end;
}


void function_memread(char *packet) {
    char *sep;
    uint32_t addr;
    uint32_t len;
    int rc;

    addr = strtoul(packet, &sep, 16);
    if (*sep != ',') {
        send_packet(NULL, 0);
        return;
    }
    len = strtoul(sep + 1, NULL, 16);
    if (!len) {
        send_packet(NULL, 0);
        return;
    }

    uint32_t real_start;
    uint32_t real_len;

    real_start = addr & 0xfffffffc;
    real_len = len & 0xfffffffc;

    if (real_len < len) real_len += 4;

    char *buffer = malloc(real_len);     // make sure we have enough space
    if (!buffer) {
        send_packet(NULL, 0);
        return;
    }
    real_len >>= 2;                     // we need a count of 32bit words

    rc = mem_read_block(real_start, real_len, (uint32_t *)buffer);

    char *out = malloc((len * 2)+1);
    if (!out) {
        send_packet(NULL, 0);
        return;
    }

    char *p = buffer + (addr - real_start);
    char *o = out;
    int i = len;
    while (i--) {
        sprintf(o, "%02x", *p++);
        o += 2;
    }
    send_packet(out, len*2);
    free(out);
    free(buffer);
}

void function_vcont(char *packet) {
    int rc;

    if (*packet == '?') {
        static const char vcont[] = "vCont;c;C;s;S";
        send_packet((char *)vcont, sizeof(vcont)-1);
        return;
    }
    if (*packet == ';') packet++;
    if (*packet == 'c') {
        // Continue
        debug_printf("unhalting core\r\n");
        rc = core_unhalt();
        debug_printf("rc=%d\r\n", rc);
        // No reply ... but we need to check regularly now!
        gdb_check_for_halt = 1;
    } else if (*packet == 's') {
        // Step
        core_step();
        // Assume we stop again...
        send_packet("S05", 3);
    }
}

static char hex_digits[] = "0123456789abcdef";

void function_rcmd(char *packet, int len) {
    char *p = packet;

    // Must be an even number of chars...
    if (len & 1) goto error;
    len >>= 1;

    // Decode the hex in place (so we don't need more buffers)
    for (int i=0; i < len; i++) {
        int v;
        char *x = index(hex_digits, tolower(*p++));
        if (!x) goto error;
        v = ((int)(x - hex_digits)) << 4;
        x = index(hex_digits, tolower(*p++));
        if (!x) goto error;
        v |= ((int)(x - hex_digits));
        packet[i] = v;
    }
    debug_printf("HAVE RCMD [%.*s]\r\n", len, packet);

    if (strncmp(packet, "reset halt", 10) == 0) {
        core_reset_halt();
        send_packet("OK", 2);
        return;
    }
    gdb_error(1);
    return;

error:
    gdb_error(1);
}

void function_vflash_erase(char *packet) {
    uint32_t start, length;
    char *p = get_two_hex_numbers(packet, ',', &start, &length);

    if (!p || !length) {
        gdb_error(1);
        return;
    }


}


void process_packet(char *packet, int packet_size) {
    // TODO: checksum
    if (!gdb_noack) gdb_printf("+");

    if (packet[0] != '$') {
        debug_printf("CORRUPT: %.*s\r\n", packet_size, packet);
        return;
    }
    // Move past the $
    packet++;
    packet_size--;

    debug_printf("PKT [%.*s]\r\n", (gdb_mkr - gdb_buffer) + 3, gdb_buffer);

    if (strncmp(packet, "qSupported:", 11) == 0) {
        function_qSupported();
    } else if (strncmp(packet, "QStartNoAckMode", 15) == 0) {
        send_packet("OK", 2);
        gdb_noack = 1;
    } else if (strncmp(packet, "vMustReplyEmpty", 15) == 0) {
        send_packet(NULL, 0);
    } else if (strncmp(packet, "qXfer:features:read:", 20) == 0) {
        function_xfer_thing(packet+20, (char *)rp2040_features_xml, sizeof(rp2040_features_xml));     
    } else if (strncmp(packet, "qXfer:memory-map:read:", 22) == 0) {
        function_xfer_thing(packet+22, (char *)rp2040_memory_map_xml, sizeof(rp2040_memory_map_xml));     
    } else if (strncmp(packet, "qXfer:threads:read:", 19) == 0) {
        //function_xfer_thing(packet+19, (char *)rp2040_threads_xml, sizeof(rp2040_threads_xml));  
        function_xfer_threads(packet+19); 
    } else if (strncmp(packet, "qAttached", 9) == 0) {
        send_packet("1", 1);
    } else if (strncmp(packet, "qSymbol::", 9) == 0) {
        send_packet("OK", 2);
    } else if (strncmp(packet, "?", 1) == 0) {
        // TODO:!!!!!
        send_packet("S00", 3);
    } else if (strncmp(packet, "H", 1) == 0) {
        // This is thread stuff...
        send_packet("OK", 2);
    } else if (strncmp(packet, "qC", 2) == 0) {
        // RTOS thread?
//        send_packet("QC0", 3);
        send_packet("QC0000000000000001", 18);
    } else if (strncmp(packet, "qOffsets", 8) == 0) {
        static const char offsets[] = "Text=0;Data=0;Bss=0";
        send_packet((char *)offsets, sizeof(offsets)-1);
    } else if (strncmp(packet, "g", 1) == 0) {
        // Get all registers...
        function_get_sys_regs(-1);
    } else if (strncmp(packet, "m", 1) == 0) {
        function_memread(packet+1);
    } else if (strncmp(packet, "p", 1) == 0) {
        // Read single register...
        function_get_sys_regs(strtoul(packet+1, NULL, 16));
    } else if (strncmp(packet, "P", 1) == 0) {
        // Write single reg...
        function_put_reg(packet);
    } else if (strncmp(packet, "vCont", 5) == 0) {
        function_vcont(packet+5);
    } else if (strncmp(packet, "qRcmd,", 6) == 0) {
        function_rcmd(packet+6, packet_size-6);
    } else if (strncmp(packet, "vFlashErase:", 12) == 0) {
        function_vflash_erase(packet+12);
    } else {
        // Not supported...
        send_packet(NULL, 0);
    }



    // Really need to copy buffer along if there's any left


    gdb_bp = gdb_buffer;
    gdb_blen = 0;
    gdb_mkr = NULL;
}


int usb_poll() {
    tud_task();
    if (usb_read_poll() == USB_PACKET) {
        int length = gdb_mkr - gdb_buffer;
        process_packet(gdb_buffer, length);
    }
//    usb_write_poll();
}


void gdb_init() {
    gdb_bp = gdb_buffer;
    gdb_blen = 0;
}

int main() {
    // Take us to 150Mhz (for future rmii support)
    set_sys_clock_khz(150 * 1000, true);

    if (swd_init() != SWD_OK) panic("unable to init SWD");
    if (dp_init() != SWD_OK) panic("unable to init DP");

    core_enable_debug();
    core_halt();

//    tusb_init();
    gdb_init();

    sleep_ms(100);
    if (core_reset_halt() != SWD_OK) panic("failed reset");
    sleep_ms(10);


    swd_test();

    uint32_t revaddr = rp2040_find_rom_func('R', '3');
    uint32_t args[1] = { 0x11002233 };

    uint32_t xx = rp2040_call_function(revaddr, args, 1);


    while(1) {
        busy_wait_ms(10);
        usb_poll();

        // Not right in here ... needs to be somewhere else.
        if (gdb_check_for_halt) {
            if (core_is_halted()) {
                debug_printf("CORE HAS HALTED\r\n");
                gdb_check_for_halt = 0;
                send_packet("S05", 3);
            }
        }
    }

}
