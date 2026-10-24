/**
 * sota_boot_ota.c - sota_boot.h on esp_ota_* and esp_reset_reason().
 */

#include "sota_boot.h"

#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_app_desc.h"
#define running_elf_sha256 esp_app_get_elf_sha256
#else
#define running_elf_sha256 esp_ota_get_app_elf_sha256
#endif
#include "sota_trial.h"

_Static_assert((int)SOTA_RST_POWERON == (int)ESP_RST_POWERON &&
                   (int)SOTA_RST_SW == (int)ESP_RST_SW &&
                   (int)SOTA_RST_PANIC == (int)ESP_RST_PANIC &&
                   (int)SOTA_RST_INT_WDT == (int)ESP_RST_INT_WDT &&
                   (int)SOTA_RST_TASK_WDT == (int)ESP_RST_TASK_WDT &&
                   (int)SOTA_RST_WDT == (int)ESP_RST_WDT &&
                   (int)SOTA_RST_DEEPSLEEP == (int)ESP_RST_DEEPSLEEP &&
                   (int)SOTA_RST_BROWNOUT == (int)ESP_RST_BROWNOUT,
               "sota_trial.h reset codes match esp_reset_reason_t");

static const esp_partition_t *app_at(uint32_t addr)
{
    const esp_partition_t *match = NULL;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it && !match; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->address == addr) match = p;
    }
    esp_partition_iterator_release(it);
    return match;
}

static bool state_is(const esp_partition_t *p, esp_ota_img_states_t want)
{
    esp_ota_img_states_t st;
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == want;
}

static bool running_pending(void)
{
    return state_is(esp_ota_get_running_partition(), ESP_OTA_IMG_PENDING_VERIFY);
}

void sota_boot_info(sota_boot_info_t *info)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    running_elf_sha256(info->image_id, sizeof(info->image_id));
    info->running_part = running ? running->address : 0;
    info->boot_part = boot ? boot->address : 0;
    info->reset_reason = (int)esp_reset_reason();
    info->pending = state_is(running, ESP_OTA_IMG_PENDING_VERIFY);
}

bool sota_boot_slot_invalid(uint32_t part)
{
    return state_is(app_at(part), ESP_OTA_IMG_INVALID);
}

bool sota_boot_set(uint32_t part)
{
    const esp_partition_t *p = app_at(part);
    return p && esp_ota_set_boot_partition(p) == ESP_OK;
}

bool sota_boot_mark_valid(void)
{
    return !running_pending() || esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

void sota_boot_rollback(uint32_t prev_part)
{
    /* Returns only when it could not. */
    if (running_pending()) esp_ota_mark_app_invalid_rollback_and_reboot();
    const esp_partition_t *prev = app_at(prev_part);
    if (prev) esp_ota_set_boot_partition(prev);
    esp_restart();
}
