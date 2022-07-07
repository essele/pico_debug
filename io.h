

#ifndef __IOIO_H
#define __IOIO_H

// Possible special return codes from io_get_byte
#define IO_DATA             0       // internal
#define IO_ERROR            -1
#define IO_CONNECT          -2
#define IO_DISCONNECT       -3

void io_poll();
int io_get_byte();
int io_peek_byte();

int reply(char *text, uint8_t *hex, int hexlen);
int reply_part(char ch, char *text, int len);
int reply_null();
int reply_ok();
int reply_err(uint8_t err);

#endif
