/* time.h - the kernel clock (M23).
 *
 * The wall clock is seeded once at boot from the CMOS RTC
 * (assumed UTC, as QEMU presents it) and advanced by the
 * 100 Hz PIT tick count. The monotonic clock is the tick
 * count itself. No drift correction and no timezone: a
 * server-first kernel needs honest timestamps more than
 * it needs NTP. */
#ifndef YOUSJ_TIME_H
#define YOUSJ_TIME_H

#include <yousj/types.h>

/* Read the RTC and seed the wall clock. Call once, early,
 * before the first user task. */
void time_init(void);

/* Seconds since the Unix epoch (UTC). */
uint64_t yousj_now_unix(void);

/* Broken-down wall clock: seconds + nanoseconds. */
void yousj_now_timespec(uint64_t *sec_out, uint64_t *nsec_out);

/* Broken-down monotonic clock (since boot). */
void yousj_mono_timespec(uint64_t *sec_out, uint64_t *nsec_out);

#endif /* YOUSJ_TIME_H */
