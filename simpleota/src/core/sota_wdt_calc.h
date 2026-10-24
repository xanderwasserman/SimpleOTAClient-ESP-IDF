/**
 * @file
 * @brief Deadline maths for the chip watchdog that resets a hung
 * trial.
 *
 * During a SimpleOTA trial the RTC (or LP) watchdog runs on the chip's slow
 * clock, so it still fires when a hang stops the confirm timer. It resets
 * the chip, and the bootloader then returns to the previous image, or,
 * without bootloader rollback, the engine rolls back the trial that booted
 * after a watchdog. It waits
 * for the confirm timeout plus a margin: the larger of a floor and 10
 * percent of the confirm timeout.
 *
 * The floor covers one request, so the confirm timeout's own rollback, which
 * may be waiting behind that request, still runs first and reports
 * confirm_timeout.
 *
 * No platform types, so the host tests can include this header. The
 * hardware calls belong to the platform port.
 */

#ifndef SOTA_WDT_CALC_H
#define SOTA_WDT_CALC_H

#include <stdbool.h>
#include <stdint.h>

/** Smallest extra seconds added to the confirm timeout. */
#define SOTA_TRIAL_WDT_MARGIN_S 60u

/**
 * Margin floor for a given HTTP timeout: SOTA_TRIAL_WDT_MARGIN_S, or more
 * when one request can take longer. A request waits up to the timeout to
 * connect, send, receive the headers and read the body, so four timeouts,
 * plus 10 s.
 */
static inline uint32_t sota_wdt_floor_sec(uint32_t http_timeout_ms)
{
    uint64_t request_s = ((uint64_t)http_timeout_ms * 4u + 999u) / 1000u + 10u;
    if (request_s < SOTA_TRIAL_WDT_MARGIN_S) return SOTA_TRIAL_WDT_MARGIN_S;
    return request_s > UINT32_MAX ? UINT32_MAX : (uint32_t)request_s;
}

/** Extra seconds on top of the confirm timeout. */
static inline uint32_t sota_wdt_margin_sec(uint32_t confirm_sec, uint32_t floor_sec)
{
    uint32_t tenth = confirm_sec / 10u;
    return tenth > floor_sec ? tenth : floor_sec;
}

/**
 * Stage-0 value, in slow-clock ticks, for the confirm timeout plus its
 * margin. cal_q13_19 is the cached slow-clock calibration (microseconds per
 * tick, Q13.19); when it is 0, nominal_hz is used. The stage register is
 * 32 bits: *clamped is set when the deadline does not fit, and the value
 * saturates (about 8 hours at the usual internal slow clock, about 13 on
 * the S2).
 */
static inline uint32_t sota_wdt_stage_ticks(uint32_t confirm_sec, uint32_t floor_sec,
                                            uint32_t cal_q13_19, uint32_t nominal_hz,
                                            bool *clamped)
{
    uint64_t seconds = (uint64_t)confirm_sec + sota_wdt_margin_sec(confirm_sec, floor_sec);
    uint64_t ticks;
    if (cal_q13_19 != 0) {
        uint64_t us = seconds * 1000000u; /* fits: seconds < 2^33 */
        ticks = us > (UINT64_MAX >> 19) ? UINT64_MAX : (us << 19) / cal_q13_19;
    } else {
        ticks = seconds * (uint64_t)(nominal_hz ? nominal_hz : 1u);
    }
    *clamped = ticks > UINT32_MAX;
    if (*clamped) return UINT32_MAX;
    return ticks < 2 ? 2u : (uint32_t)ticks;
}

#endif /* SOTA_WDT_CALC_H */
