#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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



void process_packet(char *packet, int packet_size) {
    // TODO: checksum
    if (!gdb_noack) gdb_printf("+");

    if (packet[0] != '$') {
        debug_printf("CORRUPT: %.*s\r\n", packet_size, packet);
        return;
    }
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
        function_xfer_thing(packet+19, (char *)rp2040_threads_xml, sizeof(rp2040_threads_xml));     
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

//    if (swd_init() != SWD_OK) panic("unable to init SWD");
//    if (dp_init() != SWD_OK) panic("unable to init DP");

    tusb_init();
    gdb_init();

    int i = 0;

    while(1) {
        busy_wait_ms(10);
        usb_poll();
    }

}
