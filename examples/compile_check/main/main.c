/**
 * Builds the component on chips that have no Wi-Fi radio.
 * Calls init and confirm so the trial watchdog is part of the link.
 */

#include "nvs_flash.h"
#include "simpleota.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    simpleota_config_t cfg = {
        .token = "sota_pk_compile_check",
    };
    simpleota_init(&cfg);
    simpleota_confirm();
}
