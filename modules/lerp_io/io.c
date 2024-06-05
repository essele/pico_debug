
#include "pico/printf.h"
#include "lerp/task.h"
#include "lerp/circ.h"
#include "lerp/debug.h"
#include "lerp/io.h"
#include "tusb.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"


//
// Which IO ports do we have active...
//
struct io   *ios = NULL;


/**
 * Input mechanism -- needs to support both USB and Ethernet, and unfortunately
 * one of those works as a pull and one as a push.
 *
 * So... use a circular buffer for input which we refill from usb and the ethernet
 * stack fills as packets come in.
 *
 */

// -----------------------------------------------------------------------------------
// USB FUNCTIONS
// -----------------------------------------------------------------------------------

static int refill_from_usb(struct io *io)
{
    int count = 0;

    int space = circ_space_before_wrap(io->input);
    if (space)
    {
        int avail = tud_cdc_n_available(io->cdc_port);
        if (avail)
        {
            int size = MIN(space, avail);
            count = tud_cdc_n_read(io->cdc_port, io->input->head, size);
            circ_advance_head(io->input, count);
        }
    }
    if (count && io->waiting_on_input) {
        task_wake(io->waiting_on_input, IO_DATA);
        io->waiting_on_input = NULL;
    }
    return count;
}

static void send_to_usb(struct io *io) {
    int have = circ_bytes_before_wrap(io->output);
    if (!have) return;
    int space = tud_cdc_n_write_available(io->cdc_port);
    if (!space) return;

    while (space && have) {
        int size = MIN(have, space);
        int count = tud_cdc_n_write(io->cdc_port, io->output->tail, size);
        circ_advance_tail(io->output, count);
        space -= count;
        have = circ_bytes_before_wrap(io->output);
    }
    tud_cdc_n_write_flush(io->cdc_port);
    // If we get here then we must have sent something so we
    // will have some space...
    if (io->waiting_on_output) {
        task_wake(io->waiting_on_output, IO_DATA);
        io->waiting_on_output = NULL;
    }
}

// -----------------------------------------------------------------------------------
// Support for TELNET processing when needed...
// -----------------------------------------------------------------------------------

#define TELNET_IAC              ((uint8_t) 255)
#define TELNET_WILL             ((uint8_t) 251)
#define TELNET_WONT             ((uint8_t) 252)
#define TELNET_DO               ((uint8_t) 253)
#define TELNET_DONT             ((uint8_t) 254)
#define TELNET_SE               ((uint8_t) 240)
#define TELNET_NOP              ((uint8_t) 241)
#define TELNET_DATA_MARK        ((uint8_t) 242)
#define TELNET_BREAK            ((uint8_t) 243)
#define TELNET_IP               ((uint8_t) 244)
#define TELNET_AO               ((uint8_t) 245)
#define TELNET_AYT              ((uint8_t) 246)
#define TELNET_EC               ((uint8_t) 247)
#define TELNET_EL               ((uint8_t) 248)
#define TELNET_GA               ((uint8_t) 249)
#define TELNET_SB               ((uint8_t) 250)

#define TELNET_OPT_BINARY       ((uint8_t) 0)
#define TELNET_OPT_ECHO         ((uint8_t) 1)
#define TELNET_OPT_SUPPRESS_GA  ((uint8_t) 3)
#define TELNET_OPT_STATUS       ((uint8_t) 5)
#define TELNET_OPT_TIMING_MARK  ((uint8_t) 6)
#define TELNET_OPT_EXOPL        ((uint8_t) 255)

const static char connect_sequence[] = { TELNET_IAC, TELNET_WILL, TELNET_OPT_SUPPRESS_GA,
                                         TELNET_IAC, TELNET_WILL, TELNET_OPT_BINARY,
                                        TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO };

static void inline tsend(struct io *io, uint8_t cmd, uint8_t ch) {
    circ_add_byte(io->output, TELNET_IAC);
    circ_add_byte(io->output, cmd);
    circ_add_byte(io->output, ch);
}

/**
 * @brief State machine for handling telnet escape sequences
 * 
 * @param io 
 * @return int 
 */
static int telnet_process_char(struct io *io, int ch) {
    enum { STATE_NORMAL = 0, STATE_IAC, STATE_WILL, STATE_WONT, STATE_DO, STATE_DONT };
    static int state = STATE_NORMAL;

    switch(state) {
        case STATE_NORMAL:
            if (ch == TELNET_IAC) {
                state = STATE_IAC; break;
            } else {
                circ_add_byte(io->input, ch); return 1;
            }
        case STATE_IAC:
            switch(ch) {
                case TELNET_IAC:    state = STATE_NORMAL; circ_add_byte(io->input, 255); return 1;
                case TELNET_WILL:   state = STATE_WILL; break;
                case TELNET_WONT:   state = STATE_WONT; break;
                case TELNET_DO:     state = STATE_DO; break;
                case TELNET_DONT:   state = STATE_DONT; break;
                case TELNET_AYT:    state = STATE_NORMAL; break; // error really //
                case TELNET_GA:
                case TELNET_NOP:
                default:            state = STATE_NORMAL; break;
            }
            break;
        case STATE_WILL:
            if ((ch != TELNET_OPT_BINARY) && (ch != TELNET_OPT_ECHO) && (ch != TELNET_OPT_SUPPRESS_GA)) {
                tsend(io, TELNET_DONT, ch);
            }
            state = STATE_NORMAL;
            break;
       case STATE_DO:
            if ((ch != TELNET_OPT_BINARY) && (ch != TELNET_OPT_ECHO) && (ch != TELNET_OPT_SUPPRESS_GA)) {
                tsend(io, TELNET_WONT, ch);
            }
            state = STATE_NORMAL;
            break;
        case STATE_WONT:    // no response
        case STATE_DONT:    // no response
        default:            // error, just go back to normal
            state = STATE_NORMAL;
            break;
    }
    return 0;
}

// -----------------------------------------------------------------------------------
// TCP FUNCTIONS
// -----------------------------------------------------------------------------------

static int refill_from_tcp(struct io *io)
{
    int count = 0;
    int size;

    // Don't do anything if we don't have any data...
    if (!io->tcpdata) return 0;
    if (circ_is_full(io->input)) return 0;

    if (io->support_telnet) {
        // Telnet version... let's work through as much of the first pbuf as we can
        // we'll come back again to do the next one etc.
        while (!circ_is_full(io->input)) {
            telnet_process_char(io, ((uint8_t *)(io->tcpdata->payload))[count++]);
            if (count == io->tcpdata->len) break;
        }
        io->tcpdata = pbuf_free_header(io->tcpdata, count);
    } else {
        // Otherwise try and copy as much as we can fit into the space before
        // the circ wrap (we'll be back for more)
        size = MIN(circ_space_before_wrap(io->input), io->tcpdata->tot_len); 
        count = pbuf_copy_partial(io->tcpdata, io->input->head, size, 0);
        io->tcpdata = pbuf_free_header(io->tcpdata, count);
        circ_advance_head(io->input, count);
    }
    if (count && io->waiting_on_input) {
        task_wake(io->waiting_on_input, IO_DATA);
        io->waiting_on_input = NULL;
    }
    return count;
}


void netio_close(struct io *io, char *message) {
    err_t err;

    if (message) {
        err = tcp_write(io->pcb, message, strlen(message), 0);
        tcp_output(io->pcb);
    }
    tcp_close(io->pcb);

    io->pcb = NULL;
}


/**
 * @brief Try to send any pending outgoing data (if there is any)
 * 
 * @param pcb 
 * @return err_t 
 */
static err_t netio_send(struct io *io) {
    struct tcp_pcb *pcb = io->pcb;
    err_t err;
    int sent = 0;

//    while (!circ_is_empty(outgoing)) {
    if (!circ_is_empty(io->output)) {
        int size = MIN(tcp_sndbuf(pcb), circ_bytes_before_wrap(io->output));
//        debug_printf("TCPWRITE [%.*s]\r\n", size, outgoing->tail);
        err = tcp_write(pcb, io->output->tail, size, TCP_WRITE_FLAG_COPY);
//        if (err != ERR_OK) break;
        if (err != ERR_OK) return ERR_OK;
        circ_advance_tail(io->output, size);
        sent += size;
    }
    if (sent) {
        tcp_output(pcb);
        if (io->waiting_on_output) {
            task_wake(io->waiting_on_output, IO_DATA);
            io->waiting_on_output = NULL;
        }
    }
    return ERR_OK;
}

/*
static err_t netio_sent(void *arg, struct tcp_pcb *pcb, uint16_t len) {
    debug_printf("sent %d bytes\r\n", len);
    // Try to send some more...
    //netio_send(pcb);
    return ERR_OK;
}
*/

static err_t netio_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct io *io = (struct io *)arg;

    if (!p) {
        // This is a disconnect...
        tcp_close(pcb);
        io->pcb = NULL;
        if (io->tcpdata) {
            pbuf_free(io->tcpdata);
            io->tcpdata = NULL;
        }
        debug_printf("TCP Connection dropped\r\n");
        if (io->waiting_on_input) {
            task_wake(io->waiting_on_input, IO_DISCONNECT);
            io->waiting_on_input = NULL;
        }
        return ERR_OK;
    }

    // We need to make sure "tcpdata" has some data in it, but we don't
    // want it to grow too big, so don't add to it if it's already over
    // 4k?
    if (io->tcpdata && (io->tcpdata->tot_len > 4096)) {
        // We can't accept this yet...
        return ERR_MEM;
    }
    tcp_recved(pcb, p->tot_len);
    if (!io->tcpdata) {
        io->tcpdata = p;
    } else {
        pbuf_cat(io->tcpdata, p);
    }
    return ERR_OK;
}


static err_t netio_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    static const char already_usb[] = "Session refused, USB already connected.\r\n";
    static const char already_net[] = "Session refused, net already connected.\r\n";
    struct io *io = (struct io *)arg;

    if (err != ERR_OK || pcb == NULL) {
        debug_printf("accept failed\r\n");
        return ERR_VAL;        
    }
    if (io->usb_is_connected) {
        err = tcp_write(pcb, already_usb, sizeof(already_usb)-1, 0);
        tcp_output(pcb);
        tcp_close(pcb);
        return ERR_OK;
    }
    if (io->pcb) {
        err = tcp_write(pcb, already_net, sizeof(already_net)-1, 0);
        tcp_output(pcb);
        tcp_close(pcb);
        return ERR_OK;
    }

    tcp_arg(pcb, (void *)io);
    //tcp_sent(pcb, netio_sent);
    tcp_recv(pcb, netio_recv);
    io->pcb = pcb;

    debug_printf("Have new TCP connection pcb=%08x\r\n", io->pcb);

    // Send the TELNET connection sequence if needed...
    if (io->support_telnet) {
        tcp_write(pcb, connect_sequence, sizeof(connect_sequence), 0);
        tcp_output(pcb);
    }
    return ERR_OK;
}




// -----------------------------------------------------------------------------------
// GENERIC IO FUNCTIONS
// -----------------------------------------------------------------------------------

int io_is_connected(struct io *io) {
    return io->usb_is_connected || (io->pcb != NULL);
}

int io_get_byte_nb(struct io *io) {
    if (circ_is_empty(io->input)) {
        return 0;
    }

    return circ_get_byte(io->input);
}

int io_get_byte(struct io *io) {
    int reason;

    // TODO: if not connected return -3 otherwise we could just hang if we drop a
    // connection when we aren't waiting (maybe only do this in the is_empty bit?)

    if (circ_is_empty(io->input)) {
        io->waiting_on_input = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    return circ_get_byte(io->input);
}

int io_peek_byte(struct io *io) {
    if (circ_is_empty(io->input)) return -1;
    return *(io->input->tail);
}

int io_put_byte(struct io *io, uint8_t ch) {
    int reason;

    if (circ_is_full(io->output)) {
        io->waiting_on_output = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    //debug_putch(ch);
    circ_add_byte(io->output, ch);
    return 0;
}

int io_put_bytes(struct io *io, uint8_t *data, int len) {
    int reason;

    if (circ_is_full(io->output)) {
        io->waiting_on_output = current_task();
        reason = task_block();
        if (reason < 0) return reason;
    }
    //debug_putch(ch);
    circ_add_bytes(io->output, data, len);
    return 0;
}

int io_read_flush(struct io *io) {
    if (io->usb_is_connected) tud_cdc_n_read_flush(io->cdc_port);
    circ_clean(io->input);
}

int io_put_hexbyte(struct io *io, uint8_t b) {
    static const char hexdigits[] = "0123456789abcdef";
    uint8_t sum = 0;
    uint8_t ch;
    
    ch = hexdigits[b >> 4];
    sum += ch;
    io_put_byte(io, ch);
    ch = hexdigits[b & 0xf];
    sum += ch;
    io_put_byte(io, ch);
    return sum;
}



static void _io_out(char ch, void *arg) {
    struct io *io = (struct io *)arg;
    io_put_byte(io, ch);
}
int io_printf(struct io *io, char *format, ...) {
    int len;
    va_list args;
    va_start(args, format);
    len = vfctprintf(_io_out, (void *)io, format, args);
    va_end(args);
    return len;
}
int io_aprintf(struct io *io, char *format, va_list args) {
    return vfctprintf(_io_out, (void *)io, format, args);
}

void io_close(struct io *io) {
    // TODO: close the network becasue we can
}


// -----------------------------------------------------------------------------------
// CONNECTION HANDLING
// -----------------------------------------------------------------------------------
//
// We need to keep track of whether we have a conection or not and wake up anything
// waiting if something disconnects.
//
// We also need to disconnect TCP if we get an incoming USB connection as USB will
// take priority (mainly because we can't easily bump it!)
//
// TODO: puts will block if something disconects and the circ is full, do we just
// want to consume everything if we are not connected?
//

static void check_connections(struct io*io) {
    if (io->cdc_port >= 0) {
        // New connection...
        if (!io->usb_is_connected && tud_cdc_n_connected(io->cdc_port)) {
            io->usb_is_connected = 1;
            debug_printf("HAVE USB CONNECT\r\n");
            if (io->pcb) {
                netio_close(io, "Session closed due to USB connection.\r\n");
            }
        }
        if (io->usb_is_connected && !tud_cdc_n_connected(io->cdc_port)) {
            io->usb_is_connected = 0;
            debug_printf("HAVE USB DISCONNECT\r\n");
            if (io->waiting_on_input) {
                task_wake(io->waiting_on_input, IO_DISCONNECT);
                io->waiting_on_input = NULL;
            }
        }
    }
}



static int cyw43_is_up = 0;

void io_poll() {
    // Generic polling first...
    tud_task();

    if (cyw43_is_up) {
        cyw43_arch_poll();
    }

    struct io *io = ios;
    while (io) {
        check_connections(io);

        if (io->usb_is_connected) {
            refill_from_usb(io);
            send_to_usb(io);
        }
        if (io->pcb) {
            refill_from_tcp(io);
            //send_to_tcp(io);            
            netio_send(io);
        }
        io = io->next;
    }
}

/**
 * @brief Intialise an IO interface, can we a CDC port or TCP port
 * 
 * If both are specified then it becomes auto-switching with USB
 * taking precedent and dropping the other one.
 * 
 * @param cdc_port 
 * @param tcp_port 
 * @return struct io* 
 */
struct io *io_init(int cdc_port, int tcp_port, int buf_size) {
    struct io *io = malloc(sizeof(struct io));
    if (!io) return NULL;

    tcp_port = 0;

    io->cdc_port = cdc_port;
    io->tcp_port = tcp_port;
    io->pcb = NULL;
    io->tcpdata = NULL;
    io->waiting_on_input = NULL;
    io->waiting_on_output = NULL;
    io->usb_is_connected = 0;
    io->support_telnet = 0;

    circ_init(&io->_input, malloc(buf_size), buf_size);
    circ_init(&io->_output, malloc(buf_size), buf_size);
    io->input = &io->_input;
    io->output = &io->_output;

    if (tcp_port) {
        if (!cyw43_is_up) {
            if (cyw43_arch_init()) {
                debug_printf("Failed to initialise wifi\r\n");
                return NULL;
            }
            cyw43_arch_enable_sta_mode();
            cyw43_is_up = 1;
        }

        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
        err_t err = tcp_bind(pcb, NULL, tcp_port);
        if (err) {
            debug_printf("failed to bind to port %d, err=%d\r\n", tcp_port, err);
            return NULL;
        }
        struct tcp_pcb *svr_pcb = tcp_listen(pcb);
        if (!svr_pcb) {
            debug_printf("failed to listen on port %d, err=%d\r\n", tcp_port, err);
            return NULL;
        }
        tcp_arg(svr_pcb, (void *)io);
        tcp_accept(svr_pcb, netio_accept);
    }
    // Add to our linked list...
    io->next = ios;
    ios = io;
    // And return...
    return io;
}

