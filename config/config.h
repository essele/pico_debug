
#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdint.h>
#include "lerp/circ.h"

#define CF_VERSION      1

struct cf_main {
    int             version;

    struct {
        uint32_t    speed;
        uint32_t    pin_clk;
        uint32_t    pin_io;
    } swd;

    struct {
        bool        enable;
    } dhcp;

    struct {
        char        ssid[32];
        char        creds[32];
    } wifi;

    struct {
        uint32_t    pin_rx;
        uint32_t    pin_tx;
    } uart;
};


struct cf_live {
    struct {
        uint32_t    addr;
        uint32_t    gateway;
        uint32_t    netmask;
    } ip;
};

struct cf {
    struct cf_main *flash;
    struct cf_main *main;
    struct cf_live *live;
};

extern struct cf *cf;

void config_init();
void config_save();
char *cf_set_with_tokens(char *name, struct circ *circ);
int cf_max_name_len();
char *cf_next_item(char *prev);
char *cf_get_desc(char *item);
char *cf_get_strval(char *item);

#endif
