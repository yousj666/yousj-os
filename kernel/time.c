/* time.c - CMOS RTC seeding + tick-advanced clocks (M23).
 *
 * RTC access follows the MC146818 contract: index via
 * 0x70 (with NMI left enabled), data via 0x71. Register A
 * bit 7 (UIP) says an update is in flight -- wait it out,
 * then read the full set twice until both reads agree.
 * Register B tells the encoding: 0x04 (DM) set means
 * binary values, clear means BCD; 0x02 (24H) set means
 * 24-hour hours, clear means 12-hour with the PM flag in
 * bit 7 of the hour register.
 */
#include <yousj/time.h>
#include <yousj/io.h>
#include <yousj/sched.h>
#include <drivers/serial.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SEC   0x00
#define REG_MIN   0x02
#define REG_HOUR  0x04
#define REG_DAY   0x07
#define REG_MONTH 0x08
#define REG_YEAR  0x09
#define REG_A     0x0A
#define REG_B     0x0B

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's
 * algorithm, public domain math). */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static uint64_t g_boot_epoch; /* wall seconds at tick ~0 */

void time_init(void)
{
    uint8_t raw[6], raw2[6];
    uint8_t regb;
    int i, tries;
    static const uint8_t regs[6] = {REG_SEC, REG_MIN, REG_HOUR,
                                    REG_DAY, REG_MONTH, REG_YEAR};
    for (tries = 0; tries < 100; tries++) {
        /* wait out an update-in-progress */
        while (cmos_read(REG_A) & 0x80) {
        }
        for (i = 0; i < 6; i++)
            raw[i] = cmos_read(regs[i]);
        for (i = 0; i < 6; i++)
            raw2[i] = cmos_read(regs[i]);
        for (i = 0; i < 6 && raw[i] == raw2[i]; i++) {
        }
        if (i == 6)
            break;
    }
    regb = cmos_read(REG_B);
    {
        int binary = (regb & 0x04) != 0;
        int hour24 = (regb & 0x02) != 0;
        uint32_t sec = raw[0], min = raw[1], hour = raw[2];
        uint32_t day = raw[3], mon = raw[4], year = raw[5];
        int pm = 0;
        if (!hour24 && (hour & 0x80)) {
            pm = 1;
            hour &= 0x7F;
        }
        if (!binary) {
            sec = bcd_to_bin((uint8_t)sec);
            min = bcd_to_bin((uint8_t)min);
            hour = bcd_to_bin((uint8_t)hour);
            day = bcd_to_bin((uint8_t)day);
            mon = bcd_to_bin((uint8_t)mon);
            year = bcd_to_bin((uint8_t)year);
        }
        if (!hour24) {
            if (pm && hour != 12)
                hour += 12;
            if (!pm && hour == 12)
                hour = 0;
        }
        year += 2000; /* RTC year is 00-99 in the 2000s here */
        g_boot_epoch = (uint64_t)(days_from_civil((int64_t)year,
                                                  mon, day) *
                                      86400 +
                                  (int64_t)hour * 3600 +
                                  (int64_t)min * 60 + (int64_t)sec);
    }
    serial_puts("Yousj OS: RTC wall clock seeded, epoch=");
    serial_put_u64(g_boot_epoch);
    serial_puts("\n");
}

uint64_t yousj_now_unix(void)
{
    return g_boot_epoch + sched_uptime_ticks() / 100;
}

void yousj_now_timespec(uint64_t *sec_out, uint64_t *nsec_out)
{
    uint64_t ticks = sched_uptime_ticks();
    *sec_out = g_boot_epoch + ticks / 100;
    *nsec_out = (ticks % 100) * 10000000ULL;
}

void yousj_mono_timespec(uint64_t *sec_out, uint64_t *nsec_out)
{
    uint64_t ticks = sched_uptime_ticks();
    *sec_out = ticks / 100;
    *nsec_out = (ticks % 100) * 10000000ULL;
}
