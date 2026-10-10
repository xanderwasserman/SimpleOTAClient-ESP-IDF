/**
 * simpleota.c - SimpleOTA managed OTA client for ESP-IDF.
 *
 * Behavioral reference: SimpleOTAClient-Arduino v0.4.0. The protocol
 * sequence, status event vocabulary, NVS semantics, signed-firmware gate,
 * and rollback bookkeeping mirror that (hardware-verified) implementation;
 * where ESP-IDF offers a native mechanism (bootloader app rollback, the
 * certificate bundle) it is used instead of the Arduino DIY equivalent.
 *
 * Threading model: one background task ("simpleota") owns all network and
 * flash activity. Public API calls only set flags / send notifications,
 * except simpleota_confirm() and simpleota_reboot_for_update(), which are
 * documented as app-task calls and touch NVS directly (same model as the
 * Arduino client).
 */

#include "simpleota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "sota_json.h"
#include "sota_nvs.h"
#include "sota_sha256.h"
#include "sota_signing.h"
#include "sota_wdt.h"

static const char *TAG = "simpleota";

#define SOTA_DEFAULT_BASE_URL "https://simpleota.com"
#define SOTA_DEFAULT_CHECK_INTERVAL_S 3600
#define SOTA_DEFAULT_CONFIRM_TIMEOUT_S 300
#define SOTA_TRIAL_RETRY_INTERVAL_S 10
#define SOTA_MAX_REDIRECTS 3
#define SOTA_RESP_BUF_SIZE 4096

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

typedef struct {
    bool has;
    char *url; /* heap: pre-signed URLs are long and unbounded */
    char checksum[80]; /* sha256 hex, lowercased */
    char deployment_id[80];
    char version[80];
    uint32_t build_number;
    uint32_t size;
    bool offer_signed;
    bool offer_has_signature;
    uint8_t signature[64];
    char sig_key_id[64];
} sota_offer_t;

typedef struct {
    bool initialized;

    /* Config (owned copies). */
    char *token;
    char *base_url;
    char *device_id;
    char *chip_family;
    char *board_id;
    char *hardware_revision;
    char *partition_profile;
    char *channel;
    char *labels_json;
    char *security_mode;
    char *version_label; /* explicit config override; NULL = use NVS */
    uint8_t nvs_schema_version;
    uint32_t check_interval_s;
    uint32_t initial_delay_ms;
    uint32_t confirm_timeout_s;
    bool manual_confirm;
    bool disable_auto_reboot;
    uint8_t keys[SOTA_VERIFIER_MAX_KEYS][32];
    char key_ids[SOTA_VERIFIER_MAX_KEYS][64];
    uint8_t num_keys;
    bool (*is_connected)(void *user_ctx);
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;
    char *cert_pem; /* owned copy (header promises all strings are copied) */
    esp_err_t (*crt_bundle_attach)(void *conf);

    /* Persisted state, loaded at init and kept current. */
    sota_nvs_state_t nvs;

    /* Runtime. */
    sota_offer_t offer;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stop_requested;
    volatile bool in_trial;
    volatile bool confirm_timeout_fired;
    volatile bool pending_reboot; /* update flashed, waiting for manual reboot */
    esp_timer_handle_t confirm_timer;
    bool auth_warned;
} sota_state_t;

static sota_state_t s;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static char *dup_or_null(const char *str) {
    if (!str) return NULL;
    char *copy = strdup(str);
    return copy;
}

static void emit_event(simpleota_event_id_t id, const char *reason,
                       uint32_t build, size_t done, size_t total) {
    if (!s.event_cb) return;
    simpleota_event_t evt = {
        .id = id,
        .reason = reason,
        .build_number = build,
        .bytes_done = done,
        .bytes_total = total,
    };
    s.event_cb(&evt, s.user_ctx);
}

/* Reject tokens containing CR/LF or other control bytes; defends against
 * header injection if a caller ever sourced the token from untrusted input. */
static bool token_looks_safe(const char *t) {
    if (!t || !t[0]) return false;
    for (const char *p = t; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static void clear_offer(void) {
    free(s.offer.url);
    memset(&s.offer, 0, sizeof(s.offer));
}

static void lowercase(char *str) {
    for (; *str; ++str) {
        if (*str >= 'A' && *str <= 'Z') *str += 'a' - 'A';
    }
}

static const char *effective_version_label(void) {
    if (s.version_label && s.version_label[0]) return s.version_label;
    if (s.nvs.version[0]) return s.nvs.version;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * HTTP helpers
 * ------------------------------------------------------------------------- */

static void apply_tls_config(esp_http_client_config_t *cfg) {
    if (s.cert_pem) {
        cfg->cert_pem = s.cert_pem;
    } else {
        cfg->crt_bundle_attach = s.crt_bundle_attach;
    }
}

/*
 * POST a JSON body to base_url+path with the bearer token. Used for /check/
 * and /status/ only, never the firmware download.
 *
 * Retry policy (mirrors Arduino postJson): on transport failure, wait 2 s
 * and retry once. Any received HTTP response is returned immediately; a 4xx
 * retry would be pointless and a 5xx retry is left to the next cycle.
 *
 * Returns the HTTP status code, or -1 on transport failure. On success the
 * body (up to resp_cap-1 bytes) is copied into resp, null-terminated.
 */
static int http_post_json(const char *path, const char *body, char *resp,
                          size_t resp_cap) {
    char url[160];
    int n = snprintf(url, sizeof(url), "%s%s", s.base_url, path);
    if (n <= 0 || (size_t)n >= sizeof(url)) return -1;

    char auth[192];
    n = snprintf(auth, sizeof(auth), "Bearer %s", s.token);
    if (n <= 0 || (size_t)n >= sizeof(auth)) return -1;

    size_t body_len = strlen(body);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(2000));

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
            .buffer_size = 2048,
            .buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER,
        };
        apply_tls_config(&cfg);

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;

        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_err_t err = esp_http_client_open(client, body_len);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "POST %s open failed: %s (attempt %d)", path,
                     esp_err_to_name(err), attempt);
            esp_http_client_cleanup(client);
            continue;
        }
        int written = esp_http_client_write(client, body, body_len);
        if (written < 0 || (size_t)written != body_len) {
            esp_http_client_cleanup(client);
            continue;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            esp_http_client_cleanup(client);
            continue;
        }
        int status = esp_http_client_get_status_code(client);

        size_t total = 0;
        if (resp && resp_cap > 1) {
            while (total < resp_cap - 1) {
                int r = esp_http_client_read(client, resp + total,
                                             resp_cap - 1 - total);
                if (r <= 0) break;
                total += (size_t)r;
            }
        }
        if (resp && resp_cap > 0) resp[total] = '\0';

        esp_http_client_cleanup(client);

        ESP_LOGD(TAG, "POST %s -> %d", path, status);
        if ((status == 401 || status == 403) && !s.auth_warned) {
            s.auth_warned = true;
            ESP_LOGW(TAG,
                     "server rejected the token (HTTP %d). Check your project "
                     "token.",
                     status);
        }
        return status; /* any received response: no retry */
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * /api/v1/ota/status/
 * ------------------------------------------------------------------------- */

/*
 * Post one lifecycle event. Success requires BOTH a 2xx and "accepted": true
 * in the body (mirrors Arduino sendStatusFor).
 */
static bool post_status_for(const char *event, const char *reason,
                            const char *deployment_id, uint32_t build_number) {
    if (!deployment_id || !deployment_id[0]) {
        ESP_LOGD(TAG, "status: no deployment context; skipping %s", event);
        return false;
    }
    ESP_LOGI(TAG, "status: event=%s build=%" PRIu32 " deployment=%s%s%s", event,
             build_number, deployment_id, reason ? " reason=" : "",
             reason ? reason : "");

    /* Values here are our own identifiers (device id, deployment uuid, event
     * and reason tokens): no JSON escaping needed, same as the Arduino
     * client's body construction. */
    char body[384];
    int n;
    if (reason) {
        n = snprintf(body, sizeof(body),
                     "{\"device_id\":\"%s\",\"deployment_id\":\"%s\","
                     "\"event\":\"%s\",\"build_number\":%" PRIu32
                     ",\"reason\":\"%s\"}",
                     s.device_id, deployment_id, event, build_number, reason);
    } else {
        n = snprintf(body, sizeof(body),
                     "{\"device_id\":\"%s\",\"deployment_id\":\"%s\","
                     "\"event\":\"%s\",\"build_number\":%" PRIu32 "}",
                     s.device_id, deployment_id, event, build_number);
    }
    if (n <= 0 || (size_t)n >= sizeof(body)) return false;

    char resp[256];
    int status = http_post_json("/api/v1/ota/status/", body, resp, sizeof(resp));
    if (status < 200 || status >= 300) return false;

    bool accepted = false;
    sota_json_get_bool(resp, "accepted", &accepted);
    return accepted;
}

/* Post a status event for the CURRENT offer. */
static bool post_status(const char *event, const char *reason) {
    return post_status_for(event, reason, s.offer.deployment_id,
                           s.offer.build_number);
}
