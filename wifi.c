

#include <stdio.h>
#include "wifi.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "lerp/debug.h"

#include "tusb.h"

// Keep track of whether we have a live TCP connection
int _tcp_connected = 0;


static int scan_result(void *env, const cyw43_ev_scan_result_t *result) {
    if (result) {
        debug_printf("ssid: %-32s rssi: %4d chan: %3d mac: %02x:%02x:%02x:%02x:%02x:%02x sec: %u\n",
            result->ssid, result->rssi, result->channel,
            result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5],
            result->auth_mode);
    }
    return 0;
}

enum {
    MODE_DISCONNECTED = 0,
    MODE_CONNECTED
};

int mode = MODE_DISCONNECTED;

void wifi_poll() {
    cyw43_arch_poll();


    if (mode == MODE_DISCONNECTED) {
        int ls;
        ls = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        if (ls == CYW43_LINK_JOIN) {
            mode = MODE_CONNECTED;
            debug_printf("CONNECTED TO NETOWKR\r\n");
            return;
        }
    }
    if (mode == MODE_CONNECTED) {
        int ls;
        ls = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        if (ls != CYW43_LINK_JOIN) {
            mode = MODE_DISCONNECTED;
            debug_printf("NOT CONNECTED\r\n");
        }
    }

/*
    if (mode == MODE_SCANNING) {
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            timeout = time_us_64() + (10 * 1000 * 1000);
            mode = MODE_SLEEP;
            return;
        }
    }

    if (mode == MODE_SLEEP) {
        if (time_us_64() > timeout) {
            debug_printf("STARTING NEW SCAN\r\n");
            cyw43_wifi_scan_options_t scan_options = {0};
            int err = cyw43_wifi_scan(&cyw43_state, &scan_options, NULL, scan_result);
            if (err != 0) {
                debug_printf("scan srart failed %d\r\n", err);
            }
            mode = MODE_SCANNING;
            return;
        }
    }
    */
}

static err_t my_sent(void *arg, struct tcp_pcb *pcb, uint16_t len) {
    debug_printf("sent %d bytes\r\n", len);
    return ERR_OK;
}

static err_t my_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    debug_printf("Have data\r\n");
    if (!p) {
        // This is a disconnect...
        tcp_close(pcb);
        _tcp_connected = 0;
        debug_printf("TCP Connection dropped\r\n");
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}


static err_t my_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK || pcb == NULL) {
        debug_printf("accept failed\r\n");
        return ERR_VAL;        
    }
    tcp_sent(pcb, my_sent);
    tcp_recv(pcb, my_recv);
    _tcp_connected = 1;
    return ERR_OK;
}



void wifi_init() {



}
