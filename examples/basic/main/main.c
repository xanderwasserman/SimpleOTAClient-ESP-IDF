/**
 * SimpleOTA basic example: managed OTA updates for an ESP-IDF project.
 *
 * Flow: initialize NVS, start SimpleOTA, then bring up Wi-Fi. Init runs
 * before Wi-Fi so a trial boot arms the chip watchdog and the confirm
 * timer before network bring-up. The component checks for updates on an
 * interval, downloads and flashes offered firmware (SHA-256 verified),
 * reboots into it as a bootloader-supervised trial, and confirms it on
 * the first successful check-in after boot. If the new image crashes,
 * hangs, or never confirms, the bootloader rolls back to this image.
 *
 * Configure Wi-Fi credentials and the token via `idf.py menuconfig`
 * (Example Configuration).
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

static bool is_connected(void *ctx) {
    (void)ctx;
    return example_wifi_is_connected();
}

static void on_ota_event(const simpleota_event_t *evt, void *ctx) {
    (void)ctx;
    switch (evt->id) {
    case SIMPLEOTA_EVENT_UPDATE_AVAILABLE:
        ESP_LOGI(TAG, "update available: build %" PRIu32, evt->build_number);
        break;
    case SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS:
        if (evt->bytes_total) {
            ESP_LOGI(TAG, "downloading: %u/%u bytes",
                     (unsigned)evt->bytes_done, (unsigned)evt->bytes_total);
        }
        break;
    case SIMPLEOTA_EVENT_UPDATE_FAILED:
        ESP_LOGW(TAG, "update failed: %s", evt->reason ? evt->reason : "?");
        break;
    case SIMPLEOTA_EVENT_TRIAL_BOOT:
        ESP_LOGI(TAG, "trial boot of build %" PRIu32 " (unconfirmed)",
                 evt->build_number);
        break;
    case SIMPLEOTA_EVENT_CONFIRMED:
        ESP_LOGI(TAG, "build %" PRIu32 " confirmed", evt->build_number);
        break;
    default:
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

    simpleota_config_t cfg = {
        .token = CONFIG_EXAMPLE_SIMPLEOTA_TOKEN,
        /* device_id defaults to the Wi-Fi MAC; chip_family to this target. */
        .board_id = "esp32-devkitc",
        .check_interval_s = 300, /* fast-ish for a demo; default is 3600 */
        .event_cb = on_ota_event,
        .is_connected = is_connected,
    };
    ESP_ERROR_CHECK(simpleota_init(&cfg));
    ESP_ERROR_CHECK(simpleota_start());

    ESP_ERROR_CHECK(example_wifi_connect());

    ESP_LOGI(TAG, "running build %" PRIu32 "; OTA task started",
             simpleota_current_build());

    /* The application owns the main loop; OTA happens in the background. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
