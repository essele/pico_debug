

#ifndef __IOIO_H
#define __IOIO_H

// Possible special return codes from io_get_byte
#define IO_DATA             0       // internal
#define IO_ERROR            -1
#define IO_CONNECT          -2
#define IO_DISCONNECT       -3

int io_poll();
int io_get_byte();

#endif
