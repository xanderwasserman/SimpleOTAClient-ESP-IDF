/**
 * SimpleOTA polling example: the app runs the update checks from its own
 * task instead of the component's managed task.
 *
 * Flow: bring up Wi-Fi, initialize SimpleOTA, then once an hour: confirm a
 * trial once the app is healthy, check in, and apply an offered update.
 * In polling mode the app must call simpleota_confirm(): an update that is
 * never confirmed rolls back after the confirm timeout.
 *
 * Configure Wi-Fi credentials and the token via `idf.py menuconfig`
 * (Example Configuration).
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "simpleota.h"
#include "wifi_connect.h"

static const char *TAG = "example";

#define CHECK_INTERVAL_MS (60 * 60 * 1000)
#define TRIAL_POLL_MS (5 * 1000)

/* Stand-in for the app's own health check: sensors read, server reached. */
static bool app_healthy(void) {
    return example_wifi_is_connected();
}

static void check_for_update(void) {
    bool update = false;
    esp_err_t err = simpleota_check(&update);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "check failed (%s); trying again later", esp_err_to_name(err));
        return;
    }
    if (!update) return;

    ESP_LOGI(TAG, "update offered: %s", simpleota_last_offered_version());
    simpleota_result_t r = simpleota_apply(); /* restarts on success */
    switch (r) {
    case SIMPLEOTA_OK:
        break;
    case SIMPLEOTA_UNCONFIRMED:
        ESP_LOGW(TAG, "this image is on trial: confirm it before updating");
        break;
    case SIMPLEOTA_DOWNGRADE:
        ESP_LOGW(TAG, "the offered build is not newer; refused");
        break;
    case SIMPLEOTA_SIGNATURE_FAIL:
        ESP_LOGE(TAG, "the update's signature did not verify");
        break;
    default:
        ESP_LOGW(TAG, "update failed (%d); next check in an hour", (int)r);
        break;
    }
}

void app_main(void) {
    /* NVS is required (Wi-Fi calibration + SimpleOTA state). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Before Wi-Fi: on a trial boot, init starts the watchdog that rolls
     * back a hang. */
    simpleota_config_t cfg = {
        .token = CONFIG_EXAMPLE_SIMPLEOTA_TOKEN,
        .confirm_timeout_s = 120,
    };
    ESP_ERROR_CHECK(simpleota_init(&cfg));
    ESP_LOGI(TAG, "running build %" PRIu32 "%s", simpleota_current_build(),
             simpleota_is_trial_boot() ? " (on trial)" : "");

    ESP_ERROR_CHECK(example_wifi_connect());

    for (;;) {
        /* A trial must be confirmed within confirm_timeout_s, so while one
         * runs, look again every few seconds; no update is applied until
         * it is confirmed. */
        if (simpleota_is_trial_boot()) {
            if (app_healthy()) ESP_ERROR_CHECK_WITHOUT_ABORT(simpleota_confirm());
            vTaskDelay(pdMS_TO_TICKS(TRIAL_POLL_MS));
            continue;
        }
        check_for_update();
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}
