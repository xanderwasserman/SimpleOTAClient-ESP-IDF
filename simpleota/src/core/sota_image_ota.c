/**
 * sota_image_ota.c - sota_image.h on esp_ota_*.
 */

#include "sota_image.h"

#include "esp_ota_ops.h"

static esp_ota_handle_t s_handle;

bool sota_image_begin(size_t expected, uint32_t *slot, uint32_t *running)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!run || !next || expected > next->size) return false;
    /* Erase as the write goes. A size here would erase that much up front,
     * stalling the open download for seconds. */
    if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK) return false;
    *slot = next->address;
    *running = run->address;
    return true;
}

bool sota_image_write(const uint8_t *buf, size_t len)
{
    return esp_ota_write(s_handle, buf, len) == ESP_OK;
}

void sota_image_abort(void)
{
    esp_ota_abort(s_handle);
}

bool sota_image_finish(void)
{
    return esp_ota_end(s_handle) == ESP_OK;
}
