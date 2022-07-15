
#ifndef __WIFI_H
#define __WIFI_H

void wifi_init();
void wifi_poll();

extern int _tcp_connected;

static int inline is_tcp_connected() { return _tcp_connected; }

#endif
