/**
 * @file
 * @brief The managed task, implemented by the port (FreeRTOS on
 * ESP32): it checks in on the engine's schedule, applies what is offered,
 * and runs the confirm timeout's rollback. simpleota_start() runs it.
 */

#ifndef SOTA_RUNNER_H
#define SOTA_RUNNER_H

#include <stdbool.h>
#include <stdint.h>

#include "sota_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The managed task's settings and state. Fill the public fields, zero the
 *  private ones, then call sota_runner_start(). */
typedef struct {
    sota_engine_t *engine;          /**< The engine it drives. */
    uint32_t initial_delay_ms;      /**< Before the first check, unless a confirm timeout is due. */
    bool (*is_connected)(void *ctx); /**< NULL: always connected. */
    void *ctx;                      /**< Passed to is_connected. */
    uint32_t stack_size;            /**< Also for the rollback task without a runner. */
    unsigned priority;              /**< The task's priority. */
    void *volatile task;            /**< Private: the running task, or NULL. */
    volatile bool stop;             /**< Private: asked to stop. */
} sota_runner_t;

/** Start the task, if it is not running. False when it could not start. */
bool sota_runner_start(sota_runner_t *r, const char *name);

/**
 * Ask the task to stop after its current step, so a download or flash
 * write is never cut short, and wait up to timeout_ms for it. From the
 * task itself (a callback), returns true at once and the task stops after
 * the callback. False when it is still running at the timeout.
 */
bool sota_runner_stop(sota_runner_t *r, uint32_t timeout_ms);

/**
 * For sota_app_wake(): wake the task to check now or handle the confirm
 * timeout. Without a running task, the timeout's rollback runs on a
 * short-lived task, since it writes storage and restarts.
 */
void sota_runner_wake(sota_runner_t *r);

/** The calling code runs on the runner's task. */
bool sota_runner_on_task(const sota_runner_t *r);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_RUNNER_H */
