

#include "cmdline.h"
#include "lerp/io.h"
#include "lerp/debug.h"

#include "lerp/task.h"
#include "lerp/interact.h"
#include "lerp/io.h"
#include "lerp/tokeniser.h"
#include "config/config.h"

#include "pico/cyw43_arch.h"


// Circular buffer for the command line ... 
CIRC_DEFINE(buffer, 2048);

//
// Output some status information
//
void cmd_status(struct io *io, __unused struct circ *circ) {
    uint8_t mac[6];
    int wifi_state;
    char *state;
    uint32_t ip;

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
void cmd_set(struct io *io, struct circ *circ) {
    int tok;
    char item[32];

    tok = token_get(circ);
    if (tok == TOK_END) {
        // should output a list
        int maxlen = cf_max_name_len();
        char *item = cf_next_item(NULL);
        while (item) {
            int padding = maxlen - strlen(item);
            io_printf(io, "%s:%*s %s\r\n", item, padding, "", cf_get_strval(item));
            item = cf_next_item(item);
        }
        return;
    }

    if (tok != TOK_WORD) {
        io_printf(io, "expect set <word>=<value>\r\n");
        return;
    }
    strcpy(item, token_string());

    tok = token_get(circ);
    if (tok == TOK_END) {
        // this is a request of the value
        io_printf(io, "have requested value of %s\r\n", item);
        return;
    }
    if (tok != TOK_EQUALS) {
        io_printf(io, "expect set <word>=<value>\r\n");
        return;
    }

    char *err = cf_set_with_tokens(item, circ);
    if (err) {
        io_printf(io, "Blah error: %s\r\n", err);
    }

    // Now do a set with tokens...
    /*
    if (strncmp(word, "set ssid=", 9) == 0) {
        strcpy(wifi_ssid, buffer+9);
    } else if (strncmp(buffer, "set creds=", 10) == 0) {
        strcpy(wifi_creds, buffer+10);
    } else {
        io_printf(io, "Error: can only use 'set ssid=<something>' or 'set creds=<something>'\r\n");
    }
    */
}

void cmd_join(struct io *io, __unused struct circ *circ) {
    if (*cf->main->wifi.ssid && *cf->main->wifi.creds) {
        cyw43_arch_wifi_connect_async(cf->main->wifi.ssid, cf->main->wifi.creds, CYW43_AUTH_WPA2_AES_PSK);
        io_printf(io, "join: started.\r\n");
    } else {
        io_printf(io, "join: wifi ssid or credentials missing.\r\n");
    }
}

void cmd_save(struct io *io, __unused struct circ *circ) {
    config_save();
    io_printf(io, "config saved to flash.\r\n");
}


struct cmd_item {
    char    *cmd;
    void    (*func)(struct io *io, struct circ *circ);
};

struct cmd_item commands[] = {
    { "status", cmd_status },
    { "set",    cmd_set },
    { "join",   cmd_join },
    { "save",   cmd_save },
    { NULL, NULL },
};


//
// Quick command line interpretation.... will need to be redone, perhaps rethinking the
// tokeniser approach.
//
int process_cmdline(struct io *io, struct circ *buffer) {
    // let's make sure we are zero terminated...
    *buffer->head = 0;

    int tok = token_get(buffer);
    if (tok == TOK_END) return 0;
    if (tok != TOK_WORD) {
        io_printf(io, "expected a command word.\r\n");
        return -1;
    }

    char *word = token_string();

    struct cmd_item *c = commands;
    while (c->cmd) {
        if (strcmp(word, c->cmd) == 0) {
            c->func(io, buffer);
            return 0;
        }
        c++;
    }

    io_printf(io, "uunrecognised command.\r\n");
    return 1;
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
    
//    struct interact *i = interact_with_buf(cmdline_io, buffer, sizeof(buffer), "pico-debug> ");
    struct interact *i = interact_with_circ(cmdline_io, buffer, "pico-debug> ");

    cmdline_task = current_task();

    while (1) {
        while (!io_is_connected(cmdline_io)) {
            task_sleep_ms(100);
        }
        int err = interact(i, asf);
        debug_printf("Have i=%d\r\n", err);
        int len = circ_used(i->cmd);
        debug_printf("CMD is [%.*s] (len=%d)\r\n", len, buffer, len);
        process_cmdline(cmdline_io, buffer);
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



