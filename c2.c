#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"


const uint LED_PIN = 18;

#define C2CLK           26
#define C2D             27

/* C2 registers */
#define C2PORT_DEVICEID     0x00
#define C2PORT_REVID        0x01
#define C2PORT_FPCTL        0x02
#define C2PORT_FPDAT        0xB4

/* C2 interface commands */
#define C2PORT_GET_VERSION  0x01
#define C2PORT_DEVICE_ERASE 0x03
#define C2PORT_BLOCK_READ   0x06
#define C2PORT_BLOCK_WRITE  0x07
#define C2PORT_PAGE_ERASE   0x08

/* C2 status return codes */
#define C2PORT_INVALID_COMMAND  0x00
#define C2PORT_COMMAND_FAILED   0x02
#define C2PORT_COMMAND_OK   0x0d



static void c2_reset() {
    gpio_put(C2CLK, false);
    busy_wait_us(25);
    gpio_put(C2CLK, true);
}

static void c2_strobe_clk() {
    gpio_put(C2CLK, false);
    busy_wait_us(1);
    gpio_put(C2CLK, true);
}

static void c2_write_ar(uint8_t addr) {
   int i;

    /* START field */
    c2_strobe_clk();

    /* INS field (11b, LSB first) */
    gpio_set_dir(C2D, true);
    gpio_put(C2D, 1);
    c2_strobe_clk();
    gpio_put(C2D, 1);
    c2_strobe_clk();

    /* ADDRESS field */
    for (i = 0; i < 8; i++) {
        gpio_put(C2D, (addr & 0x01));
        c2_strobe_clk();

        addr >>= 1;
    }

    /* STOP field */
    gpio_set_dir(C2D, false);
    c2_strobe_clk();   
}

static int c2_read_ar(uint8_t *addr) {
    int i;
    
    /* START field */
    c2_strobe_clk();
    
    /* INS field (10b, LSB first) */
    gpio_set_dir(C2D, true);
    gpio_put(C2D, 0);
    c2_strobe_clk();
    gpio_put(C2D, 1);
    c2_strobe_clk();
    
    /* ADDRESS field */
    gpio_set_dir(C2D, false);
    *addr = 0;
    for (i = 0; i < 8; i++) {
        *addr >>= 1;    /* shift in 8-bit ADDRESS field LSB first */
        
        c2_strobe_clk();
        if (gpio_get(C2D))
            *addr |= 0x80;
    }
    
    /* STOP field */
    c2_strobe_clk();
    return 0;
}

static int c2_write_dr(uint8_t data)
{
    int timeout, i;

    /* START field */
    c2_strobe_clk();

    /* INS field (01b, LSB first) */
    gpio_set_dir(C2D, true);
    gpio_put(C2D, 1);
    c2_strobe_clk();
    gpio_put(C2D, 0);
    c2_strobe_clk();

    /* LENGTH field (00b, LSB first -> 1 byte) */
    gpio_put(C2D, 0);
    c2_strobe_clk();
    gpio_put(C2D, 0);
    c2_strobe_clk();

    /* DATA field */
    for (i = 0; i < 8; i++) {
        gpio_put(C2D, data & 0x01);
        c2_strobe_clk();
        data >>= 1;
    }

    /* WAIT field */
    gpio_set_dir(C2D, false);
    timeout = 20;
    do {
        c2_strobe_clk();
        if (gpio_get(C2D))
            break;

        busy_wait_us(1);
    } while (--timeout > 0);
    if (timeout == 0)
        return -1;

    /* STOP field */
    c2_strobe_clk();

    return 0;
}

static int c2_read_dr(uint8_t *data)
{
    int timeout, i;

    /* START field */
    c2_strobe_clk();

    /* INS field (00b, LSB first) */
    gpio_set_dir(C2D, true);
    gpio_put(C2D, 0);
    c2_strobe_clk();
    gpio_put(C2D, 0);
    c2_strobe_clk();

    /* LENGTH field (00b, LSB first -> 1 byte) */
    gpio_put(C2D, 0);
    c2_strobe_clk();
    gpio_put(C2D, 0);
    c2_strobe_clk();

    /* WAIT field */
    gpio_set_dir(C2D, false);
    timeout = 20;
    do {
        c2_strobe_clk();
        if (gpio_get(C2D))
            break;

        busy_wait_us(1);
    } while (--timeout > 0);
    if (timeout == 0)
        return -1;

    /* DATA field */
    *data = 0;
    for (i = 0; i < 8; i++) {
        *data >>= 1;    /* shift in 8-bit DATA field LSB first */

        c2_strobe_clk();
        if (gpio_get(C2D))
            *data |= 0x80;
    }

    /* STOP field */
    c2_strobe_clk();

    return 0;
}

static int c2_poll_in_busy()
{
    uint8_t addr;
    int ret, timeout = 20;

    do {
        ret = (c2_read_ar(&addr));
        if (ret < 0)
            return -1;

        if (!(addr & 0x02))
            break;

        busy_wait_us(1);
    } while (--timeout > 0);
    if (timeout == 0)
        return -1;

    return 0;
}
static int c2_poll_out_ready()
{   
    uint8_t addr; 
    int ret, timeout = 10000; /* erase flash needs long time... */
    
    do {
        ret = (c2_read_ar(&addr));
        if (ret < 0)
            return -1;
        
        if (addr & 0x01)
            break;
        
        busy_wait_us(1);
    } while (--timeout > 0);
    if (timeout == 0)
        return -1;
    
    return 0;
}

volatile int ret;

int main()
{
    gpio_init(C2CLK);
    gpio_init(C2D);

    gpio_set_dir(C2CLK, true);      // clk is output
    gpio_set_dir(C2D, false);       // d in input (for now)

    // Set CLK high as a starting point
    gpio_put(C2CLK, true);

    sleep_ms(200);

    c2_reset();

    uint8_t data;
//    int ret;

    /* Select DEVICEID register for C2 data register accesses */
    c2_write_ar(C2PORT_DEVICEID);

    /* Read and return the device ID register */
    ret = c2_read_dr(&data);

    sleep_ms(200);

    // Initialise the PI...
    c2_write_ar(C2PORT_FPCTL);
    c2_write_dr(0x02);
    c2_write_dr(0x04);
    c2_write_dr(0x01);
    sleep_ms(20);


    uint8_t d1, d2;

    c2_write_ar(C2PORT_FPDAT);
    c2_write_dr(0x09);      // DATA_WRITE
    if (c2_poll_in_busy() < 0) {
        while (1);
    }

    if (c2_poll_out_ready() < 0) {
        while(1);
    }
    ret = c2_read_dr(&d1);

    c2_write_dr(0xAD);      // DERIVID SFR

    if (c2_poll_in_busy() < 0) {
        while(1);
    }

    c2_write_dr(0x01);      // ?
    if (c2_poll_in_busy() < 0) {
        while(1);
    }

    if (c2_poll_out_ready() < 0) {
        while(1);
    }
    ret = c2_read_dr(&d2);

    printf("ret is %d\r\n");
    while(1);

    while(1);

}
