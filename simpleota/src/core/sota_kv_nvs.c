/**
 * sota_kv_nvs.c - sota_kv.h on ESP32 NVS, namespace "simpleota".
 *
 * Uses only the NVS API that ESP-IDF 4.4 to 6 and Arduino-ESP32 2.x and 3.x
 * all have. NVS must be initialised (nvs_flash_init()) before the first call.
 * Every call opens and closes its own handle, so nothing is held between
 * calls and a call from any task is safe.
 */

#include "sota_kv.h"

#include "nvs.h"

#define SOTA_KV_NAMESPACE "simpleota"

static bool open_ns(nvs_open_mode_t mode, nvs_handle_t *h)
{
    return nvs_open(SOTA_KV_NAMESPACE, mode, h) == ESP_OK;
}

/* Close after a write; true when the write and its commit both succeeded. */
static bool finish_write(nvs_handle_t h, esp_err_t err)
{
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool sota_kv_get_u8(const char *key, uint8_t *out)
{
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    uint8_t v;
    bool ok = nvs_get_u8(h, key, &v) == ESP_OK;
    nvs_close(h);
    if (ok) *out = v;
    return ok;
}

bool sota_kv_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    uint32_t v;
    bool ok = nvs_get_u32(h, key, &v) == ESP_OK;
    nvs_close(h);
    if (ok) *out = v;
    return ok;
}

bool sota_kv_get_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    size_t len = 0;
    bool ok = nvs_get_str(h, key, NULL, &len) == ESP_OK && len <= cap &&
              nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool sota_kv_get_blob(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    size_t stored = 0;
    bool ok = nvs_get_blob(h, key, NULL, &stored) == ESP_OK && stored == len &&
              nvs_get_blob(h, key, out, &stored) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool sota_kv_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    return open_ns(NVS_READWRITE, &h) && finish_write(h, nvs_set_u8(h, key, value));
}

bool sota_kv_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t h;
    return open_ns(NVS_READWRITE, &h) && finish_write(h, nvs_set_u32(h, key, value));
}

bool sota_kv_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    return open_ns(NVS_READWRITE, &h) && finish_write(h, nvs_set_str(h, key, value));
}

bool sota_kv_set_blob(const char *key, const void *value, size_t len)
{
    nvs_handle_t h;
    return open_ns(NVS_READWRITE, &h) && finish_write(h, nvs_set_blob(h, key, value, len));
}

bool sota_kv_erase(const char *key)
{
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return false;
    esp_err_t err = nvs_erase_key(h, key);
    return finish_write(h, err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}
