

#ifndef __IOIO_H
#define __IOIO_H

#include <stdint.h>
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lerp/circ.h"

// Possible special return codes from io_get_byte
#define IO_DATA             0       // internal
#define IO_ERROR            -1
#define IO_CONNECT          -2
#define IO_DISCONNECT       -3


struct io {
    struct io           *next;          // linked list of io ports

    struct circ         *input;
    struct circ         *output;

    // We currently support either cdc ports or TCP connections...
    int                 cdc_port;
    int                 tcp_port;
    int                 support_telnet;

    // Will be populated when we are connected...
    int                 usb_is_connected;
    struct tcp_pcb      *pcb;
    struct pbuf         *tcpdata;       // tcp data waiting

    // Task pointers for blocking...
    struct task         *waiting_on_input;
    struct task         *waiting_on_output;

    // Internal circ structures for input and output...
    struct circ         _input;
    struct circ         _output;
};

struct io *io_init(int cdc_port, int tcp_port, int buf_size);

void io_poll();
int io_get_byte(struct io *io);
int io_get_byte_nb(struct io *io);
int io_put_byte(struct io *io, uint8_t ch);
int io_put_bytes(struct io *io, uint8_t *data, int len);
int io_put_hexbyte(struct io *io, uint8_t b);
int io_printf(struct io *io, char *format, ...);
int io_aprintf(struct io *io, char *format, va_list args);
int io_peek_byte(struct io *io);
int io_read_flush(struct io *io);
int io_is_connected(struct io *io);
void io_close(struct io *io);
#endif
