/**
 * sota_nvs.c - NVS persistence. See header for the key map and semantics.
 */

#include "sota_nvs.h"

#include <string.h>

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "simpleota";

#define NS "simpleota"

/* --------------------------------------------------------------------------- */

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    out[0] = '\0';
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) out[0] = '\0';
}

static void get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    if (nvs_get_u32(h, key, out) != ESP_OK) *out = 0;
}

static void get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    if (nvs_get_u8(h, key, out) != ESP_OK) *out = 0;
}

esp_err_t sota_nvs_load(sota_nvs_state_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh device: no saved OTA state yet. Not an error. */
        ESP_LOGD(TAG, "nvs: no saved OTA state yet (fresh device)");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    get_u32(h, "sota_build", &out->build);
    get_str(h, "sota_hash", out->hash, sizeof(out->hash));
    get_str(h, "sota_ver", out->version, sizeof(out->version));
    get_u8(h, "sota_trial", &out->trial);
    get_u32(h, "sota_new_part", &out->new_part);
    get_u32(h, "sota_prev_part", &out->prev_part);
    get_u32(h, "sota_prev_build", &out->prev_build);
    get_str(h, "sota_prev_hash", out->prev_hash, sizeof(out->prev_hash));
    get_str(h, "sota_prev_ver", out->prev_version, sizeof(out->prev_version));
    get_str(h, "sota_fail_dep", out->fail_dep, sizeof(out->fail_dep));
    get_u32(h, "sota_fail_build", &out->fail_build);
    get_u8(h, "sota_conf_pend", &out->conf_pend);
    get_str(h, "sota_rb_reason", out->rb_reason, sizeof(out->rb_reason));

    nvs_close(h);
    return ESP_OK;
}

/* --------------------------------------------------------------------------- */

esp_err_t sota_nvs_arm_trial(uint32_t new_build, const char *new_hash,
                             const char *new_version, uint32_t new_part,
                             uint32_t prev_part, uint32_t prev_build,
                             const char *prev_hash, const char *prev_version,
                             const char *deployment_id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* One handle, one commit: either the whole arm lands or none of it. */
    esp_err_t rc = ESP_OK;
#define DO(x)                       \
    do {                            \
        if (rc == ESP_OK) rc = (x); \
    } while (0)

    DO(nvs_set_u32(h, "sota_prev_part", prev_part));
    DO(nvs_set_u32(h, "sota_prev_build", prev_build));
    DO(nvs_set_str(h, "sota_prev_hash", prev_hash ? prev_hash : ""));
    DO(nvs_set_str(h, "sota_prev_ver", prev_version ? prev_version : ""));

    DO(nvs_set_u32(h, "sota_build", new_build));
    DO(nvs_set_str(h, "sota_hash", new_hash ? new_hash : ""));
    DO(nvs_set_str(h, "sota_ver", new_version ? new_version : ""));
    DO(nvs_set_u32(h, "sota_new_part", new_part));

    DO(nvs_set_str(h, "sota_fail_dep", deployment_id ? deployment_id : ""));
    DO(nvs_set_u32(h, "sota_fail_build", new_build));
    DO(nvs_set_u8(h, "sota_conf_pend", 1));

    DO(nvs_set_u8(h, "sota_trial", 1));

    DO(nvs_commit(h));
#undef DO
    nvs_close(h);
    return rc;
}

esp_err_t sota_nvs_rollback_restore(const sota_nvs_state_t *snapshot,
                                    const char *reason) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* Restore UNCONDITIONALLY: zero / empty are valid fresh-device values. */
    esp_err_t rc = ESP_OK;
#define DO(x)                       \
    do {                            \
        if (rc == ESP_OK) rc = (x); \
    } while (0)
    DO(nvs_set_u32(h, "sota_build", snapshot->prev_build));
    DO(nvs_set_str(h, "sota_hash", snapshot->prev_hash));
    DO(nvs_set_str(h, "sota_ver", snapshot->prev_version));
    DO(nvs_set_u8(h, "sota_trial", 2));
    DO(nvs_set_str(h, "sota_rb_reason",
                   reason ? reason : "confirm_timeout"));
    DO(nvs_commit(h));
#undef DO
    nvs_close(h);
    return rc;
}

static void erase_quiet(nvs_handle_t h, const char *key) {
    esp_err_t err = nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs: erase %s failed: %s", key, esp_err_to_name(err));
    }
}

esp_err_t sota_nvs_clear_trial(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    erase_quiet(h, "sota_trial");
    erase_quiet(h, "sota_new_part");
    erase_quiet(h, "sota_prev_part");
    erase_quiet(h, "sota_prev_build");
    erase_quiet(h, "sota_prev_hash");
    erase_quiet(h, "sota_prev_ver");
    erase_quiet(h, "sota_rb_reason");
    /* fail_dep / fail_build / conf_pend deliberately kept: they persist
     * until the server accepts the deferred report. */
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t sota_nvs_clear_report(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    erase_quiet(h, "sota_fail_dep");
    erase_quiet(h, "sota_fail_build");
    erase_quiet(h, "sota_conf_pend");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
