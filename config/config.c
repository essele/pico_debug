
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "config.h"

#include "lerp/flash.h"
#include "lerp/tokeniser.h"


struct cf _cf;
struct cf *cf = &_cf;

struct cf_main  _cf_main;
struct cf_live  _cf_live;

// Used for error messages and return values
static char    cf_err[80];

struct cf_info {
    char    *name;              // name of the item
    char    *desc;              // description of the item
    int     offset;             // offset into main cf array
    int     iv1, iv2;           // integer values for general use
    void    *vp1, *vp2;         // void pointers for general use
    char    *(*get_str_func)(struct cf_info *);
    char    *(*set_int_func)(struct cf_info *, int);
    char    *(*set_str_func)(struct cf_info *, char *);
    char    *(*set_tok_func)(struct cf_info *, struct circ *);
};

#define CF_OFFSET(item)     (offsetof(struct cf_main, item))
#define CF_INTP(offset)     ((int *)((void *)(cf->main) + offset))
#define CF_STRP(offset)     ((char *)((void *)(cf->main) + offset))

// ---------------------------------------------------------------------------------
// Generic get and set functions
// ---------------------------------------------------------------------------------
static char *set_integer_int(struct cf_info *c, int val) {
    if ((val < c->iv1) || (val > c->iv2)) {
        sprintf(cf_err, "value must be between %d and %d", c->iv1, c->iv2);
        return cf_err;
    }
    *CF_INTP(c->offset) = val;
    return NULL;
}
static char *get_integer_str(struct cf_info *c) {
    sprintf(cf_err, "%d", *CF_INTP(c->offset));
    return cf_err;
}
static char *set_boolean_int(struct cf_info *c, int val) {
    if (val < 0 || val > 1) return "value must be 0 (false) or 1 (true)";
    *CF_INTP(c->offset) = val;
    return NULL;
}
static char *set_boolean_str(struct cf_info *c, char *val) {
    int *p = CF_INTP(c->offset);
    if (strcasecmp(val, "true") == 0) *p = 1;
    else if (strcasecmp(val, "yes") == 0) *p = 1;
    else if (strcasecmp(val, "enable") == 0) *p = 1;
    else if (strcasecmp(val, "false") == 0) *p = 0;
    else if (strcasecmp(val, "no") == 0) *p = 0;
    else if (strcasecmp(val, "disable") == 0) *p = 0;
    else return "value must be true, yes, enable, false, no, disable.";
    return NULL;
}
static char *get_boolean_str(struct cf_info *c) {
    int v = *CF_INTP(c->offset);
    if (v) return "true";
    return "false";
}
static char *set_string_str(struct cf_info *c, char *val) {
    int l = strlen(val);
    if ((l < c->iv1) || (l > c->iv2)) {
        sprintf(cf_err, "value must be between %d and %d characters", c->iv1, c->iv2);
        return cf_err;
    }
    strcpy(CF_STRP(c->offset), val);
    return NULL;
}
static char *get_string_str(struct cf_info *c) {
    return CF_STRP(c->offset);
}


// ---------------------------------------------------------------------------------
// Default values for our config...
// ---------------------------------------------------------------------------------

static const struct cf_main cf_main_defaults = {
    .version = CF_VERSION,
    .swd = { .speed = 25000, .pin_clk = 2, .pin_io = 3 },
    .dhcp = { .enable = 1 },
    .wifi = { .ssid = "", .creds = "" },
};

// ---------------------------------------------------------------------------------
// The main configuration table...
// ---------------------------------------------------------------------------------

static const struct cf_info cf_list[] = {
    { "swd.speed", "sets the speed of the SWD interface in kHz (1-25000)", 
        CF_OFFSET(swd.speed), 1, 25000, NULL, NULL,
        get_integer_str, set_integer_int, NULL, NULL },
    { "swd.pin_clk", "the CLK pin for the SWD interface",
        CF_OFFSET(swd.pin_clk), 1, 31, NULL, NULL,
        get_integer_str, set_integer_int, NULL, NULL },
    { "swd.pin_io", "the DATA pin for the SWD interface",
        CF_OFFSET(swd.pin_io), 1, 31, NULL, NULL,
        get_integer_str, set_integer_int, NULL, NULL },

    { "wifi.ssid", "the SSID for the WIFI interface",
        CF_OFFSET(wifi.ssid), 0, 32, NULL, NULL,
        get_string_str, NULL, set_string_str, NULL },
    { "wifi.creds", "the password for the WIFI interface",
        CF_OFFSET(wifi.creds), 0, 32, NULL, NULL,
        get_string_str, NULL, set_string_str, NULL },

    { NULL, NULL, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL }
};      

struct cf_info *cf_get_byname(char *name) {
    struct cf_info *c = (struct cf_info *)cf_list;
    while (c->name) {
        if (strcmp(c->name, name) == 0) return c;
        c++;
    }
    return NULL;
}

char *cf_next_item(char *prev) {
    if (!prev) return cf_list[0].name;

    struct cf_info *c = cf_get_byname(prev);
    if (!c) return NULL;
    c++;
    return c->name;
}

char *cf_get_desc(char *item) {
    struct cf_info *c = cf_get_byname(item);
    if (!c) return "<err>";
    return c->desc;
}
char *cf_get_strval(char *item) {
    struct cf_info *c = cf_get_byname(item);
    if (!c) return "<err>";
    return (c->get_str_func(c));
}

int cf_max_name_len() {
    static int max_l = 0;

    if (max_l) return max_l;

    struct cf_info *c = (struct cf_info *)cf_list;
    while (c->name) {
        int l = strlen(c->name);
        if (l > max_l) max_l = l;
        c++;
    }
    return max_l;
}

char *cf_set_with_tokens(char *name, struct circ *circ) {
    const struct cf_info *c = cf_get_byname(name);

    if (!c) return "unknown configuration item";

    // If we have a token processing function, then use that...
    if (c->set_tok_func) {
        return c->set_tok_func((struct cf_info *)c, circ);
    }
    // Otherwise figure out what kind of token it is...
    int token = token_get(circ);

    // We need at least one...

    switch(token) {
        case TOK_END:
        case TOK_ERROR:
            return "expected a value";
        
        case TOK_INTEGER:
            if (!c->set_int_func) return "unable to set item with integer";
            return c->set_int_func((struct cf_info *)c, token_int());

        case TOK_WORD:
        case TOK_STRING:
            if (!c->set_str_func) return "unable to set item with word/string";
            return c->set_str_func((struct cf_info *)c, token_string());

        default:
            return "unable to set item with that value";
    }
}

/**
 * @brief Handle config changes between versions
 * 
 * @param oldversion 
 */
void cf_version_upgrade(int oldversion) {
    // Start with the defaults...
    memcpy(cf->main, &cf_main_defaults, sizeof(struct cf_main));


    // Then copy over (or adjust) anything we want to keep
    switch(oldversion) {

        case 1:     
            break;


        case 0:         // we didn't have anything, or...
        default:        // anythng else we just live with the defaults
            break;
    }
    return;
}


void config_save() {
    write_file("config.cf", (uint8_t *)cf->main, sizeof(struct cf_main));
    // And update cf->flash to point to the new file...
    cf->flash = file_addr("config.cf", NULL);
}


void config_init() {

    flash_init();

    // Now see if we have a flash config file...
    cf->flash = file_addr("config.cf", NULL);

    // Setup the main config structures...
    cf->main = &_cf_main;
    cf->live = &_cf_live;

    if (!cf->flash || (cf->flash->version != CF_VERSION)) {
        // Missing config or old version...
        cf_version_upgrade(cf->flash ? cf->flash->version : 0);
        config_save();
    } else {
        // Copy flash to the main cf...
        memcpy(cf->main, cf->flash, sizeof(struct cf_main));
    }
}