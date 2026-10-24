/**
 * @file
 * @brief Time, locking, the confirm timer and the chip watchdog.
 * Implemented by the platform port (FreeRTOS, esp_timer and wdt_hal on
 * ESP32) and the host tests' fake.
 */

#ifndef SOTA_SYS_H
#define SOTA_SYS_H

#include <stdbool.h>
#include <stdint.h>

#include "sota_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sota_engine;

/** Block the calling task for ms milliseconds. */
void sota_sys_sleep_ms(uint32_t ms);

/** Milliseconds since boot; wraps. */
uint32_t sota_sys_now_ms(void);

/** One lock for every engine. Never held across a network call. */
/** @{ */
void sota_sys_lock(void);
void sota_sys_unlock(void);
/** @} */

/**
 * Start a one-shot timer that calls sota_engine_timeout_fired(e) after
 * seconds. *timer holds the port's handle. False when it could not start.
 */
bool sota_sys_timer_start(void **timer, uint32_t seconds, struct sota_engine *e);

/** Stop and free the timer, if *timer holds one. */
void sota_sys_timer_stop(void **timer);

/**
 * Arm the chip watchdog for confirm_s plus a margin of at least floor_s
 * (sota_wdt_calc.h). Returns true when the deadline was longer than the
 * watchdog can wait and it was clamped.
 */
bool sota_sys_wdt_arm(uint32_t confirm_s, uint32_t floor_s);

/** Stop the watchdog if sota_sys_wdt_arm() started it. */
void sota_sys_wdt_disarm(void);

/** At boot, before a trial may arm it again: stop a watchdog a reset left
 *  running. */
void sota_sys_wdt_quiesce(void);

/** Restart the chip. */
SOTA_NORETURN void sota_sys_restart(void);

/** Length of sota_sys_default_device_id()'s string, with its NUL. */
#define SOTA_DEVICE_ID_LEN 18

/**
 * The device ID used when the application gives none: the Wi-Fi station
 * MAC as "aa:bb:cc:dd:ee:ff", the same on both clients, so a device keeps
 * its identity when it moves from one to the other.
 */
void sota_sys_default_device_id(char out[SOTA_DEVICE_ID_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_SYS_H */
