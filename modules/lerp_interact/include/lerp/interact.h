/**
 * @file interactive.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-02-25
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef __INTERACT_H
#define __INTERACT_H

#include "lerp/circ.h"

#define INTA_OK                   0
#define INTA_ERR                  -1
#define INTA_ABORT                -2

#define CMDLINE_RUNNING         0       // Nothing more to do
#define CMDLINE_DONE            1       // We have pressed return
#define CMDLINE_ADV             2       // Switch to advanced mode
#define CMDLINE_COMP            3       // Completion called

#define PROMPT_MAX          32
#define HISTORY_MAX         1024
#define CONN_TIME_WAIT_US   200000

struct interact {
    struct io           *io;

    // In order to use the interactive system we need to be able to call
    // a number of io funcions.
//    int     (*func_getch)(void);        // a getch function
 //   int     (*func_putch)(int ch);      // a putch function
//    void    (*func_rdflush)(void);      // a read flush function
//    void    (*func_wrflush)(void);      // a write flush function
//    int     (*func_printf)(char *fmt, ...);     // a printf function
//    void    (*func_close)(void);        // a close function

//    struct connection   *c;         // our connection for io
    struct circ         *cmd;       // our command line

    // History bits...
    char    history[HISTORY_MAX];
    char    *hptr;

    char    prompt[PROMPT_MAX];
    int     plen;
    int     cols;

    // A struct circ for use when we have a buffer in real life
    struct circ     _circ;

    // A task pointer so we can potentially nudge the task to
    // re-look at the async function, but we should only do that
    // when we are waiting for input characters.
    struct task     *nudgeable;
};

int interact(struct interact *i, char *(*async_func)(void));
int interact_nudge(struct interact *i);
struct interact *interact_with_circ(struct io *io, struct circ *cmd, char *prompt);
struct interact *interact_with_buf(struct io *io, char *buf, int maxlen, char *prompt);

#endif