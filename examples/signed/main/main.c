/**
 * SimpleOTA signed-firmware example: on-device Ed25519 verification.
 *
 * Like the basic example, plus: the project's signing PUBLIC key is pinned
 * and security_mode is "signed". Every offered image must carry a valid
 * Ed25519 signature over its exact bytes, verified on this device before
 * the partition is marked bootable. A missing, stripped, or invalid
 * signature aborts the update (reported as failed / signature_invalid) and
 * the device stays on its current build.
 *
 * Setup:
 *  1. Create a signing key in the SimpleOTA dashboard (project -> Signing
 *     keys) and paste the PUBLIC key PEM below.
 *  2. Sign uploads in CI (see the SimpleOTA signed-firmware guide) or use
 *     the dashboard's "Sign it for me in my browser" helper.
 *
 * The placeholder key below deliberately does NOT parse:
 * simpleota_init() fails closed rather than running unverified.
 */

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "simpleota.h"
#include "wifi_connect.h"

static const char *TAG = "example";

/* PASTE YOUR REAL PUBLIC KEY HERE (dashboard -> project -> Signing keys). */
static const char *SIGNING_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "PASTE_YOUR_PROJECT_PUBLIC_KEY_HERE_THIS_WILL_NOT_PARSE\n"
    "-----END PUBLIC KEY-----\n";

static bool is_connected(void *ctx) {
    (void)ctx;
    return example_wifi_is_connected();
}

static void on_ota_event(const simpleota_event_t *evt, void *ctx) {
    (void)ctx;
    if (evt->id == SIMPLEOTA_EVENT_UPDATE_FAILED) {
        ESP_LOGW(TAG, "update failed: %s", evt->reason ? evt->reason : "?");
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(example_wifi_connect());

    const simpleota_signing_key_t keys[] = {
        {.key_id = NULL, .pem = SIGNING_PUBLIC_KEY_PEM},
        /* During key rotation, pin the old and the new key side by side:
         * {.key_id = "key-2027", .pem = NEW_KEY_PEM}, */
    };

    simpleota_config_t cfg = {
        .token = CONFIG_EXAMPLE_SIMPLEOTA_TOKEN,
        .board_id = "esp32-devkitc",
        .security_mode = SIMPLEOTA_SECURITY_SIGNED,
        .signing_keys = keys,
        .num_signing_keys = 1,
        .check_interval_s = 300,
        .event_cb = on_ota_event,
        .is_connected = is_connected,
    };

    /* Fail closed: an unparseable key aborts here instead of silently
     * running without verification. Paste your real key above. */
    err = simpleota_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "simpleota_init failed (%s). Did you paste your project's "
                 "signing PUBLIC key? Halting.",
                 esp_err_to_name(err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
    ESP_ERROR_CHECK(simpleota_start());

    ESP_LOGI(TAG, "running build %" PRIu32 "; signed OTA task started",
             simpleota_current_build());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
