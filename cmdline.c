

#include "cmdline.h"
#include "lerp/io.h"
#include "lerp/debug.h"

#include "lerp/task.h"
#include "lerp/interact.h"
#include "lerp/io.h"

#include "pico/cyw43_arch.h"


// Static buffer for the command line ... 
static char buffer[2048];

static char wifi_ssid[32];
static char wifi_creds[32];

//
// Output some status information
//
void cmd_status(struct io *io) {
    char mac[6];
    int wifi_state;
    char *state;
    uint32_t ip;

    static const char *states[] = { "joined", "fail", "no-net", "bad-auth" };

    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    io_printf(io, "MAC Address:  %02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_state = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    switch(wifi_state) {
        case CYW43_LINK_JOIN:       state = "joined"; break;
        case CYW43_LINK_FAIL:       state = "failed"; break;
        case CYW43_LINK_NONET:      state = "no-net"; break;
        case CYW43_LINK_BADAUTH:    state = "bad-auth"; break;
        case CYW43_LINK_DOWN:       state = "down"; break;
        default:                    state = "unknown"; break;
    }
    io_printf(io, "WIFI State:   %s (%d)\r\n", state, wifi_state);

    ip = cyw43_state.netif->ip_addr.addr;
    io_printf(io, "IP Address:   %d.%d.%d.%d\r\n", ip&0xff, (ip>>8)&0xff, (ip>>16)&0xff, (ip>>24));
}

//
// Dirty mechanism for seting an SSID and credentials and causing a rejoin
//
// TODO: this will need to be saved into flash
//
// set ssid=rest of the line
// set creds=rest of the line
// wifi-join
//
void cmd_set(struct io *io, char *buffer, int len) {
    if (strncmp(buffer, "set ssid=", 9) == 0) {
        strcpy(wifi_ssid, buffer+9);
    } else if (strncmp(buffer, "set creds=", 10) == 0) {
        strcpy(wifi_creds, buffer+10);
    } else {
        io_printf(io, "Error: can only use 'set ssid=<something>' or 'set creds=<something>'\r\n");
    }
}

void cmd_join(struct io *io) {
    cyw43_arch_wifi_connect_async(wifi_ssid, wifi_creds, CYW43_AUTH_WPA2_AES_PSK);
}

//
// Quick command line interpretation.... will need to be redone, perhaps rethinking the
// tokeniser approach.
//
int process_cmdline(struct io *io, char *buffer, int len) {
    // let's make sure we are zero terminated...
    buffer[len] = 0;

    // Very limited capabilities for the moment...
    if (strcmp(buffer, "status") == 0) {
        cmd_status(io);
    } else if (strncmp(buffer, "set ", 4) == 0) {
        cmd_set(io, buffer, len);
    } else if (strcmp(buffer, "wifi-join") == 0) {
        cmd_join(io);
    } else {
        io_printf(io, "uunrecognised command.\r\n");
    }

}



//  Get at the debug output...
extern struct circ *circ_debug;

// Async func for doing something...
//
// Let's have a play with debug output ... if there is a line (\r\n) then output it
// otherwise if we get over 100 chars, output that.
//
// TODO: this is an experiment and needs to be done properly.
//
static char db_line[120];
static int db_lpos = 0;


char *asf() {
    if (db_lpos == 0) {
        db_line[0] = 'D';
        db_line[1] = '/';
        db_line[2] = ' ';
        db_lpos = 3;
    }

    while(!circ_is_empty(circ_debug)) {
        int ch = circ_get_byte(circ_debug);
        db_line[db_lpos++] = ch;
        if ((db_lpos == 100) || (ch == '\n')) {
            db_line[db_lpos] = 0;
            db_lpos = 0;
            return db_line;
        }
    }
    return NULL;
}

struct task *cmdline_task = NULL;
struct io *cmdline_io = NULL;

DEFINE_TASK(cmdline, 1024);

DEFINE_TASK(dummy, 1024);


void func_cmdline(void *arg) {

    // Initialise the IO mechanism for the commandline/debug output...
    cmdline_io = io_init(CMD_CDC, CMD_TCP, 4096);
    cmdline_io->support_telnet = 1;
    
    struct interact *i = interact_with_buf(cmdline_io, buffer, sizeof(buffer), "pico-debug> ");

    cmdline_task = current_task();

    while (1) {
        while (!io_is_connected(cmdline_io)) {
            task_sleep_ms(100);
        }
        int err = interact(i, asf);
        debug_printf("Have i=%d\r\n", err);
        int len = circ_used(i->cmd);
        debug_printf("CMD is [%.*s] (len=%d)\r\n", len, buffer, len);
        process_cmdline(cmdline_io, buffer, len);
    }
}

// Nudge the interactive process every 200ms...
// We need to switch this to a trigger from debug_printf ideally so
// we won't need this at all 
// TODO:
void func_dummy(void *arg) {
    while (1) {
        if (cmdline_task) {
            if (cmdline_task->state == BLOCKED) {
                task_wake(cmdline_task, -10);
            }
        }
        task_sleep_ms(200);
    }

}

void cmdline_init() {
        CREATE_TASK(cmdline, func_cmdline, NULL);
        CREATE_TASK(dummy, func_dummy, NULL);
}



