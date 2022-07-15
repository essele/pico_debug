/**
 * @file interact.c
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-02-25
 * 
 * @copyright Copyright (c) 2022
 *
 * Module to do command line interaction (command line editing, history etc)
 * ideally supporting both buffer/length and circ options (compile time)
 */

#include "lerp/interact.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>              // isprint()
#include "lerp/task.h"
#include "lerp/io.h"
#include "lerp/debug.h"


/*
    A stack of strings ... we push new strings on the top, and the old
    ones fall off the back if they won't fit
*/
static void push_history(struct interact *i) {
    // Just move the data along enough to fit our string in with
    // the zero terminator
    int len = i->cmd->head - i->cmd->tail;
    char *p = i->history;
    int hsize = HISTORY_MAX;

    // If it's empty we don't add it..
    // If it's the same as our previous one, we also don't add it...
    // But we do need to reset hptr
    if (len == 0 || strcmp((char *)(i->cmd->tail), p) == 0) {
        i->hptr = NULL;
        return;
    }

    int needed = len + 1;   // need zero term

    memmove(p + needed, p, hsize-needed);
    memcpy(p, i->cmd->tail, len);
    *(p+len) = 0;
    i->hptr = NULL;         // not pointing at anyting
}

// Move the history pointer on, make sure we have a whole string in the
// buffer, and then update the cmd structure...
static void set_prev_history(struct interact *i) {
    struct circ *cmd = i->cmd;
    char *p = i->hptr;
    char *end = i->history + HISTORY_MAX;

    if (p != NULL) {
        // We know the current string fits, so move on and see if the
        // next one does...
        while (*p++);
        // p now points at the previous string (next in memory)
    } else {
        p = i->history;
    }

    char *new = p;
    // Now see if it fits...
    while (*p && p < end) p++;
    if (p >= end || !*new) {
        // The next string doesn't fit or is empty, we stay where we are...
        new = i->hptr;
    }
    strcpy((char *)cmd->data, new);
    cmd->head = cmd->data + strlen(new);
    cmd->tail = cmd->head;
    i->hptr = new;
    return;
}
// Move the history point back ... if we are already at the start or NULL
// then we just clear it.
static void set_next_history(struct interact *i) {
    struct circ *cmd = i->cmd;
    char *p = i->hptr;

    if (p == i->history || p == NULL) {
        i->hptr = NULL;
        cmd->head = cmd->tail = cmd->data;
        return;
    }
    // Go back past the terminating zero from the prior string
    p--; p--;
    // Now go backwards until we get a zero, or go one beyond the start
    while (*p && p >= i->history) p--;
    p++;

    strcpy((char *)cmd->data, p);
    cmd->head = cmd->data + strlen(p);
    cmd->tail = cmd->head;
    i->hptr = p;
    return;
}

/**
 * @brief Get the number of columns in the window
 * 
 * @param i 
 * @return int 
 */
static int get_columns(struct interact *i) {
    struct io *io = i->io;
    int row = 0;
    int col = 0;
    int ch;

    // Make sure we don't have any input waiting...
    io_read_flush(io);

    // save pos, go right, report pos, restore pos...
    io_printf(io, "\0337\033[999C\033[6n\0338");
    // Now write flush concept anymore (handled by io)

    // Expect ... ESC [ 1 8 ; 8 R  (where 18=row, 8=col)
    ch = io_get_byte(io);
    if (ch != '\x1b') return -1;
    ch = io_get_byte(io);
    if (ch != '[') return -1;
    while (1) {
        ch = io_get_byte(io);
        if (isdigit(ch)) {
            row *= 10;
            row += ch - '0';
            continue;
        } else if (ch == ';') {
            break;
        } else {
            return -1;
        }
    }
    while (1) {
        ch = io_get_byte(io);
        if (isdigit(ch)) {
            col *= 10;
            col += ch - '0';
            continue;
        } else if (ch == 'R') {
            break;
        } else {
            return -1;
        }
    }
    return col;
}


static void refresh_line(struct interact *i) {
    struct io *io = i->io;
    struct circ *cmd = i->cmd;
    int plen = i->plen;

    uint8_t *buf = cmd->data;
    int l = cmd->head - cmd->data;  // length
    int p = cmd->tail - cmd->data;  // position

    while (i->plen + p >= i->cols) {
        buf++; l--; p--;
    }
    while (i->plen + l >= i->cols) {
        l--;
    }
    io_printf(io, "\r%s", i->prompt);
    if (l) io_printf(io, "%.*s", l, buf);
    io_printf(io, "\033[0K\r\033[%dC", p + plen);    
}


struct interact *interact_with_circ(struct io *io, struct circ *cmd, char *prompt) {
    assert(strlen(prompt) < PROMPT_MAX);
    struct interact *i = malloc(sizeof(struct interact));
    if (!i) return NULL;

    i->io = io;
    i->nudgeable = NULL;

    strcpy(i->prompt, prompt);
    i->plen = strlen(prompt);
    memset(i->history, 0, HISTORY_MAX);

    i->cmd = cmd;
    return i;
}

struct interact *interact_with_buf(struct io *io, char *buf, int maxlen, char *prompt) {
    assert(strlen(prompt) < PROMPT_MAX);
    struct interact *i = malloc(sizeof(struct interact));
    if (!i) return NULL;

    i->io = io;
    i->nudgeable = NULL;

    strcpy(i->prompt, prompt);
    i->plen = strlen(prompt);
    memset(i->history, 0, HISTORY_MAX);

    // Setup the internal circ to use the buffer...
    i->_circ.data = (uint8_t *)buf;
    i->_circ.end = (uint8_t *)buf + maxlen;
    i->_circ.size = maxlen;
    i->_circ.head = i->_circ.tail = (uint8_t *)buf;
    i->_circ.full = 0;

    i->cmd = &(i->_circ);
    return i;
}


int interact_nudge(struct interact *i) {
    if (i->nudgeable) {
        task_wake(i->nudgeable, -10);
    }
}


int interact(struct interact *i, char *(*async_func)(void)) {
    struct io *io = i->io;
    struct circ *cmd = i->cmd;
    int ch;

    // Make sure our cmdline circ is properly reset
    cmd->head = cmd->tail = cmd->data;
    *cmd->data = 0;

    // Try to figure out the columns
    i->cols = get_columns(i);
    if (i->cols < 0) i->cols = 80;

    // Output the prompt...
    refresh_line(i);

    while(1) {
        // See if we have some async stuff to display...
        if (async_func) {
            char *async = async_func();
            if (async) {
                io_printf(io, "\r\033[0K");
                io_printf(io, "%s", async);
                refresh_line(i);
                continue;
            }
        }
        // Limit nudging for this getch() only...
        i->nudgeable = current_task();
        ch = io_get_byte(io);
        i->nudgeable = NULL;
        if (ch == -10) continue;        // interactive nudge
        if (ch < 0) return INTA_ERR;

        switch (ch) {
            case '\x1b':            // Escape sequence...
                ch = io_get_byte(io);
                if (ch == -1) return INTA_ERR;
                if (ch != '[') break;   // we only want ESC [ xxx
                ch = io_get_byte(io);
                if (ch == -1) return INTA_ERR;
                switch (ch) {
                    case 'A':       // UP
                        set_prev_history(i);
                        refresh_line(i);
                        break;
                    case 'B':       // DOWN
                        set_next_history(i);
                        refresh_line(i);
                        break;
                    case 'D':       // LEFT
                        if (cmd->tail > cmd->data) {
                            cmd->tail--;
                            refresh_line(i);
                        }
                        break;
                    case 'C':       // RIGHT
                        if (cmd->tail < cmd->head) {
                            cmd->tail++;
                            refresh_line(i);
                        }
                        break;
                }
                break;

            case '\x03':    // CTRL-C
                cmd->head = cmd->tail;            // put end where cursor is
                refresh_line(i);
                io_printf(io, "^C\r\n");
                return INTA_ABORT;

            case '\x04':    // CTRL-D
                // should close the connection (if possible) if we have nothing typed
                if (cmd->head == cmd->data) {
                    io_close(io);
                    return INTA_ERR;
                }
                break;

            case '\x08':    // BACKSPACE
            case 0x7f:      // BACKSPACE in telnet (otherwise isn't it delete?)
                if (cmd->tail > cmd->data && cmd->head > cmd->data) {
                    memmove(cmd->tail-1, cmd->tail, cmd->head - cmd->tail);
                    cmd->head--;
                    cmd->tail--;
                    refresh_line(i);
                }
                break;

            case '\r':      // ENTER
                cmd->tail = cmd->head;
                refresh_line(i);
                io_printf(io, "\r\n");
                cmd->tail = cmd->data;                            // ready to process
                push_history(i);
                return INTA_OK;

            default:
                if (isprint(ch) && cmd->head < cmd->end) {
                    if (cmd->head == cmd->tail) {
                        *cmd->tail++ = ch;
                        cmd->head++;
                        if (i->plen + (cmd->head - cmd->data) < i->cols) {
                            io_printf(io, "%c", ch);
                        } else {
                            refresh_line(i);
                        }
                    } else {
                        memmove(cmd->tail+1, cmd->tail, cmd->head - cmd->tail);
                        *cmd->tail++ = ch;
                        cmd->head++;
                        refresh_line(i);
                    }
                }
                break;      
        }
    }
}
