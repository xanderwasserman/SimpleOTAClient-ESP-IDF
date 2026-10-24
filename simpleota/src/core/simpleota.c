/**
 * simpleota.c - the public API (simpleota.h) over the engine, and the
 * core's application interface. The ESP-IDF component and the Arduino
 * library both build it; sota_framework.h has what differs between them.
 *
 * Threading model: in managed mode one background task ("simpleota", the
 * core's sota_runner) owns all network and flash activity. Public API calls
 * only notify it, except simpleota_confirm(), which records the confirm on
 * the caller's task, and simpleota_reboot_for_update(), which stops the
 * background task and posts its status from a short-lived task of the same
 * stack size. In polling mode simpleota_check() and simpleota_apply() also
 * run on a short-lived task of that size while the caller waits, because
 * TLS and signature checks need more stack than a typical caller has (the
 * ESP-IDF main task's default is 3584 bytes). Without the managed task, the
 * confirm timeout's rollback runs on a short-lived task.
 */

#include "simpleota.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sota_app.h"
#include "sota_engine.h"
#include "sota_framework.h"
#include "sota_net_esp32.h"
#include "sota_runner.h"
#include "sota_sys.h"
#include "sota_url.h"

_Static_assert((int)SOTA_EVENT_CHECK_DONE == (int)SIMPLEOTA_EVENT_CHECK_DONE &&
                   (int)SOTA_EVENT_UPDATE_AVAILABLE == (int)SIMPLEOTA_EVENT_UPDATE_AVAILABLE &&
                   (int)SOTA_EVENT_DOWNLOAD_PROGRESS == (int)SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS &&
                   (int)SOTA_EVENT_UPDATE_FAILED == (int)SIMPLEOTA_EVENT_UPDATE_FAILED &&
                   (int)SOTA_EVENT_BEFORE_REBOOT == (int)SIMPLEOTA_EVENT_BEFORE_REBOOT &&
                   (int)SOTA_EVENT_TRIAL_BOOT == (int)SIMPLEOTA_EVENT_TRIAL_BOOT &&
                   (int)SOTA_EVENT_CONFIRMED == (int)SIMPLEOTA_EVENT_CONFIRMED &&
                   (int)SOTA_EVENT_ROLLING_BACK == (int)SIMPLEOTA_EVENT_ROLLING_BACK,
               "sota_app.h events match simpleota_event_id_t");
_Static_assert((int)SOTA_APPLY_OK == (int)SIMPLEOTA_OK &&
                   (int)SOTA_APPLY_CHECKSUM_FAIL == (int)SIMPLEOTA_CHECKSUM_FAIL &&
                   (int)SOTA_APPLY_FLASH_FAIL == (int)SIMPLEOTA_FLASH_FAIL &&
                   (int)SOTA_APPLY_NO_OFFER == (int)SIMPLEOTA_NO_OFFER &&
                   (int)SOTA_APPLY_SIGNATURE_FAIL == (int)SIMPLEOTA_SIGNATURE_FAIL &&
                   (int)SOTA_APPLY_REBOOT_PENDING == (int)SIMPLEOTA_REBOOT_PENDING &&
                   (int)SOTA_APPLY_UNCONFIRMED == (int)SIMPLEOTA_UNCONFIRMED &&
                   (int)SOTA_APPLY_DOWNGRADE == (int)SIMPLEOTA_DOWNGRADE,
               "simpleota_result_t mirrors sota_apply_result_t");

static const char *TAG = "simpleota";

/* The config's strings, copied at init: the engine borrows them. */
enum {
    STR_TOKEN,
    STR_BASE_URL,
    STR_DEVICE_ID,
    STR_CHIP_FAMILY,
    STR_BOARD_ID,
    STR_HARDWARE_REVISION,
    STR_PARTITION_PROFILE,
    STR_CHANNEL,
    STR_LABELS_JSON,
    STR_VERSION_LABEL,
    STR_CERT_PEM,
    STR_COUNT,
};

typedef struct {
    sota_net_esp32_t net; /* First: the engine's user pointer is this struct. */
    sota_engine_t engine;
    sota_runner_t runner;
    char *str[STR_COUNT];
    SemaphoreHandle_t polled;   /* Given when a polling call's task is done. */
    TaskHandle_t volatile poll_task; /* That task, while it runs. */
    StaticSemaphore_t polled_buf;
    bool initialized;
    bool manual_confirm;
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;
} simpleota_t;

static simpleota_t s;

/* ---------------------------------------------------------------------------
 * sota_app.h
 * ------------------------------------------------------------------------- */

void sota_app_event(void *user, const sota_event_t *ev)
{
    simpleota_t *c = (simpleota_t *)user;
#if CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK
    if (ev->id == SOTA_EVENT_TRIAL_BOOT) {
        SOTA_LOGW("bootloader rollback is off: this trial is supervised in software");
    }
#endif
    if (!c->event_cb) return;
    simpleota_event_t out = {
        .id = (simpleota_event_id_t)ev->id,
        .reason = ev->reason,
        .build_number = ev->build,
        .bytes_done = ev->done,
        .bytes_total = ev->total,
    };
    c->event_cb(&out, c->user_ctx);
}

void sota_app_wake(void *user)
{
    sota_runner_wake(&((simpleota_t *)user)->runner);
}

/* The core's levels are esp_log_level_t's. */
void sota_app_log(int level, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOG_LEVEL((esp_log_level_t)level, TAG, "%s", line);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static bool copy_strings(const simpleota_config_t *cfg)
{
    char mac[SOTA_DEVICE_ID_LEN];
    if (!cfg->device_id) sota_sys_default_device_id(mac);
    const char *src[STR_COUNT] = {
        [STR_TOKEN] = cfg->token,
        [STR_BASE_URL] = cfg->base_url,
        [STR_DEVICE_ID] = cfg->device_id ? cfg->device_id : mac,
        [STR_CHIP_FAMILY] = cfg->chip_family ? cfg->chip_family : CONFIG_IDF_TARGET,
        [STR_BOARD_ID] = cfg->board_id,
        [STR_HARDWARE_REVISION] = cfg->hardware_revision,
        [STR_PARTITION_PROFILE] = cfg->partition_profile,
        [STR_CHANNEL] = cfg->channel,
        [STR_LABELS_JSON] = cfg->labels_json,
        [STR_VERSION_LABEL] = cfg->version_label,
        [STR_CERT_PEM] = cfg->cert_pem,
    };
    for (int i = 0; i < STR_COUNT; ++i) {
        if (src[i] && !(s.str[i] = strdup(src[i]))) return false;
    }
    return true;
}

/* The platform's name for a mode; NULL leaves it out of check-ins. */
static const char *security_mode_name(simpleota_security_t mode)
{
    switch (mode) {
    case SIMPLEOTA_SECURITY_BASIC: return "basic";
    case SIMPLEOTA_SECURITY_SIGNED: return "signed";
    default: return NULL;
    }
}

static esp_err_t init_engine(const simpleota_config_t *config)
{
    if ((unsigned)config->security_mode > SIMPLEOTA_SECURITY_SIGNED) {
        SOTA_LOGE("security_mode %d is not a simpleota_security_t", (int)config->security_mode);
        return ESP_ERR_INVALID_ARG;
    }
    /* Reporting signed without a key would take signed builds unverified. */
    if (config->security_mode == SIMPLEOTA_SECURITY_SIGNED && !config->num_signing_keys) {
        SOTA_LOGE("security_mode SIGNED needs a signing key");
        return ESP_ERR_INVALID_ARG;
    }
    sota_url_status_t url = SOTA_URL_OK;
    if (config->base_url) url = sota_validate_server_url(config->base_url);
    if (url != SOTA_URL_OK) {
        SOTA_LOGE("base_url rejected (%s): it must be an https:// URL with a host",
                 sota_url_status_name(url));
        return ESP_ERR_INVALID_ARG;
    }
    sota_config_t cfg = {
        .token = s.str[STR_TOKEN],
        .server_url = s.str[STR_BASE_URL],
        .device =
            {
                .device_id = s.str[STR_DEVICE_ID],
                .framework = SOTA_FRAMEWORK_NAME,
                .chip_family = s.str[STR_CHIP_FAMILY],
                .board_id = s.str[STR_BOARD_ID],
                .hardware_revision = s.str[STR_HARDWARE_REVISION],
                .partition_profile = s.str[STR_PARTITION_PROFILE],
                .channel = s.str[STR_CHANNEL],
                .security_mode = security_mode_name(config->security_mode),
                .labels_json = s.str[STR_LABELS_JSON],
                .nvs_schema_version = config->nvs_schema_version ? config->nvs_schema_version : 1,
            },
        .version_label = s.str[STR_VERSION_LABEL],
        .image_build = config->image_build,
        .check_interval_s = config->check_interval_s,
        .confirm_timeout_s = config->confirm_timeout_s,
        .http_timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
        .auto_confirm = false, /* Managed mode only: see simpleota_start(). */
        .auto_reboot = !config->disable_auto_reboot,
        .rollback = SOTA_ROLLBACK,
        .user = &s,
    };
    if (!sota_engine_init(&s.engine, &cfg)) return ESP_ERR_INVALID_ARG;
    /* An unparseable key fails init: never continue unpinned. */
    for (uint8_t i = 0; i < config->num_signing_keys; ++i) {
        const simpleota_signing_key_t *k = &config->signing_keys[i];
        if (!k->pem || !sota_engine_add_key(&s.engine, k->key_id, k->pem)) {
            SOTA_LOGE("signing key %u: invalid Ed25519 public key PEM", i);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t simpleota_init(const simpleota_config_t *config)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || config->num_signing_keys > SOTA_VERIFIER_MAX_KEYS) return ESP_ERR_INVALID_ARG;
    const char *ca = config->cert_pem;
    esp_err_t (*attach)(void *conf) = config->crt_bundle_attach;
    if (!ca && !attach && !sota_framework_default_trust(&ca, &attach)) {
        SOTA_LOGE("no TLS trust anchor: enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE "
                      "or set cert_pem");
        return ESP_ERR_INVALID_STATE;
    }
    sota_framework_init_logging(TAG);

    memset(&s, 0, sizeof(s));
    if (!copy_strings(config)) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = init_engine(config);
    if (err != ESP_OK) {
        simpleota_deinit();
        return err;
    }

    /* A default CA is static: only the application's is copied. */
    s.net.cert_pem = config->cert_pem ? s.str[STR_CERT_PEM] : ca;
    s.net.crt_bundle_attach = attach;
    s.runner.engine = &s.engine;
    s.runner.initial_delay_ms = config->initial_delay_ms;
    s.runner.is_connected = config->is_connected;
    s.runner.ctx = config->user_ctx;
    s.runner.stack_size = CONFIG_SIMPLEOTA_TASK_STACK_SIZE;
    s.runner.priority = CONFIG_SIMPLEOTA_TASK_PRIORITY;
    s.manual_confirm = config->manual_confirm;
    s.event_cb = config->event_cb;
    s.user_ctx = config->user_ctx;
    s.polled = xSemaphoreCreateBinaryStatic(&s.polled_buf);
    s.initialized = true;

    sota_engine_validate_boot(&s.engine);
    SOTA_LOGI("init: device=%s chip=%s build=%" PRIu32 " keys=%u rollback=%d",
             s.str[STR_DEVICE_ID], s.str[STR_CHIP_FAMILY], s.engine.st.live.build,
             s.engine.num_keys, SOTA_ROLLBACK);
    return ESP_OK;
}

esp_err_t simpleota_start(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (s.runner.task) return ESP_OK;
    s.engine.cfg.auto_confirm = !s.manual_confirm;
    if (sota_runner_start(&s.runner, "simpleota")) return ESP_OK;
    s.engine.cfg.auto_confirm = false;
    return ESP_ERR_NO_MEM;
}

/* The task stops between steps, so a flash write is never interrupted. */
esp_err_t simpleota_stop(void)
{
    if (!sota_runner_stop(&s.runner, 60000)) {
        SOTA_LOGW("stop: task did not exit within 60 s");
        return ESP_ERR_TIMEOUT;
    }
    s.engine.cfg.auto_confirm = false;
    return ESP_OK;
}

/* Polling mode belongs to the app's own task; the managed task owns the
 * engine while it runs. */
static bool polling_allowed(const char *caller)
{
    if (!s.initialized) return false;
    if (!s.runner.task) return true;
    SOTA_LOGW("%s: the managed task is running; call simpleota_stop() first", caller);
    return false;
}

/* A polling call, run on its own task. */
typedef struct {
    bool apply;
    bool offered;
    sota_apply_result_t result;
} poll_job_t;

static void poll_task(void *arg)
{
    poll_job_t *job = (poll_job_t *)arg;
    s.poll_task = xTaskGetCurrentTaskHandle();
    if (job->apply) {
        job->result = sota_engine_apply(&s.engine);
    } else {
        job->offered = sota_engine_check(&s.engine);
    }
    s.poll_task = NULL;
    xSemaphoreGive(s.polled);
    vTaskDelete(NULL);
}

/* Run job on a task with the client's stack, at the caller's priority, and
 * wait for it. False when there is no memory for the task. */
static bool run_polled(poll_job_t *job)
{
    if (xTaskCreate(poll_task, "simpleota", CONFIG_SIMPLEOTA_TASK_STACK_SIZE, job,
                    uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        SOTA_LOGE("no memory for the polling task");
        return false;
    }
    xSemaphoreTake(s.polled, portMAX_DELAY);
    return true;
}

esp_err_t simpleota_check(bool *update_available)
{
    if (!update_available) return ESP_ERR_INVALID_ARG;
    *update_available = false;
    if (!polling_allowed("check")) return ESP_ERR_INVALID_STATE;
    poll_job_t job = {.apply = false};
    if (!run_polled(&job)) return ESP_ERR_NO_MEM;
    *update_available = job.offered;
    return s.engine.last_check_ok ? ESP_OK : ESP_FAIL;
}

simpleota_result_t simpleota_apply(void)
{
    if (!polling_allowed("apply")) return SIMPLEOTA_NO_OFFER;
    poll_job_t job = {.apply = true};
    if (!run_polled(&job)) return SIMPLEOTA_FLASH_FAIL;
    return (simpleota_result_t)job.result;
}

esp_err_t simpleota_set_server_url(const char *url)
{
    if (!s.initialized || s.runner.task) return ESP_ERR_INVALID_STATE;
    const sota_url_status_t status = sota_validate_server_url(url);
    if (status != SOTA_URL_OK) {
        SOTA_LOGW("set_server_url rejected (%s); keeping %s", sota_url_status_name(status),
                 s.engine.cfg.server_url);
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(url);
    if (!copy) return ESP_ERR_NO_MEM;
    free(s.str[STR_BASE_URL]);
    s.str[STR_BASE_URL] = copy;
    s.engine.cfg.server_url = copy;
    return ESP_OK;
}

const char *simpleota_last_offered_version(void) { return s.engine.offer.version; }

esp_err_t simpleota_check_now(void)
{
    if (!s.initialized || !s.runner.task) return ESP_ERR_INVALID_STATE;
    sota_runner_wake(&s.runner);
    return ESP_OK;
}

esp_err_t simpleota_confirm(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (sota_engine_confirm(&s.engine) == SOTA_CONFIRM_FAILED) return ESP_FAIL;
    /* The task posts the confirmed report. */
    if (s.runner.task) sota_runner_wake(&s.runner);
    return ESP_OK;
}

/* The status post needs a TLS handshake, which does not fit a typical
 * caller's stack (the main task's default is 3584 bytes). */
static void reboot_task(void *arg)
{
    (void)arg;
    sota_engine_reboot_for_update(&s.engine);
    vTaskDelete(NULL);
}

esp_err_t simpleota_reboot_for_update(void)
{
    if (!s.initialized || !sota_engine_update_waiting()) return ESP_ERR_INVALID_STATE;
    /* From event_cb, on the managed task or a polling call's task: their
     * stack is big enough. */
    if (sota_runner_on_task(&s.runner) || xTaskGetCurrentTaskHandle() == s.poll_task) {
        sota_engine_reboot_for_update(&s.engine);
    }
    /* Otherwise stop the task first, so only one TLS session is open. */
    simpleota_stop();
    if (xTaskCreate(reboot_task, "sota_reboot", CONFIG_SIMPLEOTA_TASK_STACK_SIZE, NULL,
                    CONFIG_SIMPLEOTA_TASK_PRIORITY, NULL) != pdPASS) {
        SOTA_LOGW("no memory for the reboot task; restarting without the status post");
        esp_restart();
    }
    for (;;) vTaskDelay(portMAX_DELAY);
}

bool simpleota_is_trial_boot(void) { return s.engine.in_trial; }

uint32_t simpleota_current_build(void) { return s.engine.st.live.build; }

esp_err_t simpleota_deinit(void)
{
    /* The task cannot wait for itself to stop. */
    if (sota_runner_on_task(&s.runner)) return ESP_ERR_INVALID_STATE;
    /* A task still in a long download uses the strings below. */
    if (simpleota_stop() != ESP_OK) return ESP_ERR_TIMEOUT;
    /* Also after a failed init, which may have set the engine up. */
    sota_engine_deinit(&s.engine);
    for (int i = 0; i < STR_COUNT; ++i) free(s.str[i]);
    memset(&s, 0, sizeof(s));
    return ESP_OK;
}
