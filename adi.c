#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "adi.h"
#include "swd.h"

extern int usb_n_printf(int n, char *format, ...);
#define debug_printf(...) usb_n_printf(1, __VA_ARGS__)

//
// Debug Port Register Addresses
//
#define DP_DPIDR                        0x00U   // IDCODE Register (RD)
#define DP_ABORT                        0x00U   // Abort Register (WR)
#define DP_CTRL_STAT                    0x04U   // Control & Status
#define DP_RESEND                       0x08U   // Resend (RD)
#define DP_SELECT                       0x08U   // Select Register (WR)
#define DP_RDBUFF                       0x0CU   // Read Buffer (RD)
#define DP_TARGETSEL                    0x0CU   // Read Buffer (WR)

#define DP_DLCR                         0x14    // (RW)
#define DP_TARGETID                     0x24    // Target ID (RD)
#define DP_DLPIDR                       0x34    // (RD)
#define DP_EVENTSTAT                    0x44    // (RO)

//
// Control/Status Register Defines
//
#define ORUNDETECT          (1<<0)
#define STICKYORUN          (1<<1)
#define STICKYCMP           (1<<4)
#define STICKYERR           (1<<5)
#define WDATAERR            (1<<7)
#define SWDERRORS           (STICKYORUN|STICKYCMP|STICKYERR)    

#define CDBGPWRUPREQ        (1<<28)
#define CDBGPWRUPACK        (1<<29)
#define CSYSPWRUPREQ        (1<<30)
#define CSYSPWRUPACK        (1<<31)

//
// Abort Register Defines
//
#define DAP_ABORT           (1<<0)
#define STKCMPCLR           (1<<1)          
#define STKERRCLR           (1<<2)
#define WDERRCLR            (1<<3)
#define ORUNERRCLR          (1<<4)
#define ALLERRCLR           (STKCMPCLR|WDERRCLR|WDERRCLR|ORUNERRCLR)


// ----------------------------------------------------------------------------
// We need a few things on a per-core basis...
// ----------------------------------------------------------------------------

struct reg {
    int valid;
    uint32_t value;
};

enum {
    STATE_UNKNOWN,
    STATE_RUNNING,
    STATE_HALTED,
};

enum {
    REASON_UNKNOWN,
    REASON_DEBUG,
    REASON_BREAKPOINT,
    REASON_STEP,
    REASON_RESET,
};

struct core {
    int         state;
    int         reason;

    uint32_t    dp_select_cache;
    uint32_t    ap_mem_csw_cache;

    uint32_t    breakpoints[4];
    struct reg  reg_cache[24];
};


struct core cores[2];

// Core will point at whichever one is current...
struct core *core = &cores[0];


// ----------------------------------------------------------------------------
// Slightly Higher Level Functions
// ----------------------------------------------------------------------------


/**
 * @brief A cached copy of whats in the DP_SELECT register so that we can
 *        be efficient about updating it.
 */
//static int dp_select_cache = 0xffffffff;

/**
 * @brief Change the dp bank in SELECT if it needs changing
 * 
 * @param bank 
 * @return int 
 */
static inline int dp_select_bank(int bank) {
    int rc = SWD_OK;

    assert(bank <= 0xf);

    if ((core->dp_select_cache & 0xf) != bank) {
        core->dp_select_cache = (core->dp_select_cache & 0xfffffff0) | bank;
        rc = swd_write(0, DP_SELECT, core->dp_select_cache);
    }
    return rc;
}

/**
 * @brief Select the AP and bank if we need to (note bank is bits 4-7)
 * 
 * @param ap 
 * @param bank 
 * @return int 
 */
static inline int ap_select_with_bank(uint ap, uint bank) {
    int rc = SWD_OK;

    assert((bank & 0x0f) == 0);
    assert(bank <= 255);
    assert(ap <= 255);

    if ((ap != (core->dp_select_cache >> 24)) || (bank != (core->dp_select_cache & 0xf0))) {
        core->dp_select_cache = (ap << 24) | bank | (core->dp_select_cache & 0xf);
        rc = swd_write(0, DP_SELECT, core->dp_select_cache);
    }
    return rc;
}

int dp_read(uint32_t addr, uint32_t *res) {
    int rc;

    // First check to see if we are reading something where we might
    // care about the dp_banksel
    if ((addr & 0x0f) == 4) {
        rc = dp_select_bank((addr & 0xf0) >> 4);
        if (rc != SWD_OK) return rc;
    }
    return swd_read(0, addr & 0xf, res);
}

int dp_write(uint32_t addr, uint32_t value) {
    int rc;

    // First check to see if we are writing something where we might
    // care about the dp_banksel
    if ((addr & 0x0f) == 4) {
        rc = dp_select_bank((addr & 0xf0) >> 4);
        if (rc != SWD_OK) return rc;
    }
    return swd_write(0, addr & 0xf, value);
}

// Reset is at least 50 bits of 1 followed by zero's
static const uint32_t reset_seq[] = { 0xffffffff, 0x0003ffff };

// Selection alert sequence... 128bits
static const uint32_t selection_alert_seq[] = { 0x6209F392, 0x86852D95, 0xE3DDAFE9, 0x19BC0EA2 };

// Zero sequence...
static const uint32_t zero_seq[] = { 0x00000000 };
static const uint32_t ones_seq[] = { 0xffffffff };

// Activation sequence (8 bits)
static const uint32_t act_seq[] = { 0x1a };

int core_select(int num) {
    uint32_t dpidr;
    uint32_t dlpidr;
    uint32_t targetid;
    
    // See if we are already selected...
    if (core == &cores[num]) return SWD_OK;
    
    targetid = num == 0 ? 0x01002927 : 0x11002927;

    swd_send_bits((uint32_t *)reset_seq, 52);
    swd_targetsel(targetid);

    // Now read the DPIDR register... this must be next (so swd_read)
    CHECK_OK(swd_read(0, DP_DPIDR, &dpidr));

    // Need to switch the core here for dp_read to work...
    core = &cores[num];

    // If that was ok we can validate the switch by checking the TINSTANCE part of
    // DLPIDR
    CHECK_OK(dp_read(DP_DLPIDR, &dlpidr));

    if ((dlpidr & 0xf0000000) != (targetid & 0xf0000000)) return SWD_ERROR;


    return SWD_OK;
}

int core_get() {
    return (core == &cores[0]) ? 0 : 1;
}


/**
 * @brief Send the required sequence to reset the line and start SWD ops
 * 
 */
int dp_initialise() {
    int rc;

    for (int i=0; i < 2; i++) {
        cores[i].state = STATE_UNKNOWN;
        cores[i].reason = REASON_UNKNOWN;
        cores[i].dp_select_cache = 0xffffffff;
        cores[i].ap_mem_csw_cache = 0xffffffff;
        for (int j=0; j < 4; j++) {
            cores[i].breakpoints[j] = 0xffffffff;
        }
        for (int j=0; j < sizeof(cores[i].reg_cache)/sizeof(struct reg); j++) {
            cores[i].reg_cache[j].valid = 0;
        }
    }
    core = NULL;

//    swd_send_bits(reset_seq, 64);   // includes zeros
    swd_send_bits((uint32_t *)ones_seq, 8);
    swd_send_bits((uint32_t *)selection_alert_seq, 128);
    swd_send_bits((uint32_t *)zero_seq, 4);
    swd_send_bits((uint32_t *)act_seq, 8);
    swd_send_bits((uint32_t *)ones_seq, 8);
    swd_send_bits((uint32_t *)reset_seq, 64);   // includes zeros

    // Now a reset sequence
//    swd_send_bits((uint32_t *)reset_seq, 64);


//    swd_targetsel(0x01002927);
//    swd_send_bits((uint32_t *)zero_seq, 20);

    // Now read the status register... this must be next (so swd_read)
//    rc = swd_read(0, 0, &id);
//    if (rc != SWD_OK) return rc;

    core_select(0);

    // And then try to clear any errors...
    rc = swd_write(0, DP_ABORT, ALLERRCLR);
    return rc;
}


/**
 * @brief Do everything we need to be able to utilise to the AP's
 * 
 * This powers on the needed subdomains so that we can access the
 * other AP's.
 * 
 * @return int 
 */
int dp_power_on() {
    uint32_t    rv;

    for (int i=0; i < 10; i++) {
        // Attempt to power up...
        if (dp_write(DP_CTRL_STAT, CDBGPWRUPREQ|CSYSPWRUPREQ) != SWD_OK) continue;
        if (dp_read(DP_CTRL_STAT, &rv) != SWD_OK) continue;
        if (rv & SWDERRORS) continue;
        if (!(rv & CDBGPWRUPACK)) continue;
        if (!(rv & CSYSPWRUPACK)) continue;
        return SWD_OK;
    }
    return SWD_ERROR;
}


/**
 * @brief Read a value from the given AP
 * 
 * This means setting it as the SELECTED ap, setting up the bank, reading
 * the value and then getting it from the buffer.
 * 
 * Note: this will destroy *res even if it fails, might want to rethink it.
 * 
 * @param apnum 
 * @param addr 
 * @param res 
 * @return int 
 */
int ap_read(int apnum, uint32_t addr, uint32_t *res) {
    int rc;

    // Select the AP and bank (if needed)
    rc = ap_select_with_bank(apnum, addr & 0xf0);
    if (rc != SWD_OK) return rc;

    // Now kick off the read (addr[3:2])...
    rc = swd_read(1, addr&0xc, res);
    if (rc != SWD_OK) return rc;

    // Now read the real result from 
    rc = swd_read(0, DP_RDBUFF, res);
    return rc;
}
/**
 * @brief The defer version is used for streaming type access where
 *        we return the value from the read (which will be the previous
 *        result) ... the should be followed by a ap_read_last()
 * 
 * @param apnum 
 * @param addr 
 * @param res 
 * @return int 
 */
int ap_read_defer(int apnum, uint32_t addr, uint32_t *res) {
    int rc;

    // Select the AP and bank (if needed)
    rc = ap_select_with_bank(apnum, addr & 0xf0);
    if (rc != SWD_OK) return rc;

    // Now kick off the read (addr[3:2])...
    rc = swd_read(1, addr&0xc, res);
    return rc;
}

int ap_read_last(uint32_t *res) {
    return swd_read(0, DP_RDBUFF, res);
}

/**
 * @brief Write a value to a given AP
 * 
 * @param apnum 
 * @param addr 
 * @param value 
 * @return int 
 */
int ap_write(int apnum, uint32_t addr, uint32_t value) {
    int rc;

    // Select the AP and bank (if needed)
    rc = ap_select_with_bank(apnum, addr & 0xf0);
    if (rc != SWD_OK) return rc;

    // Now kick off the write (addr[3:2])...
    rc = swd_write(1, addr&0xc, value);
    return rc;
}



// DBGSWENABLE, AHB_MASTER_DEBUG, HPROT1, no-auto-inc, need to add size...
#define AP_MEM_CSW_SINGLE     (1 << 31) \
                            | (1 << 29) \
                            | (1 << 25) \
                            | (0 << 4) 

#define AP_MEM_CSW_32       0b010
#define AP_MEM_CSW_16       0b001
#define AP_MEM_CSW_8        0b000

#define AP_MEM_CSW          0x00
#define AP_MEM_TAR          0x04
#define AP_MEM_DRW          0x0C

// DBGSWENABLE, AHB_MASTER_DEBUG, HPROT1, auto-inc, 32-bit
#define AP_MEM_CSW_INC        (1 << 31) \
                            | (1 << 29) \
                            | (1 << 25) \
                            | (1 << 4)


/**
 * @brief Update the memory csw if we need to
 * 
 * @param value 
 */
static inline int ap_mem_set_csw(uint32_t value) {
//    static uint32_t ap_mem_csw_cache = 0xffffffff;
    int rc = SWD_OK;

    if (core->ap_mem_csw_cache != value) {
        core->ap_mem_csw_cache = value;
        rc = ap_write(0, AP_MEM_CSW, value);
    }
    return rc;
}


int mem_read32(uint32_t addr, uint32_t *res) {
    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_SINGLE | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
    return ap_read(0, AP_MEM_DRW, res);
}

int mem_read8(uint32_t addr, uint8_t *res) {
    uint32_t v;

    // Actually do a 32 bit read - may save a CSW update...
    CHECK_OK(mem_read32(addr & 0xfffffffc, &v));

    *res = v >> ((addr & 3) << 3);
    return SWD_OK;
}
int mem_read16(uint32_t addr, uint16_t *res) {
    uint32_t v;

    // Actually do a 32 bit read - may save a CSW update...
    CHECK_OK(mem_read32(addr & 0xfffffffc, &v));

    *res = (addr & 2) ? (v >> 16) : (v & 0xffff);
    return SWD_OK;
}

int mem_write8(uint32_t addr, uint8_t value) {
    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_SINGLE | AP_MEM_CSW_8));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
    return ap_write(0, AP_MEM_DRW, value << ((addr & 3) << 3));
}

int mem_write16(uint32_t addr, uint16_t value) {
    assert((addr & 1) == 0);            // Must be 16 bit aligned

    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_SINGLE | AP_MEM_CSW_16));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
    return (ap_write(0, AP_MEM_DRW, (addr & 2) ? value << 16: value));
}
int mem_write32(uint32_t addr, uint32_t value) {
    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_SINGLE | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
    return ap_write(0, AP_MEM_DRW, value);
}

/**
 * @brief Writes memory to the target it 32bit aligned chunks
 * 
 * Both the src and dst need to be aligned on a 32bit boundary and count
 * needs to be a multiple of 4.
 * 
 * @param addr 
 * @param count 
 * @param src 
 * @return int 
 */
static int mem_write_block_aligned(uint32_t addr, uint32_t count, uint32_t *src) {
    // Set auto-increment and the starting address...
    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_INC | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

    // We count in 32bit words...
    count >>= 2;

    while(count--) {
        CHECK_OK(ap_write(0, AP_MEM_DRW, *src++));

        // We need to track the address to deal with the 1k wrap limit
        addr += 4;
        if ((addr & 0x3ff) == 0) {
            CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
        }
    }
    return SWD_OK;
}
/**
 * @brief Writes memory to the target when it's not aligned
 * 
 * The arget addr needs to be aligned on a 32bit boundary but the src
 * doesn't.
 * 
 * @param addr 
 * @param count 
 * @param src 
 * @return int 
 */
static int mem_write_block_unaligned(uint32_t addr, uint32_t count, uint8_t *src) {
    uint32_t v32;

    // Set auto-increment and the starting address...
    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_INC | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

    // We count in 32bit words...
    count >>= 2;

    while(count--) {
        v32 = *src++;
        v32 |= (*src++) << 8;
        v32 |= (*src++) << 16;
        v32 |= (*src++) << 24;
        CHECK_OK(ap_write(0, AP_MEM_DRW, v32));

        // We need to track the address to deal with the 1k limit
        addr += 4;
        if ((addr & 0x3ff) == 0) {
            CHECK_OK(ap_write(0, AP_MEM_TAR, addr));
        }
    }
    return SWD_OK;
}

int mem_write_block(uint32_t addr, uint32_t count, uint8_t *src) {
    uint16_t v16;

    // The first phase is getting to an aligned address if we aren't...
    if (addr & 3) {
        if ((addr & 1) && count) {
            CHECK_OK(mem_write8(addr++, *src++));
            count--;
            if (!count) return SWD_OK;
        }
        if ((addr & 2) && count) {
            if (count == 1) return mem_write8(addr, *src);
            v16 = *src++;
            v16 |= (*src++) << 8;
            CHECK_OK(mem_write16(addr, v16));
            count -= 2;
            if (!count) return SWD_OK;
            addr += 2;
        }
    }

    // At this point we have an aligned addr, see if we can optimise...
    if (count >= 4) {
        if (((uint32_t)src & 3) == 0) {
            CHECK_OK(mem_write_block_aligned(addr, (count & ~3), (uint32_t *)src));
        } else {
            CHECK_OK(mem_write_block_unaligned(addr, (count & ~3), src));
        }
        count = count & 3;
        if (!count) return SWD_OK;
    }

    // If we get here then we have some stragglers to deal with...
    if (count & 2) {
        v16 = *src++;
        v16 |= (*src++) << 8;
        CHECK_OK(mem_write16(addr, v16));
        count -= 2;
        if (!count) return SWD_OK;
        addr += 2;
    }
    // Must be one byte left to do...
    return mem_write8(addr, *src);
}



int mem_read_block_unaligned(uint32_t addr, uint32_t count, uint8_t *dest) {
    uint32_t v32;
    
    // We count in 32bit words...
    count >>= 2;

    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_INC | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

    CHECK_OK(ap_read_defer(0, AP_MEM_DRW, &v32));
    while (--count) {
        addr += 4;
        if ((addr & 0x3ff) == 0) CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

        CHECK_OK(ap_read_defer(0, AP_MEM_DRW, &v32));
        *dest++ = (v32 & 0xff);
        *dest++ = (v32 & 0xff00) >> 8;
        *dest++ = (v32 & 0xff0000) >> 16;
        *dest++ = v32 >> 24;
    }
    CHECK_OK(ap_read_last(&v32));
        *dest++ = (v32 & 0xff);
        *dest++ = (v32 & 0xff00) >> 8;
        *dest++ = (v32 & 0xff0000) >> 16;
        *dest++ = v32 >> 24;

    return SWD_OK;
}

int mem_read_block_aligned(uint32_t addr, uint32_t count, uint32_t *dest) {
    // We count in 32bit words...
    count >>= 2;

    CHECK_OK(ap_mem_set_csw(AP_MEM_CSW_INC | AP_MEM_CSW_32));
    CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

    CHECK_OK(ap_read_defer(0, AP_MEM_DRW, dest));
    while (--count) {
        addr += 4;
        if ((addr & 0x3ff) == 0) CHECK_OK(ap_write(0, AP_MEM_TAR, addr));

        CHECK_OK(ap_read_defer(0, AP_MEM_DRW, dest++));
    }
    return ap_read_last(dest);
}


int mem_read_block(uint32_t addr, uint32_t count, uint8_t *dest) {
    uint16_t v16;

    // The first phase is getting to an aligned address if we aren't...
    if (addr & 3) {
        if ((addr & 1) && count) {
            CHECK_OK(mem_read8(addr++, dest++));
            count--;
            if (!count) return SWD_OK;
        }
        if ((addr & 2) && count) {
            if (count == 1) return mem_read8(addr, dest);
            CHECK_OK(mem_read16(addr, &v16));
            *dest++ = v16 & 0xff;
            *dest++ = (v16 & 0xff00) >> 8;
            count -= 2;
            if (!count) return SWD_OK;
            addr += 2;
        }
    }

    // At this point we have an aligned addr, see if we can optimise...
    if (count >= 4) {
        if (((uint32_t)dest & 3) == 0) {
            CHECK_OK(mem_read_block_aligned(addr, (count & 0xfffffffc), (uint32_t *)dest));
        } else {
            CHECK_OK(mem_read_block_unaligned(addr, (count & 0xfffffffc), dest));
        }
        dest += count & 0xfffffffc;
        count = count & 3;
        if (!count) return SWD_OK;
    }

    // If we get here then we have some stragglers to deal with...
    if (count & 2) {
        CHECK_OK(mem_read16(addr, &v16));
        *dest++ = v16 & 0xff;
        *dest++ = (v16 & 0xff00) >> 8;
        count -= 2;
        if (!count) return SWD_OK;
        addr += 2;
    }
    // Must be one byte left to do...
    return mem_read8(addr, dest);
}




volatile int ret;


static volatile uint32_t xx;
static volatile uint32_t id;
//static volatile uint32_t rc;

uint32_t buffer[100];

int dp_init() {
    // Initialise and switch out of dormant mode (select target 0)
    if (dp_initialise() != SWD_OK) panic("unable to initialise dp\r\n");
    if (dp_power_on() != SWD_OK) panic("unable to power on dp (core0)\r\n");
    if (core_enable_debug() != SWD_OK) panic("unable to enable debug on core0\r\n");
    if (core_select(1) != SWD_OK) panic("unable to select core1\r\n");
    if (dp_power_on() != SWD_OK) panic("unable to power on core1\r\n");
    if (core_enable_debug() != SWD_OK) panic("unable to enable debug on core1\r\n");
    if (core_select(0) != SWD_OK) panic("unable reselect core 0\r\n");
    return SWD_OK;
}


#define DCB_DHCSR       0xE000EDF0
#define DCB_DCRSR       0xE000EDF4
#define DCB_DCRDR       0xE000EDF8
#define DCB_DEMCR       0xE000EDFC
#define DCB_DSCSR       0xE000EE08

#define NVIC_AIRCR      0xE000ED0C


//struct reg reg_cache[24];

#define REG_PC          15  

void reg_flush_cache() {
    for (int i=0; i < sizeof(core->reg_cache)/sizeof(struct reg); i++) {
        core->reg_cache[i].valid = 0;
    }
}


/**
 * @brief Read a core register ... we already assume we are in debug state
 * 
 * @param reg 
 * @param res 
 * @return int 
 */
int reg_read(int reg, uint32_t *res) {
    int rc;
    uint32_t value;

    if (0 && core->reg_cache[reg].valid) {
        *res = core->reg_cache[reg].value;
        return SWD_OK;
    }

    rc = mem_write32(DCB_DCRSR, (0 << 16) | (reg & 0x1f));
    if (rc != SWD_OK) return rc;

    // We are supposed to wait for the reg ready flag ... but it seems we don't
    // need it?
    while (1) {
        rc = mem_read32(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00010000) break;
    }
    CHECK_OK(mem_read32(DCB_DCRDR, &value));
    core->reg_cache[reg].value = value;
    core->reg_cache[reg].valid = 1;
    *res = value;
    return SWD_OK;
}
int reg_write(int reg, uint32_t value) {
    int rc;

    core->reg_cache[reg].value = value;
    core->reg_cache[reg].valid = 1;

    // Write the data into the RDR
    rc = mem_write32(DCB_DCRDR, value);
    if (rc != SWD_OK) return rc;

    // Now write the reg number
    rc = mem_write32(DCB_DCRSR, (1 << 16) | (reg & 0x1f));
    if (rc != SWD_OK) return rc;

    // Now wait until it's done
    while (1) {
        rc = mem_read32(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00010000) break;
    }
    return SWD_OK;
}


#define BPCR        0xE0002000
//static uint32_t breakpoints[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
static const uint32_t bp_reg[4] = { 0xE0002008, 0xE000200C, 0xE0002010, 0xE0002014 };

int bp_find(uint32_t addr) {
    for (int i=0; i < 4; i++) {
        if (core->breakpoints[i] == addr) return i;
    }
    return -1;
}

int bp_set(uint32_t addr) {
    int rc;
    uint32_t matchword;
    int bp = bp_find(addr);
    if (bp != -1) return SWD_OK;        // already have it
    bp = bp_find(0xffffffff);
    if (bp == -1) return SWD_ERROR;     // no slots available

    // Set the breakpoint...
    core->breakpoints[bp] = addr;

    matchword = (addr & 2) ? (0b10 << 30) : (0b01 << 30);

    rc = mem_write32(bp_reg[bp], matchword | (addr & 0x1ffffffc) | (1));
    if (rc != SWD_OK) return rc;

    // Turn on the breakpoint system...
    rc = mem_write32(BPCR, (1<<1) | 1);
    return rc;
}

int bp_clr(uint32_t addr) {
    int rc;

    int bp = bp_find(addr);
    if (bp == -1) return SWD_OK;        // we don't have it? Error?
    core->breakpoints[bp] = 0xffffffff;
    rc = mem_write32(bp_reg[bp], 0);      // fully disabled
    rc = mem_write32(bp_reg[bp], 0);      // fully disabled
    return rc;
}

int bp_is_set(uint32_t addr) {
    for (int i=0; i < 4; i++) {
        if (core->breakpoints[i] == addr) return 1;
    }
    return 0;
}


int core_enable_debug() {
    return mem_write32(DCB_DHCSR, (0xA05F << 16) | 1);
}

int core_halt() {
    int rc;
    uint32_t value;

    reg_flush_cache();

    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (1<<1) | 1);
//    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (1<<1) | 1);
    if (rc != SWD_OK) return rc;
    while (1) {
        rc = mem_read32(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00020000) break;
    }
    return SWD_OK;
}

int core_unhalt() {
    int rc;
    
    reg_flush_cache();

//    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (1 <<3) | (0<<1) | 1);
    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;
    core->state = STATE_RUNNING;
    return SWD_OK;
    // TODO: more?
}

int core_unhalt_with_masked_ints() {
    int rc;

    reg_flush_cache();

    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;
    core->state = STATE_RUNNING;
    return SWD_OK;
}


int core_step() {
    int rc;

    // step and !halt...
    rc = mem_write32(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (1<<2) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;

    reg_flush_cache();

    core->state = STATE_RUNNING;
    return SWD_OK;
}

int core_step_avoiding_breakpoint() {
    uint32_t pc;
    int      had_breakpoint = 0;

    CHECK_OK(reg_read(REG_PC,&pc));
    if (bp_is_set(pc)) {
        bp_clr(pc);
        had_breakpoint = 1;
    }

    CHECK_OK(core_step());

    // Now wait for a halt again...
//    while (1) {
//        rc = mem_read32(DCB_DHCSR, &value);
//        if (rc != SWD_OK) return rc;
//        if (value & 0x00020000) break;
//    }

    // And put the breakpoint back...
    if (had_breakpoint) {
        bp_set(pc);
    }
    return SWD_OK;
}

int core_is_halted() {
    int rc;
    uint32_t value;

    rc = mem_read32(DCB_DHCSR, &value);

    if (rc != SWD_OK) {
        panic("HERE");
    }
    if (rc != SWD_OK) return -1;
    if (value & 0x00020000) return 1;
    return 0;
}

int core_update_status() {
    uint32_t dhcsr;

    CHECK_OK(mem_read32(DCB_DHCSR, &dhcsr));

    // Are we halted or running...
    if (dhcsr & (1<<17)) {
        core->state = STATE_HALTED;
    } else {
        core->state = STATE_RUNNING;
    }

    // Do we know why?
    if (dhcsr & (1<<25)) {
        core->reason = REASON_RESET;
    }
    return SWD_OK;
}

// If one core stops we need to stop the other one....
// we return the one that stopped (or -1 if neither did)
int check_cores() {
    int cur = core_get();
    int other = 1-cur;
    int old_state = core->state;
    int to_halt = -1;
    int rc = -1;

    // The first phase is just gathering some info...
    core_update_status();
    if ((core->state == STATE_HALTED) && core->state != old_state) {
        // We must have stopped... so we should stop the other one
        to_halt = other;
        rc = cur;
    }
    core_select(other);
    old_state = core->state;
    core_update_status();
    if ((core->state == STATE_HALTED) && core->state != old_state) {
        to_halt = cur;
        rc = other;
    }

    // At this point we have other selected, so we can halt it if needed...
    if (to_halt == other) {
        if (core->state != STATE_HALTED) {
            debug_printf("Halting core: %d\r\n", other);
            core_halt();
        }
    }

    // Go back to the orginal one and see if we needed to stop that...
    core_select(cur);
    if (to_halt == cur) {
        if (core->state != STATE_HALTED) {
            debug_printf("Halting core: %d\r\n", cur);
            core_halt();
        }
    }
    return rc;
}


/**
 * @brief Reset the core but stop it from executing any instructions
 * 
 * @return int 
 */
int core_reset_halt() {
    int rc;
    uint32_t value;

    reg_flush_cache();

    // First halt the core...
    core_halt();

    // Now set the DWTENA and VC_CORERESET bits...
    rc = mem_write32(DCB_DEMCR, (1<<24) | (1 << 0));
    if (rc != SWD_OK) return rc;

    // Now reset the core (will be caught by the above)
    rc = mem_write32(NVIC_AIRCR, (0x05FA << 16) | (1 << 2));

    // Now make sure we get a reset flag....
    while (1) {
        rc = mem_read32(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & (1<<25)) break;
    }

    // Then make sure it clears...
    while (1) {
        rc = mem_read32(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (!(value & (1<<25))) break;
    }

    // Now clear the CORERESET bit...
    rc = mem_write32(DCB_DEMCR, (1<<24) | (0 << 0));
    return rc;
}




// this is 'M' 'u', 1 (version)
#define BOOTROM_MAGIC 0x01754d
#define BOOTROM_MAGIC_ADDR 0x00000010


uint32_t rp2040_find_rom_func(char ch1, char ch2) {
    uint16_t tag = (ch2 << 8) | ch1;

    // First read the bootrom magic value...
    uint32_t magic;
    int rc;

    rc = mem_read32(BOOTROM_MAGIC_ADDR, &magic);
    if (rc != SWD_OK) return 0;
    if ((magic & 0xffffff) != BOOTROM_MAGIC) return 0;

    // Now find the start of the table...
    uint16_t v;
    uint32_t tabaddr;
    rc = mem_read16(BOOTROM_MAGIC_ADDR+4, &v);
    if (rc != SWD_OK) return 0;
    tabaddr = v;

    // Now try to find our function...
    uint16_t value;
    do {
        rc = mem_read16(tabaddr, &value);
        if (rc != SWD_OK) return 0;
        if (value == tag) {
            rc = mem_read16(tabaddr+2, &value);
            if (rc != SWD_OK) return 0;
            return (uint32_t)value;
        }
        tabaddr += 4;
    } while(value);
    return 0;
}


int rp2040_call_function(uint32_t addr, uint32_t args[], int argc) {
    static uint32_t trampoline_addr = 0;
    static uint32_t trampoline_end;
    int rc;
    uint32_t r0;

    assert(argc <= 4);

    // First get the trampoline address...
    if (!trampoline_addr) {
        trampoline_addr = rp2040_find_rom_func('D', 'T');
        trampoline_end = rp2040_find_rom_func('D', 'E');
        if (!trampoline_addr || !trampoline_end) return SWD_ERROR;
    }

    // Set the registers for the trampoline call...
    // function in r7, args in r0, r1, r2, and r3, end in lr?
    rc = reg_write(7, addr);
    if (rc != SWD_OK) return rc;
    for (int i=0; i < argc; i++) {
        rc = reg_write(i, args[i]);
        if (rc != SWD_OK) return rc;
    }
    

    // Now set the PC to go to our address
    rc = reg_write(15, trampoline_addr);
    if (rc != SWD_OK) return rc;

    // Put the end address in LR
    rc = reg_write(14, trampoline_end);
    if (rc != SWD_OK) return rc;

    // Set the stack pointer to something sensible... (MSP)
    rc = reg_write(17, 0x20040800);
    if (rc != SWD_OK) return rc;

    // Set xPSR for the thumb thingy...
    rc = reg_write(16, (1 << 24));

    rc = reg_read(0, &r0);
    if (rc != SWD_OK) return rc;


    rc = core_is_halted();
    if (rc == -1) panic("aaarg!");
    if (!rc) panic("core not halted");

    // Now can we continue and just wait for a halt?
//    core_unhalt();
    core_unhalt_with_masked_ints();
    while(1) {
        busy_wait_ms(2);
        rc = core_is_halted();
        if (rc == -1) panic("here");
        if (rc) break;
    }


/*
    // Bloody hell if we get here!
    uint32_t regs[5];

    for (int i=0; i < 5; i++) {
        rc = reg_read(i, &regs[i]);
        if (rc != SWD_OK) return rc;
    }

    // What is our pc
    uint32_t pc;
    rc = reg_read(15, &pc);
    if (rc != SWD_OK) return rc;
    */
   return SWD_OK;
}


 __attribute__((noinline, section("mysec"))) int remote_func (int x) {
    return x + 2;
}

int swd_test() {
    extern char __start_mysec[];
    extern char __stop_mysec[];

    int length = (__stop_mysec - __start_mysec);
    int rc;

    rc = mem_write_block(0x20001000, length, (uint8_t *)__start_mysec);
    if (rc != SWD_OK) panic("fail");

    uint32_t args[1] = { 0x00100020 };

    rc = rp2040_call_function(0x20001000, args, 1);
    if (rc != SWD_OK) panic("fail");
    return 0;
}


