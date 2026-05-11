/*
 * wspr_transmit.c
 *
 * Command line WSPR RF transmitter for the ELEC3607/ELEC9607 WSPR-SDR board.
 *
 * The program encodes a standard WSPR type-1 message:
 *
 *     ./wspr_transmit VK2AAL QF56 3
 *
 * It then keys Si5351 CLK2 as 4-FSK.  CLK2 is driven from PLLB so the receiver
 * LO outputs on CLK0/CLK1 can keep using their existing PLLA configuration.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef NO_I2C
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
#endif

#define SI5351_ADDR_DEFAULT 0x60
#define I2C_DEVICE_DEFAULT "/dev/i2c-3"

#define WSPR_SYMBOLS 162
#define WSPR_SYMBOL_SECONDS (8192.0 / 12000.0)
#define WSPR_TONE_SPACING_HZ (12000.0 / 8192.0)

#define WSPR_POLY1 0xF2D05351u
#define WSPR_POLY2 0xE4613C47u

#define SI5351_PLLB_BASE 34
#define SI5351_MS2_BASE 58
#define SI5351_CLK2_CTRL 18
#define SI5351_OUTPUT_ENABLE 3
#define SI5351_PLL_RESET 177

/* 40 m WSPR: USB dial 7.038600 MHz plus 1500 Hz audio tone 0. */
#define DEFAULT_TX_TONE0_HZ 7040100.0
#define DEFAULT_XTAL_HZ 27000000.0
#define DEFAULT_PLL_HZ 900000000.0
#define DEFAULT_DENOM 1000000u
#define DEFAULT_SLOT_OFFSET_SECONDS 1.0

static const uint8_t wspr_sync[WSPR_SYMBOLS] = {
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,1,0,1,1,1,1,0,0,0,0,0,
    0,0,1,0,0,1,0,1,0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,0,1,0,
    0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,
    0,0,1,0,0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,0,1,0,0,0,1,1,1,
    0,0,0,0,0,1,0,1,0,0,1,1,0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0
};

typedef struct {
    const char *i2c_device;
    int i2c_addr;
    double tx_tone0_hz;
    double xtal_hz;
    double pll_hz;
    unsigned denom;
    int drive_ma;
    int print_symbols;
    int wait_slot;
    double slot_offset_seconds;
    int dry_run;
    int i2c_probe;
    int verbose;
} config_t;

typedef struct {
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned p1;
    unsigned p2;
    unsigned p3;
    double actual_ratio;
} synth_params_t;

#ifndef NO_I2C
static int i2c_fd = -1;
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] CALLSIGN GRID POWER_DBM\n"
        "\n"
        "Example:\n"
        "  %s VK2AAL QF56 3\n"
        "\n"
        "Options:\n"
        "  -f HZ       RF frequency for WSPR tone 0 [7040100.0]\n"
        "  -d DEVICE   I2C device path [/dev/i2c-3]\n"
        "  -a ADDR     Si5351 I2C address in hex/decimal [0x60]\n"
        "  -x HZ       Si5351 crystal frequency [27000000.0]\n"
        "  -p HZ       PLLB frequency [900000000.0]\n"
        "  -D MA       CLK2 drive current: 2, 4, 6 or 8 mA [2]\n"
        "  -w          wait until the next even UTC minute plus slot offset before TX\n"
        "  -o SEC      slot offset used with -w [1.0]\n"
        "  -s          print the 162 WSPR symbols\n"
        "  -n          dry run: encode and time, but do not touch I2C\n"
        "  -P          probe Si5351 over I2C, then exit\n"
        "  -v          verbose register/frequency messages\n"
        "  -h          show this help\n",
        prog, prog);
}

static int parse_int_auto(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < 0 || v > 0x7fffffff) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_double_arg(const char *s, double *out)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return 0;
    }
    *out = v;
    return 1;
}

static void uppercase_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < dst_size && src[i] != '\0'; ++i) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int callsign_code(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    if (ch == ' ') return 36;
    return -1;
}

static int validate_callsign(const char *call)
{
    size_t len = strlen(call);
    size_t i;
    int digit_pos;

    if (len < 3 || len > 6) {
        fprintf(stderr, "Callsign must be 3 to 6 characters for WSPR type-1.\n");
        return 0;
    }
    for (i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)call[i]) && !isupper((unsigned char)call[i])) {
            fprintf(stderr, "Callsign contains an unsupported character: %c\n", call[i]);
            return 0;
        }
    }
    digit_pos = isdigit((unsigned char)call[1]) ? 1 :
                (isdigit((unsigned char)call[2]) ? 2 : -1);
    if (digit_pos < 0) {
        fprintf(stderr, "Callsign must have a digit in position 2 or 3, e.g. VK2ABC.\n");
        return 0;
    }
    if (digit_pos == 1 && len > 5) {
        fprintf(stderr, "A one-letter prefix callsign is too long for WSPR type-1.\n");
        return 0;
    }
    for (i = 0; i < len; ++i) {
        if ((int)i == digit_pos) {
            continue;
        }
        if (!isupper((unsigned char)call[i])) {
            fprintf(stderr, "Only the prefix digit may be numeric in this type-1 encoder.\n");
            return 0;
        }
    }
    return 1;
}

static int validate_grid(const char *grid)
{
    if (strlen(grid) != 4) {
        fprintf(stderr, "Grid locator must be exactly 4 characters, e.g. QF56.\n");
        return 0;
    }
    if (grid[0] < 'A' || grid[0] > 'R' || grid[1] < 'A' || grid[1] > 'R' ||
        !isdigit((unsigned char)grid[2]) || !isdigit((unsigned char)grid[3])) {
        fprintf(stderr, "Grid must be two letters A-R followed by two digits.\n");
        return 0;
    }
    return 1;
}

static int validate_power(int pwr)
{
    int last_digit = pwr % 10;
    if (pwr < 0 || pwr > 60) {
        fprintf(stderr, "Power must be 0..60 dBm.\n");
        return 0;
    }
    if (!(last_digit == 0 || last_digit == 3 || last_digit == 7)) {
        fprintf(stderr,
            "Warning: %d dBm is not a standard WSPR reported power level.\n", pwr);
    }
    return 1;
}

static uint32_t encode_callsign(const char *call)
{
    char c6[7] = "      ";
    size_t len = strlen(call);
    int code[6];
    int i;
    uint32_t n;

    if (isdigit((unsigned char)call[1])) {
        c6[0] = ' ';
        memcpy(&c6[1], call, len < 5 ? len : 5);
    } else {
        memcpy(c6, call, len < 6 ? len : 6);
    }
    c6[6] = '\0';

    for (i = 0; i < 6; ++i) {
        code[i] = callsign_code(c6[i]);
    }

    n = (uint32_t)code[0];
    n = n * 36u + (uint32_t)code[1];
    n = n * 10u + (uint32_t)code[2];
    n = n * 27u + (uint32_t)(code[3] - 10);
    n = n * 27u + (uint32_t)(code[4] - 10);
    n = n * 27u + (uint32_t)(code[5] - 10);
    return n;
}

static uint32_t encode_grid_power(const char *grid, int power)
{
    uint32_t m;
    m = (uint32_t)((179 - 10 * (grid[0] - 'A') - (grid[2] - '0')) * 180);
    m += (uint32_t)(10 * (grid[1] - 'A') + (grid[3] - '0'));
    return m * 128u + (uint32_t)power + 64u;
}

static int parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (int)(x & 1u);
}

static uint8_t bit_reverse8(uint8_t x)
{
    x = (uint8_t)(((x & 0xF0u) >> 4) | ((x & 0x0Fu) << 4));
    x = (uint8_t)(((x & 0xCCu) >> 2) | ((x & 0x33u) << 2));
    x = (uint8_t)(((x & 0xAAu) >> 1) | ((x & 0x55u) << 1));
    return x;
}

static void encode_wspr_symbols(const char *call, const char *grid, int power,
                                uint8_t symbols[WSPR_SYMBOLS])
{
    uint32_t n = encode_callsign(call);
    uint32_t m = encode_grid_power(grid, power);
    uint64_t packed = ((uint64_t)n << 22) | (uint64_t)m;
    uint8_t source_bits[81];
    uint8_t coded[WSPR_SYMBOLS];
    uint8_t interleaved[WSPR_SYMBOLS];
    uint32_t shift = 0;
    int i, p = 0;

    for (i = 0; i < 50; ++i) {
        source_bits[i] = (uint8_t)((packed >> (49 - i)) & 1u);
    }
    for (i = 50; i < 81; ++i) {
        source_bits[i] = 0;
    }

    for (i = 0; i < 81; ++i) {
        shift = (shift << 1) | source_bits[i];
        coded[2 * i] = (uint8_t)parity32(shift & WSPR_POLY1);
        coded[2 * i + 1] = (uint8_t)parity32(shift & WSPR_POLY2);
    }

    memset(interleaved, 0, sizeof(interleaved));
    for (i = 0; i < 256 && p < WSPR_SYMBOLS; ++i) {
        uint8_t j = bit_reverse8((uint8_t)i);
        if (j < WSPR_SYMBOLS) {
            interleaved[j] = coded[p++];
        }
    }

    for (i = 0; i < WSPR_SYMBOLS; ++i) {
        symbols[i] = (uint8_t)(wspr_sync[i] + 2u * interleaved[i]);
    }
}

static int drive_bits_from_ma(int ma)
{
    switch (ma) {
    case 2: return 0;
    case 4: return 1;
    case 6: return 2;
    case 8: return 3;
    default:
        fprintf(stderr, "CLK drive must be 2, 4, 6 or 8 mA.\n");
        return -1;
    }
}

static synth_params_t make_synth_params(double ratio, unsigned denom)
{
    synth_params_t p;
    double frac;
    unsigned floor128;

    p.a = (unsigned)floor(ratio);
    frac = ratio - (double)p.a;
    p.b = (unsigned)llround(frac * (double)denom);
    p.c = denom;
    if (p.b >= p.c) {
        p.a += 1;
        p.b = 0;
    }

    floor128 = (unsigned)((128ull * p.b) / p.c);
    p.p1 = 128u * p.a + floor128 - 512u;
    p.p2 = 128u * p.b - p.c * floor128;
    p.p3 = p.c;
    p.actual_ratio = (double)p.a + (double)p.b / (double)p.c;
    return p;
}

static int si5351_write(uint8_t reg, uint8_t value)
{
#ifdef NO_I2C
    (void)reg;
    (void)value;
    return 0;
#else
    if (i2c_smbus_write_byte_data(i2c_fd, reg, value) < 0) {
        fprintf(stderr, "I2C write failed at reg 0x%02x: %s\n", reg, strerror(errno));
        return -1;
    }
    return 0;
#endif
}

static int si5351_read(uint8_t reg, uint8_t *value)
{
#ifdef NO_I2C
    *value = 0xffu;
    (void)reg;
    return 0;
#else
    int res = i2c_smbus_read_byte_data(i2c_fd, reg);
    if (res < 0) {
        fprintf(stderr, "I2C read failed at reg 0x%02x: %s\n", reg, strerror(errno));
        return -1;
    }
    *value = (uint8_t)res;
    return 0;
#endif
}

static int si5351_write_synth(int base_reg, const synth_params_t *p)
{
    uint8_t r[8];
    r[0] = (uint8_t)((p->p3 >> 8) & 0xffu);
    r[1] = (uint8_t)(p->p3 & 0xffu);
    r[2] = (uint8_t)((p->p1 >> 16) & 0x03u);
    r[3] = (uint8_t)((p->p1 >> 8) & 0xffu);
    r[4] = (uint8_t)(p->p1 & 0xffu);
    r[5] = (uint8_t)(((p->p3 >> 16) & 0x0fu) << 4 |
                     ((p->p2 >> 16) & 0x0fu));
    r[6] = (uint8_t)((p->p2 >> 8) & 0xffu);
    r[7] = (uint8_t)(p->p2 & 0xffu);

    for (int i = 0; i < 8; ++i) {
        if (si5351_write((uint8_t)(base_reg + i), r[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

static int si5351_open(const config_t *cfg)
{
#ifdef NO_I2C
    (void)cfg;
    return 0;
#else
    i2c_fd = open(cfg->i2c_device, O_RDWR);
    if (i2c_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", cfg->i2c_device, strerror(errno));
        return -1;
    }
    if (ioctl(i2c_fd, I2C_SLAVE, cfg->i2c_addr) < 0) {
        fprintf(stderr, "Cannot select Si5351 address 0x%02x: %s\n",
                cfg->i2c_addr, strerror(errno));
        return -1;
    }
    return 0;
#endif
}

static void si5351_close(void)
{
#ifndef NO_I2C
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
#endif
}

static int si5351_probe(const config_t *cfg)
{
    const uint8_t regs[] = {0x00, SI5351_OUTPUT_ENABLE, SI5351_CLK2_CTRL, SI5351_PLL_RESET};

    if (si5351_open(cfg) < 0) {
        return -1;
    }

    printf("I2C device: %s, Si5351 address: 0x%02x\n",
           cfg->i2c_device, cfg->i2c_addr);
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        uint8_t value;
        if (si5351_read(regs[i], &value) < 0) {
            si5351_close();
            return -1;
        }
        printf("r dev(0x%x) reg(0x%x)=0x%x (decimal %u)\n",
               cfg->i2c_addr, regs[i], value, value);
    }

    si5351_close();
    return 0;
}

static int si5351_enable_clk2(int enable)
{
    uint8_t reg3;
    if (si5351_read(SI5351_OUTPUT_ENABLE, &reg3) < 0) {
        return -1;
    }
    if (enable) {
        reg3 = (uint8_t)(reg3 & ~(1u << 2));
    } else {
        reg3 = (uint8_t)(reg3 | (1u << 2));
    }
    return si5351_write(SI5351_OUTPUT_ENABLE, reg3);
}

static int si5351_init_clk2(const config_t *cfg, double *actual_pll_hz)
{
    int drive_bits = drive_bits_from_ma(cfg->drive_ma);
    synth_params_t pllb;

    if (drive_bits < 0) {
        return -1;
    }

    if (si5351_open(cfg) < 0) {
        return -1;
    }

    pllb = make_synth_params(cfg->pll_hz / cfg->xtal_hz, cfg->denom);
    *actual_pll_hz = cfg->xtal_hz * pllb.actual_ratio;

    if (cfg->verbose) {
        fprintf(stderr,
            "PLLB target %.3f Hz, actual %.3f Hz, mult %u + %u/%u\n",
            cfg->pll_hz, *actual_pll_hz, pllb.a, pllb.b, pllb.c);
    }

    if (si5351_enable_clk2(0) < 0) return -1;
    if (si5351_write(SI5351_CLK2_CTRL, 0x80u) < 0) return -1;
    if (si5351_write_synth(SI5351_PLLB_BASE, &pllb) < 0) return -1;

    /* Reset PLLB only. */
    if (si5351_write(SI5351_PLL_RESET, 0x20u) < 0) return -1;

    /*
     * CLK2 control: powered, multisynth clock source, PLLB as MS2 source,
     * non-inverted, selected drive current.
     */
    if (si5351_write(SI5351_CLK2_CTRL, (uint8_t)(0x0cu | 0x20u | drive_bits)) < 0) {
        return -1;
    }
    return 0;
}

static int si5351_set_clk2_frequency(double pll_hz, double out_hz,
                                     unsigned denom, int verbose)
{
    synth_params_t ms2 = make_synth_params(pll_hz / out_hz, denom);
    if (verbose) {
        double actual = pll_hz / ms2.actual_ratio;
        fprintf(stderr, "CLK2 %.6f Hz, actual %.6f Hz, div %u + %u/%u\n",
                out_hz, actual, ms2.a, ms2.b, ms2.c);
    }
    return si5351_write_synth(SI5351_MS2_BASE, &ms2);
}

static void add_seconds(struct timespec *ts, double seconds)
{
    time_t whole = (time_t)floor(seconds);
    long nsec = (long)llround((seconds - (double)whole) * 1000000000.0);
    ts->tv_sec += whole;
    ts->tv_nsec += nsec;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int sleep_until_monotonic(const struct timespec *target)
{
    int rc;
    while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, target, NULL)) != 0) {
        if (rc != EINTR) {
            errno = rc;
            perror("clock_nanosleep");
            return -1;
        }
    }
    return 0;
}

static int wait_next_even_utc_minute(double slot_offset_seconds)
{
    time_t now = time(NULL);
    time_t target = ((now / 120) + 1) * 120;
    struct timespec ts;
    double offset_whole;
    double offset_frac = modf(slot_offset_seconds, &offset_whole);
    int rc;

    target += (time_t)offset_whole;
    if (target - now < 2) {
        target += 120;
    }
    ts.tv_sec = target;
    ts.tv_nsec = (long)llround(offset_frac * 1000000000.0);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    fprintf(stderr, "Waiting for WSPR slot at UTC %ld + %.3f s...\n",
            (long)(target - (time_t)offset_whole), slot_offset_seconds);
    while ((rc = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL)) != 0) {
        if (rc != EINTR) {
            errno = rc;
            perror("clock_nanosleep");
            return -1;
        }
    }
    return 0;
}

static int transmit_symbols(const config_t *cfg, const uint8_t symbols[WSPR_SYMBOLS])
{
    double actual_pll_hz = cfg->pll_hz;
    struct timespec next;

    if (!cfg->dry_run) {
        if (si5351_init_clk2(cfg, &actual_pll_hz) < 0) {
            return -1;
        }
    }

    if (!cfg->dry_run && cfg->wait_slot &&
        wait_next_even_utc_minute(cfg->slot_offset_seconds) < 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int i = 0; i < WSPR_SYMBOLS; ++i) {
        double freq = cfg->tx_tone0_hz + (double)symbols[i] * WSPR_TONE_SPACING_HZ;
        if (!cfg->dry_run) {
            if (si5351_set_clk2_frequency(actual_pll_hz, freq, cfg->denom,
                                          cfg->verbose) < 0) {
                return -1;
            }
            if (i == 0 && si5351_enable_clk2(1) < 0) {
                return -1;
            }
        } else if (cfg->verbose) {
            fprintf(stderr, "symbol %3d tone %u freq %.6f Hz\n", i, symbols[i], freq);
        }

        if (!cfg->dry_run) {
            add_seconds(&next, WSPR_SYMBOL_SECONDS);
            if (sleep_until_monotonic(&next) < 0) {
                return -1;
            }
        }
    }

    if (!cfg->dry_run) {
        if (si5351_enable_clk2(0) < 0) return -1;
        if (si5351_write(SI5351_CLK2_CTRL, 0x80u) < 0) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    config_t cfg;
    char call[16], grid[8];
    int power = 0;
    int argi = 1;
    uint8_t symbols[WSPR_SYMBOLS];

    cfg.i2c_device = I2C_DEVICE_DEFAULT;
    cfg.i2c_addr = SI5351_ADDR_DEFAULT;
    cfg.tx_tone0_hz = DEFAULT_TX_TONE0_HZ;
    cfg.xtal_hz = DEFAULT_XTAL_HZ;
    cfg.pll_hz = DEFAULT_PLL_HZ;
    cfg.denom = DEFAULT_DENOM;
    cfg.drive_ma = 2;
    cfg.print_symbols = 0;
    cfg.wait_slot = 0;
    cfg.slot_offset_seconds = DEFAULT_SLOT_OFFSET_SECONDS;
    cfg.dry_run = 0;
    cfg.i2c_probe = 0;
    cfg.verbose = 0;

    while (argi < argc && argv[argi][0] == '-') {
        const char *opt = argv[argi++];
        if (strcmp(opt, "-h") == 0 || strcmp(opt, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(opt, "-f") == 0 && argi < argc) {
            if (!parse_double_arg(argv[argi++], &cfg.tx_tone0_hz)) {
                fprintf(stderr, "Invalid -f frequency.\n");
                return 2;
            }
        } else if (strcmp(opt, "-d") == 0 && argi < argc) {
            cfg.i2c_device = argv[argi++];
        } else if (strcmp(opt, "-a") == 0 && argi < argc) {
            if (!parse_int_auto(argv[argi++], &cfg.i2c_addr)) {
                fprintf(stderr, "Invalid -a address.\n");
                return 2;
            }
        } else if (strcmp(opt, "-x") == 0 && argi < argc) {
            if (!parse_double_arg(argv[argi++], &cfg.xtal_hz)) {
                fprintf(stderr, "Invalid -x crystal frequency.\n");
                return 2;
            }
        } else if (strcmp(opt, "-p") == 0 && argi < argc) {
            if (!parse_double_arg(argv[argi++], &cfg.pll_hz)) {
                fprintf(stderr, "Invalid -p PLL frequency.\n");
                return 2;
            }
        } else if (strcmp(opt, "-D") == 0 && argi < argc) {
            if (!parse_int_auto(argv[argi++], &cfg.drive_ma)) {
                fprintf(stderr, "Invalid -D drive current.\n");
                return 2;
            }
        } else if (strcmp(opt, "-w") == 0) {
            cfg.wait_slot = 1;
        } else if (strcmp(opt, "-o") == 0 && argi < argc) {
            if (!parse_double_arg(argv[argi++], &cfg.slot_offset_seconds) ||
                cfg.slot_offset_seconds < 0.0 || cfg.slot_offset_seconds > 10.0) {
                fprintf(stderr, "Invalid -o slot offset; use 0..10 seconds.\n");
                return 2;
            }
        } else if (strcmp(opt, "-s") == 0) {
            cfg.print_symbols = 1;
        } else if (strcmp(opt, "-n") == 0) {
            cfg.dry_run = 1;
        } else if (strcmp(opt, "-P") == 0 || strcmp(opt, "--probe") == 0) {
            cfg.i2c_probe = 1;
        } else if (strcmp(opt, "-v") == 0) {
            cfg.verbose = 1;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", opt);
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.i2c_probe) {
        if (argc - argi != 0) {
            usage(argv[0]);
            return 2;
        }
        return si5351_probe(&cfg) < 0 ? 1 : 0;
    }

    if (argc - argi != 3) {
        usage(argv[0]);
        return 2;
    }

    uppercase_copy(call, sizeof(call), argv[argi++]);
    uppercase_copy(grid, sizeof(grid), argv[argi++]);
    if (!parse_int_auto(argv[argi++], &power)) {
        fprintf(stderr, "Invalid power value.\n");
        return 2;
    }

    if (!validate_callsign(call) || !validate_grid(grid) || !validate_power(power)) {
        return 2;
    }
    if (cfg.tx_tone0_hz < 1000000.0 || cfg.tx_tone0_hz > 30000000.0) {
        fprintf(stderr, "Tone-0 RF frequency is outside the supported range.\n");
        return 2;
    }

    encode_wspr_symbols(call, grid, power, symbols);

    printf("WSPR message: %s %s %d\n", call, grid, power);
    printf("CLK2 tone 0: %.6f Hz, spacing %.9f Hz, duration %.3f s\n",
           cfg.tx_tone0_hz, WSPR_TONE_SPACING_HZ,
           WSPR_SYMBOLS * WSPR_SYMBOL_SECONDS);

    if (cfg.print_symbols) {
        printf("Symbols:");
        for (int i = 0; i < WSPR_SYMBOLS; ++i) {
            printf("%c%u", (i % 32 == 0) ? '\n' : ' ', symbols[i]);
        }
        printf("\n");
    }

    if (cfg.dry_run) {
        printf("Dry run enabled: I2C writes are disabled.\n");
    }

    if (transmit_symbols(&cfg, symbols) < 0) {
        return 1;
    }

    printf("Transmission complete.\n");
    return 0;
}
