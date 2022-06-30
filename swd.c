#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#pragma GCC optimize ("O0")


#include "hardware/pio.h"
#include "swd.pio.h"

#include "swd.h"

#define SWDCLK              26
#define SWDIO               22

#define OUT                 1
#define IN                  0

static PIO                  swd_pio = pio0;
static uint                 swd_sm;


//
// Debug Port Register Addresses
//
#define DP_IDCODE                       0x00U   // IDCODE Register (RD)
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


/**
 * @brief Return parity for a given 4 bit number
 * 
 * @param value 
 * @return int 
 */
static int parity4(uint32_t value) {
    static const uint32_t plook[16] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };
    return plook[value];
}

/**
 * @brief Return parity for a given 32 bit number
 * 
 * @param value 
 * @return int 
 */
static inline int parity32(uint32_t value) {
    uint32_t p16 = ((value & 0xffff0000) >> 16) ^ (value & 0xffff);
    uint32_t p8 = ((p16 & 0xff00) >> 8) ^ (p16 & 0xff);
    uint32_t p4 = ((p8 & 0xf0) >> 4) ^ (p8 & 0x0f);
    return parity4(p4);
}


/**
 * @brief Load the PIO with the SWD code
 * 
 * @param pio 
 * @return int 
 */
static int swd_pio_load(PIO pio) {
    pio_sm_config   c;

    // Load the program at offset zero so the jumps are constant...
    pio_add_program_at_offset(pio, &swd_program, 0);
    c = swd_program_get_default_config(0);      // 0=offset

    // Map the appropriate pins...
    sm_config_set_out_pins(&c, SWDIO, 1);
    sm_config_set_set_pins(&c, SWDIO, 1);
    sm_config_set_in_pins(&c, SWDIO);
    sm_config_set_sideset_pins(&c, SWDCLK);

    // Setup PIO to GPIO
    pio_gpio_init(pio, SWDCLK);
    pio_gpio_init(pio, SWDIO);
    gpio_pull_up(SWDIO);

    // Get a state machine...
    swd_sm = pio_claim_unused_sm(pio, true);

    // Setup direction... we want lsb first (so shift right)
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_in_shift(&c, true, true, 32);

    // Set directions...
    pio_sm_set_consecutive_pindirs(pio, swd_sm, SWDCLK, 1, true);
    pio_sm_set_consecutive_pindirs(pio, swd_sm, SWDIO, 1, false);

    // And initialise
    pio_sm_init(pio, swd_sm, 0, &c);    // 0=offset

    // Go slow for the minute...
    //pio_sm_set_clkdiv_int_frac(pio, swd_sm, 128, 0);
    pio_sm_set_clkdiv_int_frac(pio, swd_sm, 32, 0);

    // 125MHz / 2 / 3 = ~ 20Mhz --> divider of 3
    // 150Mhz / 2 / 3 = 25Mhz --> divider of 3 (and it works!)
}




/**
 * @brief Sends up to 21 bits of data using a single control word
 * 
 * @param count 
 * @param data 
 */
static inline void swd_short_output(int count, uint32_t data) {
    assert(count > 0);
    assert(count <= 21);

    pio_sm_put_blocking(swd_pio, swd_sm, (data << 10) | ((count-1) << 5) | swd_offset_short_output);
}

/**
 * @brief Receive up to 32 bits (a single return value)
 * 
 * Note we can ask for more but will need to read the rest outselves
 * 
 * @param count 
 * @return uint32_t 
 */
static inline uint32_t swd_short_input(int count) {
    assert(count > 0);

    pio_sm_put_blocking(swd_pio, swd_sm, ((count-1) << 5) | swd_offset_input);
    return pio_sm_get_blocking(swd_pio, swd_sm);
}

/**
 * @brief Select a specific target
 * 
 * This is a custom routine because we can't check the ACK bits so we can't use
 * the main conditional function. 
 * 
 * @param target 
 * @return int 
 */
static void swd_targetsel(uint32_t target) {
    uint32_t parity = parity32(target);

    // First we send the 8 bits out real output (inc park)...
    swd_short_output(8, 0b10011001);

    // Now we read 5 bits (trn, ack0, ack1, ack2, trn) ... (will send it back)
    pio_sm_put_blocking(swd_pio, swd_sm, ((5-1) << 5) | swd_offset_input);

    // Now we can write the target id (lsb) and a parity bit
    pio_sm_put_blocking(swd_pio, swd_sm, ((33-1) << 5) | swd_offset_output);
    pio_sm_put_blocking(swd_pio, swd_sm, target);               // lsb first
    pio_sm_put_blocking(swd_pio, swd_sm, parity);

    // Now we can read back the three bits (well 6) that should be waiting for us
    // and discard them
    pio_sm_get_blocking(swd_pio, swd_sm);
}

/**
 * @brief Perform an SWD read operation
 * 
 * We pull out bits A[2:3] from the address field
 * 
 * @param APnDP 
 * @param addr 
 * @param result 
 * @return int 
 */
static int _swd_read(int APnDP, int addr, uint32_t *result) {
    uint32_t ack;
    uint32_t res;
    uint32_t parity, p;

    // We care about 4 bits for parity, 1=RD, APnDP, A3/2
    uint32_t packpar = parity4((addr & 0xc) | 2 | APnDP);

    // First we send the 7 bits out real output... (will send one value back)
    // LSB -> start, (APnDP), 1=RD, A2, A3, parity, stop, 1=park)
    uint32_t data =     (1 << 7)                // park bit
                    |   (0 << 6)                // stop bit
                    |   (packpar << 5)          // parity
                    |   (addr & 0xc) << 1       // A3/A2
                    |   (1 << 2)                // Read
                    |   (APnDP << 1)
                    |   (1);                    // start bit

    swd_short_output(8, data);

    // Now we do a conditional read...
    //
    // 5 bits -- location for conditional
    // 8 bits -- how many bits (value of x) for read
    // 5 bits -- location to jump to if good
    // 14 bits -- locatiom to jump to if we fail
    //
    pio_sm_put_blocking(swd_pio, swd_sm, swd_offset_start << (5+8+5)        // go to start on failure
                            | swd_offset_in_jmp << (5+8)                    // to in_jump on success
                            | (33-1) << 5                                   // 32 + 1 parity
                            | swd_offset_conditional);                      // conditional
    ack = pio_sm_get_blocking(swd_pio, swd_sm) >> 1;

    //
    // We expect an OK response, otherwise decode the error... there will be no
    // further output if we have an error.
    //
    if (ack != 1) {
        // We need a trn if we fail...
        swd_short_output(1, 0);
        if (ack == 2) return SWD_WAIT;
        if (ack == 4) return SWD_FAULT;
        return SWD_ERROR;
    }

    res = pio_sm_get_blocking(swd_pio, swd_sm);
    parity = (pio_sm_get_blocking(swd_pio, swd_sm) & 0x80000000) == 0x80000000;
    if (parity != parity32(res)) return SWD_PARITY;
    *result = res;

    // We always need a trn after a read...
    swd_short_output(1, 0);
    return SWD_OK;    
}
static int swd_read(int APnDP, int addr, uint32_t *result) {
    int rc;

    do {
        rc = _swd_read(APnDP, addr, result);
    } while (rc == SWD_WAIT);
    return rc;
}

/**
 * @brief Perform an SWD write operation
 * 
 * We pull out bits A[2:3] from the address field
 * 
 * @param APnDP 
 * @param addr 
 * @param result 
 * @return int 
 */
static int _swd_write(int APnDP, int addr, uint32_t value) {
    uint32_t ack;

    // We care about 4 bits for parity, 0=WR, APnDP, A3/2
    uint32_t packpar = parity4((addr & 0xc) | 0 | APnDP);
    uint32_t parity = parity32(value);

    // First we send the 7 bits out real output... (will send one value back)
    // LSB -> start, (APnDP), 1=RD, A2, A3, parity, stop, 1=park)
    uint32_t data =     (1 << 7)                // park
                    |   (0 << 6)                // stop bit
                    |   (packpar << 5)          // parity
                    |   ((addr & 0xc) << 1)     // A3/A2
                    |   (0 << 2)                // Write
                    |   (APnDP << 1)
                    |   (1);                    // start bit

    swd_short_output(8, data);

    // Now we do a conditional rite...
    //
    // 5 bits -- location for conditional
    // 8 bits -- how many bits (value of x) for write
    // 5 bits -- location to jump to if good
    // 14 bits -- locatiom to jump to if we fail
    //
    pio_sm_put_blocking(swd_pio, swd_sm, swd_offset_cond_write_fail << (5+8+5)  // for failure
                            | swd_offset_cond_write_ok << (5+8)                 // to in_jump on success
                            | (33-1) << 5                                       // 32 + 1 parity
                            | swd_offset_conditional);                          // conditional
    pio_sm_put_blocking(swd_pio, swd_sm, value);
    pio_sm_put_blocking(swd_pio, swd_sm, parity);

    ack = pio_sm_get_blocking(swd_pio, swd_sm) >> 1;

    //
    // We expect an OK response, otherwise decode the error... the system will throw
    // away our other output if there's an error
    //
    if (ack != 1) {
        // We need a trn if we fail...
        swd_short_output(1, 0);
        if (ack == 2) return SWD_WAIT;
        if (ack == 4) return SWD_FAULT;
        return SWD_ERROR;
    }
    return SWD_OK;    
}
static int swd_write(int APnDP, int addr, uint32_t value) {
    int rc;

    do {
        rc = _swd_write(APnDP, addr, value);
    } while (rc == SWD_WAIT);
    return rc;
}

/**
 * @brief This sends an arbitary number of bits to the target
 * 
 * It sends each 32bit word in turn LSB first, so it's a little
 * counter intuitive, but keeps the code simple for other parts.
 * 
 * @param data 
 * @param bitcount 
 */
void swd_send_bits(uint32_t *data, int bitcount) {
    // Jump to the output function...
    pio_sm_put_blocking(swd_pio, swd_sm, ((bitcount-1) << 5) | swd_offset_output);

    // And write them...
    while (bitcount > 0) {
        pio_sm_put_blocking(swd_pio, swd_sm, *data++);
        bitcount -= 32;
    }
    // Do we need a sacrificial word?
    if (bitcount == 0) pio_sm_put_blocking(swd_pio, swd_sm, 0); 
}



// ----------------------------------------------------------------------------
// Slightly Higher Level Functions
// ----------------------------------------------------------------------------


/**
 * @brief A cached copy of whats in the DP_SELECT register so that we can
 *        be efficient about updating it.
 */
static int dp_select_cache = 0xffffffff;

/**
 * @brief Change the dp bank in SELECT if it needs changing
 * 
 * @param bank 
 * @return int 
 */
static inline int dp_select_bank(int bank) {
    int rc = SWD_OK;

    assert(bank <= 0xf);

    if ((dp_select_cache & 0xf) != bank) {
        dp_select_cache = (dp_select_cache & 0xfffffff0) | bank;
        rc = swd_write(0, DP_SELECT, dp_select_cache);
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

    if ((ap != (dp_select_cache >> 24)) || (bank != (dp_select_cache & 0xf0))) {
        dp_select_cache = (ap << 24) | bank | dp_select_cache & 0xf;
        rc = swd_write(0, DP_SELECT, dp_select_cache);
    }
    return rc;
}

int dp_read(uint32_t addr, uint32_t *res) {
    int rc;

    // First check to see if we are reading something where we might
    // care about the dp_banksel
    if (1 && (addr & 0x0f) == 4) {
        rc = dp_select_bank((addr & 0xf0) >> 4);
        if (rc != SWD_OK) return rc;
    }
    return swd_read(0, addr & 0xf, res);
}

int dp_write(uint32_t addr, uint32_t value) {
    int rc;

    // First check to see if we are writing something where we might
    // care about the dp_banksel
    if (1 && (addr & 0x0f) == 4) {
        rc = dp_select_bank((addr & 0xf0) >> 4);
        if (rc != SWD_OK) return rc;
    }
    return swd_write(0, addr & 0xf, value);
}

// Reset is at least 50 bits of 1...
static const uint32_t reset_seq[] = { 0xffffffff, 0x00ffffff };

// Selection alert sequence... 128bits
static const uint32_t selection_alert_seq[] = { 0x6209F392, 0x86852D95, 0xE3DDAFE9, 0x19BC0EA2 };

// Zero sequence...
static const uint32_t zero_seq[] = { 0x00000000 };
static const uint32_t ones_seq[] = { 0xffffffff };

// Activation sequence (8 bits)
static const uint32_t act_seq[] = { 0x1a };


/**
 * @brief Send the required sequence to reset the line and start SWD ops
 * 
 */
int dp_initialise() {
    uint32_t id;
    int rc;

//    swd_send_bits(reset_seq, 64);   // includes zeros
    swd_send_bits((uint32_t *)ones_seq, 8);
    swd_send_bits((uint32_t *)selection_alert_seq, 128);
    swd_send_bits((uint32_t *)zero_seq, 4);
    swd_send_bits((uint32_t *)act_seq, 8);
    swd_send_bits((uint32_t *)ones_seq, 8);
    swd_send_bits((uint32_t *)reset_seq, 64);   // includes zeros

    // Now a reset sequence
    swd_send_bits((uint32_t *)reset_seq, 64);


    swd_targetsel(0x01002927);
    swd_send_bits((uint32_t *)zero_seq, 20);

    // Now read the status register... this must be next (so swd_read)
    rc = swd_read(0, 0, &id);
    if (rc != SWD_OK) return rc;

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
    int         rc;
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

/**
 * @brief Update the memory csr if we need to
 * 
 * @param value 
 */
static inline int ap_mem_set_csr(uint32_t value) {
    static uint32_t ap_mem_csr_cache = 0xffffffff;
    int rc = SWD_OK;

    if (ap_mem_csr_cache != value) {
        ap_mem_csr_cache = value;
        rc = ap_write(0, 0x00, value);
    }
    return rc;
}

// DBGSWENABLE, AHB_MASTER_DEBUG, HPROT1, no-auto-inc, 32-bit
#define AP_MEM_CSW_SINGLE     (1 << 31) \
                            | (1 << 29) \
                            | (1 << 25) \
                            | (0 << 4)  \
                            | (2 << 0)

// DBGSWENABLE, AHB_MASTER_DEBUG, HPROT1, auto-inc, 32-bit
#define AP_MEM_CSW_INC        (1 << 31) \
                            | (1 << 29) \
                            | (1 << 25) \
                            | (1 << 4) \
                            | (2 << 0)

int mem_read(uint32_t addr, uint32_t *res) {
    int rc;
    // Control/Status word in the mem AP...
    rc = ap_mem_set_csr(AP_MEM_CSW_SINGLE);

    if (rc != SWD_OK) return rc;

    // Set the adress
    rc = ap_write(0, 0x04, addr);
    if (rc != SWD_OK) return rc;

    rc = ap_read(0, 0x0c, res);
    return rc;
}

int mem_read16(uint32_t addr, uint16_t *res) {
    uint32_t v;
    int rc;

    rc = mem_read(addr & 0xfffffffc, &v);
    if (rc != SWD_OK) return rc;

    if (addr & 2) {
        // This will be the high bits?
        *res = (v >> 16);
    } else {
        *res = (v & 0xffff);
    }
    return SWD_OK;
}


int mem_write(uint32_t addr, uint32_t value) {
    int rc;
    // Control/Status word in the mem AP...
    rc = ap_mem_set_csr(AP_MEM_CSW_SINGLE);

    if (rc != SWD_OK) return rc;

    // Set the adress
    rc = ap_write(0, 0x04, addr);
    if (rc != SWD_OK) return rc;

    rc = ap_write(0, 0x0c, value);
    return rc;
}

int mem_write_block(uint32_t addr, uint32_t count, uint32_t *src) {
    int rc;

    for (int i=0; i < count; i++) {
        rc = mem_write(addr, *src++);
        if (rc != SWD_OK) return rc;
        addr += 4;
    }
    return SWD_OK;

    // Auto increment isues?
    rc = ap_mem_set_csr(AP_MEM_CSW_INC);
    if (rc != SWD_OK) return rc;

    // Set the starting address
    rc = ap_write(0, 0x04, addr);
    if (rc != SWD_OK) return rc;

    while(count--) {
        rc = ap_write(0, 0x0C, *src++);
        if (rc != SWD_OK) return rc;
    }
    return rc;
}



int mem_read_block(uint32_t addr, uint32_t count, uint32_t *dest) {
    int rc;

    rc = ap_mem_set_csr(AP_MEM_CSW_INC);
    if (rc != SWD_OK) return rc;

    // Set the starting address
    rc = ap_write(0, 0x04, addr);
    if (rc != SWD_OK) return rc;

    // Loop round the reads, using the deferred model.. so ignore the first one
    rc = ap_read_defer(0, 0x0C, dest);
    if (rc != SWD_OK) return rc;
    while (--count) {
        rc = ap_read_defer(0, 0x0C, dest++);
        if (rc != SWD_OK) return rc;
    }
    // Now read the last one...
    rc = ap_read_last(dest);
    return rc;
}




volatile int ret;


static volatile uint32_t xx;
static volatile uint32_t id;
//static volatile uint32_t rc;

uint32_t buffer[100];

int swd_init() {
    // Load and start the PIO...
    swd_pio_load(swd_pio);
    pio_sm_set_enabled(swd_pio, swd_sm, true);
    return SWD_OK;
}

int dp_init() {
    if (dp_initialise() != SWD_OK) panic("unable to initialise dp\r\n");
    if (dp_power_on() != SWD_OK) panic("unable to power on dp\r\n");
    return SWD_OK;
}


#define DCB_DHCSR       0xE000EDF0
#define DCB_DCRSR       0xE000EDF4
#define DCB_DCRDR       0xE000EDF8
#define DCB_DEMCR       0xE000EDFC
#define DCB_DSCSR       0xE000EE08

#define NVIC_AIRCR      0xE000ED0C


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

    rc = mem_write(DCB_DCRSR, (0 << 16) | (reg & 0x1f));
    if (rc != SWD_OK) return rc;

    // We are supposed to wait for the reg ready flag ... but it seems we don't
    // need it?
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00010000) break;
    }
    rc = mem_read(DCB_DCRDR, res);
    return rc;
}
int reg_write(int reg, uint32_t value) {
    int rc;

    // Write the data into the RDR
    rc = mem_write(DCB_DCRDR, value);
    if (rc != SWD_OK) return rc;

    // Now write the reg number
    rc = mem_write(DCB_DCRSR, (1 << 16) | (reg & 0x1f));
    if (rc != SWD_OK) return rc;

    // Now wait until it's done
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00010000) break;
    }
    return SWD_OK;
}

int core_enable_debug() {
    return mem_write(DCB_DHCSR, (0xA05F << 16) | 1);
}

int core_halt() {
    int rc;
    uint32_t value;

    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (1<<1) | 1);
//    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1<<1) | 1);
    if (rc != SWD_OK) return rc;
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00020000) break;
    }
    return SWD_OK;
}

int core_unhalt() {
    int rc;

    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1 <<3) | (0<<1) | 1);
//    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;
    return SWD_OK;
    // TODO: more?
}

int core_unhalt_with_masked_ints() {
    int rc;

    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;
    return SWD_OK;
}

int core_step() {
    int rc;
    uint32_t value;

    // step and !halt...
    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1<<3) | (1<<2) | (0<<1) | 1);
//    rc = mem_write(DCB_DHCSR, (0xA05F << 16) | (1<<2) | (0<<1) | 1);
    if (rc != SWD_OK) return rc;

    // Now wait for a halt again...
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & 0x00020000) break;
    }
    return SWD_OK;
}

int core_is_halted() {
    int rc;
    uint32_t value;

    rc = mem_read(DCB_DHCSR, &value);

    if (rc != SWD_OK) {
        panic("HERE");
    }
    if (rc != SWD_OK) return -1;
    if (value & 0x00020000) return 1;
    return 0;
}

/**
 * @brief Reset the core but stop it from executing any instructions
 * 
 * @return int 
 */
int core_reset_halt() {
    int rc;
    uint32_t value;

    // First halt the core...
    core_halt();

    // Now set the DWTENA and VC_CORERESET bits...
    rc = mem_write(DCB_DEMCR, (1<<24) | (1 << 0));
    if (rc != SWD_OK) return rc;

    // Now reset the core (will be caught by the above)
    rc = mem_write(NVIC_AIRCR, (0x05FA << 16) | (1 << 2));

    // Now make sure we get a reset flag....
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (value & (1<<25)) break;
    }

    // Then make sure it clears...
    while (1) {
        rc = mem_read(DCB_DHCSR, &value);
        if (rc != SWD_OK) return rc;
        if (!(value & (1<<25))) break;
    }

    // Now clear the CORERESET bit...
    rc = mem_write(DCB_DEMCR, (1<<24) | (0 << 0));
    return rc;
}

#define BPCR        0xE0002000
static uint32_t breakpoints[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
static const uint32_t bp_reg[4] = { 0xE0002008, 0xE000200C, 0xE0002010, 0xE0002014 };

int bp_find(uint32_t addr) {
    for (int i=0; i < 4; i++) {
        if (breakpoints[i] == addr) return i;
    }
    return -1;
}

int bp_set(uint32_t addr) {
    int rc;
    int bp = bp_find(addr);
    if (bp != -1) return SWD_OK;        // already have it
    bp = bp_find(0xffffffff);
    if (bp == -1) return SWD_ERROR;     // no slots available

    // Set the breakpoint...
    breakpoints[bp] = addr;
    rc = mem_write(bp_reg[bp], 0xC0000000 | (addr & 0xfffffffc) | (1));
    if (rc != SWD_OK) return rc;

    // Turn on the breakpoint system...
    rc = mem_write(BPCR, (1<<1) | 1);
    return rc;
}

int bp_clr(uint32_t addr) {
    int rc;

    int bp = bp_find(addr);
    if (bp == -1) return SWD_OK;        // we don't have it? Error?
    breakpoints[bp] = 0xffffffff;
    rc = mem_write(bp_reg[bp], 0);      // fully disabled
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

    rc = mem_read(BOOTROM_MAGIC_ADDR, &magic);
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
    core_unhalt();
//    core_unhalt_with_masked_ints();
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

    rc = mem_write_block(0x20001000, length, (uint32_t *)__start_mysec);
    if (rc != SWD_OK) panic("fail");

    uint32_t args[1] = { 0x00100020 };

    rc = rp2040_call_function(0x20001000, args, 1);
    if (rc != SWD_OK) panic("fail");
    return 0;
}


int Xswd_test() {
    while (1) {
        sleep_ms(200);

        core_enable_debug();
        core_halt();

        // Try to read r0

        int i;
        int rc;

        rc = reg_read(0b01111, &id);        // debug return address
        rc = reg_read(0b10001, &id);        // main stack pointer
        rc = reg_read(0b10010, &id);        // process stack pointer
        rc = reg_read(0b10000, &id);        // xPSR
        rc = reg_read(0, &id);
        rc = reg_read(1, &id);
        rc = reg_read(2, &id);


        rc = mem_write(DCB_DCRSR, (0 << 16) | (0));
        if (rc != SWD_OK) panic("fail");
       for (i=0; i < 10; i++) {
            uint32_t v;

            rc = mem_read(DCB_DHCSR, &v);
            if (rc != SWD_OK) panic("fail");
            if (v & 0x00010000) break;
        }
        id = i;
        rc = mem_read(DCB_DCRDR, &id);
        if (rc != SWD_OK) panic("fail");



        rc = ap_read(0, 0xFC, &id);
     //   continue;

        rc = mem_read_block(0x00000000, 50, buffer);

        //rc = mem_read(0xd0000000, &id);
        rc = mem_read(0xe000ed00, &id);

        rc = mem_read(0x00000000, &id);
        rc = mem_read(0x10000000, &id);
        // CHeck sticky
        rc = swd_read(0, 0x4, &id);

                // Now see if it's in the read buf
        rc = swd_read(0, 0xc, &id);


        // This should be a SELECT of AP ID 0, at bank F0
        rc = swd_write(0, 0x8, (0x00 << 24) | (0xf << 4));


        // This should read the ID
        rc = swd_read(1, 0xc, &id);

        // CHeck sticky
        rc = swd_read(0, 0x4, &id);


        // Now see if it's in the read buf
        rc = swd_read(0, 0xc, &id);

        rc = swd_read(0, 0x4, &id);

        // Now try reading the ID
//        rc = swd_read(1, 0xc, &id);

        xx = id;
    }


    while(1);

}
