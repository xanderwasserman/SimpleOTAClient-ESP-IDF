/**
 * sota_runner_esp32.c - sota_runner.h on FreeRTOS tasks and notifications.
 */

#include "sota_runner.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sota_app.h"
#include "sota_sys.h"
#include "sota_time.h"

static void wait_ms(uint64_t ms)
{
    ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks(ms, configTICK_RATE_HZ));
}

static void run(void *arg)
{
    sota_runner_t *r = (sota_runner_t *)arg;
    /* The delay keeps clear of other TLS sessions starting at boot, unless
     * the confirm timeout is already due. */
    if (r->initial_delay_ms && !r->engine->timeout_pending) wait_ms(r->initial_delay_ms);
    while (!r->stop) {
        bool connected = !r->is_connected || r->is_connected(r->ctx);
        uint32_t wait_s = sota_engine_step(r->engine, connected);
        if (r->stop) break;
        wait_ms((uint64_t)wait_s * 1000u);
    }
    /* r may be freed once task is NULL, so nothing touches it after. */
    r->task = NULL;
    vTaskDelete(NULL);
}

bool sota_runner_start(sota_runner_t *r, const char *name)
{
    if (r->task) return true;
    r->stop = false;
    /* The handle is written before the task can run. */
    return xTaskCreate(run, name, r->stack_size, r, r->priority, (TaskHandle_t *)&r->task) ==
           pdPASS;
}

bool sota_runner_stop(sota_runner_t *r, uint32_t timeout_ms)
{
    TaskHandle_t task = (TaskHandle_t)r->task;
    if (!task) return true;
    r->stop = true;
    if (xTaskGetCurrentTaskHandle() == task) return true;
    xTaskNotifyGive(task);
    for (uint32_t waited = 0; r->task; waited += 10) {
        if (waited >= timeout_ms) return false;
        sota_sys_sleep_ms(10);
    }
    return true;
}

static void service(void *arg)
{
    sota_engine_service((sota_engine_t *)arg);
    vTaskDelete(NULL);
}

void sota_runner_wake(sota_runner_t *r)
{
    TaskHandle_t task = (TaskHandle_t)r->task;
    if (task) {
        xTaskNotifyGive(task);
        return;
    }
    if (xTaskCreate(service, "sota_rollback", r->stack_size, r->engine, r->priority, NULL) !=
        pdPASS) {
        SOTA_LOGE("no memory for the rollback task; restarting, which rolls back");
        sota_sys_restart();
    }
}

bool sota_runner_on_task(const sota_runner_t *r)
{
    return r->task && xTaskGetCurrentTaskHandle() == (TaskHandle_t)r->task;
}
