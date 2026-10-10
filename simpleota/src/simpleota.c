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

/*
 * Deferred terminal reports. Both retry across reboots until the server
 * accepts them; the NVS keys they consume are cleared only on acceptance.
 */
static void report_rolled_back_if_pending(void) {
    if (s.nvs.trial != 2) return;
    if (!s.nvs.fail_dep[0]) {
        /* No context to report about (legacy state); just clear. */
        s.nvs.trial = 0;
        sota_nvs_clear_trial();
        sota_nvs_clear_report();
        return;
    }
    const char *reason =
        s.nvs.rb_reason[0] ? s.nvs.rb_reason : "confirm_timeout";
    if (post_status_for("rolled_back", reason, s.nvs.fail_dep,
                        s.nvs.fail_build)) {
        s.nvs.trial = 0;
        s.nvs.conf_pend = 0;
        s.nvs.fail_dep[0] = '\0';
        s.nvs.fail_build = 0;
        s.nvs.rb_reason[0] = '\0';
        sota_nvs_clear_trial();
        sota_nvs_clear_report();
        ESP_LOGI(TAG, "rolled_back report accepted");
    }
}

static void report_confirmed_if_pending(void) {
    if (!s.nvs.conf_pend) return;
    /* Only after the trial resolved on THIS image: never while a trial is
     * running, never while an armed-but-not-yet-booted update is pending
     * (trial==1 in the deferred-reboot window), never while a rolled_back
     * report is outstanding (trial==2). Mirrors the Arduino client, which
     * queues the confirmed report only when boot validation resolves. */
    if (s.in_trial || s.nvs.trial != 0) return;
    if (!s.nvs.fail_dep[0]) {
        s.nvs.conf_pend = 0;
        sota_nvs_clear_report();
        return;
    }
    if (post_status_for("confirmed", NULL, s.nvs.fail_dep, s.nvs.fail_build)) {
        s.nvs.conf_pend = 0;
        s.nvs.fail_dep[0] = '\0';
        s.nvs.fail_build = 0;
        sota_nvs_clear_report();
        emit_event(SIMPLEOTA_EVENT_CONFIRMED, NULL, s.nvs.build, 0, 0);
        ESP_LOGI(TAG, "confirmed report accepted");
    }
}

/* ---------------------------------------------------------------------------
 * Confirm / rollback
 * ------------------------------------------------------------------------- */

static void stop_confirm_timer(void) {
    if (s.confirm_timer) {
        esp_timer_stop(s.confirm_timer); /* ok if not running */
        esp_timer_delete(s.confirm_timer);
        s.confirm_timer = NULL;
    }
}

/* Confirm the running trial image. Called from the component task
 * (auto-confirm) or the app task (simpleota_confirm). */
static void do_confirm(void) {
    if (!s.in_trial) return;
    s.in_trial = false; /* first: disarms a racing timeout callback */
    sota_wdt_disarm();

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Local operation; cannot fail on network. Seals the bootloader state. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
#endif
    stop_confirm_timer();
    sota_nvs_clear_trial();
    s.nvs.trial = 0;
    ESP_LOGI(TAG, "trial image confirmed (build %" PRIu32 ")", s.nvs.build);
    /* The confirmed status POST happens via report_confirmed_if_pending()
     * on the check path (we already know the server is reachable there). */
}

/* Execute the confirm-timeout rollback. Runs in the COMPONENT task (the
 * esp_timer callback only notifies; see plan: never run NVS + mark_invalid
 * work on the shared esp_timer task stack). */
static void do_rollback_from_timeout(void) {
    if (!s.in_trial) return; /* confirmed in the meantime */
    s.in_trial = false;
    sota_wdt_disarm();

    ESP_LOGW(TAG, "confirm timeout: rolling back to build %" PRIu32,
             s.nvs.prev_build);
    emit_event(SIMPLEOTA_EVENT_ROLLING_BACK, "confirm_timeout",
               s.nvs.prev_build, 0, 0);

    /* Restore identity BEFORE rebooting so the next boot reports the old
     * build (unconditionally: prev_build==0 is valid fresh-device state). */
    sota_nvs_rollback_restore(&s.nvs, "confirm_timeout");
    stop_confirm_timer();

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Marks the running app INVALID in otadata and reboots into the last
     * bootable image. Returns only on error. */
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback reboot failed: %s (staying up)",
             esp_err_to_name(err));
#else
    /* Should be unreachable: the timer is only armed with rollback support. */
    esp_restart();
#endif
}

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static void confirm_timer_cb(void *arg) {
    (void)arg;
    /* esp_timer task context: keep it tiny. The component task does the work. */
    s.confirm_timeout_fired = true;
    if (s.task) xTaskNotifyGive(s.task);
}

static esp_err_t arm_confirm_timer(void) {
    const esp_timer_create_args_t args = {
        .callback = confirm_timer_cb,
        .name = "sota_confirm",
    };
    esp_err_t err = esp_timer_create(&args, &s.confirm_timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_once(s.confirm_timer,
                                (uint64_t)s.confirm_timeout_s * 1000000ULL);
}
#endif /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE */

/* ---------------------------------------------------------------------------
 * Boot-time trial validation
 * ------------------------------------------------------------------------- */

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* Map a stored flash address back to its app partition (for otadata state). */
static const esp_partition_t *find_app_partition_by_addr(uint32_t addr) {
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p && p->address == addr) {
            esp_partition_iterator_release(it);
            return p;
        }
    }
    return NULL;
}
#endif

static void process_boot_validation(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (s.nvs.trial == 1) {
        if (running && running->address == s.nvs.new_part) {
            /* First boot of the new image: a trial. */
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
            esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
            esp_ota_get_state_partition(running, &state);
            if (state != ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGW(TAG,
                         "trial boot but app state is %d (expected "
                         "PENDING_VERIFY); bootloader rollback may be off",
                         (int)state);
            }
            s.in_trial = true;
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                sota_wdt_arm(s.confirm_timeout_s);
            }
            if (arm_confirm_timer() != ESP_OK) {
                /* Cannot supervise the trial: fail safe by rolling back now
                 * rather than running unconfirmed forever. */
                ESP_LOGE(TAG, "confirm timer unavailable; rolling back");
                do_rollback_from_timeout();
                return;
            }
            ESP_LOGI(TAG,
                     "trial boot of build %" PRIu32 "; confirm within %" PRIu32
                     " s",
                     s.nvs.build, s.confirm_timeout_s);
            emit_event(SIMPLEOTA_EVENT_TRIAL_BOOT, NULL, s.nvs.build, 0, 0);
#else
            /* Degraded mode: no bootloader rollback available. Confirm on
             * the first successful check only; log once. */
            ESP_LOGW(TAG,
                     "running new image without CONFIG_BOOTLOADER_APP_"
                     "ROLLBACK_ENABLE: no automatic rollback. The image is "
                     "treated as accepted; 'confirmed' is reported on the "
                     "first successful check-in.");
            sota_nvs_clear_trial();
            s.nvs.trial = 0;
#endif
        } else {
            bool image_invalid = false;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
            const esp_partition_t *new_part =
                find_app_partition_by_addr(s.nvs.new_part);
            if (new_part) {
                esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
                if (esp_ota_get_state_partition(new_part, &st) == ESP_OK &&
                    st == ESP_OTA_IMG_INVALID) {
                    /* Only the confirm-timeout path marks the slot INVALID. */
                    image_invalid = true;
                }
            }
#endif
            const char *cause =
                sota_rollback_cause(image_invalid, (int)esp_reset_reason());
            ESP_LOGW(TAG,
                     "rollback detected (%s): running 0x%" PRIx32
                     " but new image was at 0x%" PRIx32,
                     cause, running ? (uint32_t)running->address : 0,
                     s.nvs.new_part);
            sota_nvs_rollback_restore(&s.nvs, cause);
            /* Reflect the restore in RAM too. */
            s.nvs.build = s.nvs.prev_build;
            strlcpy(s.nvs.hash, s.nvs.prev_hash, sizeof(s.nvs.hash));
            strlcpy(s.nvs.version, s.nvs.prev_version, sizeof(s.nvs.version));
            strlcpy(s.nvs.rb_reason, cause, sizeof(s.nvs.rb_reason));
            s.nvs.trial = 2;
        }
    }

    /* trial==2 (rolled_back pending) and conf_pend survive in s.nvs and are
     * retried by the deferred-report helpers until the server accepts. */
}

/* ---------------------------------------------------------------------------
 * /api/v1/ota/check/
 * ------------------------------------------------------------------------- */

/* Build the check-in body by concatenation (same approach as the Arduino
 * client): config strings are identifiers the integrator controls and are
 * documented as needing to be JSON-safe; labels_json is embedded verbatim. */
static char *build_check_body(void) {
    size_t cap = 1024;
    if (s.labels_json) cap += strlen(s.labels_json);
    char *body = malloc(cap);
    if (!body) return NULL;

    size_t off = (size_t)snprintf(
        body, cap,
        "{\"device_id\":\"%s\",\"framework\":\"esp_idf\","
        "\"chip_family\":\"%s\",\"current_build_number\":%" PRIu32
        ",\"nvs_schema_version\":%u",
        s.device_id, s.chip_family, s.nvs.build, (unsigned)s.nvs_schema_version);

#define APPEND(...)                                                       \
    do {                                                                  \
        int a = snprintf(body + off, cap - off, __VA_ARGS__);             \
        if (a < 0 || (size_t)a >= cap - off) {                            \
            free(body);                                                   \
            return NULL;                                                  \
        }                                                                 \
        off += (size_t)a;                                                 \
    } while (0)

    if (s.board_id) APPEND(",\"board_id\":\"%s\"", s.board_id);
    if (s.hardware_revision)
        APPEND(",\"hardware_revision\":\"%s\"", s.hardware_revision);
    if (s.partition_profile)
        APPEND(",\"partition_profile\":\"%s\"", s.partition_profile);
    if (s.security_mode) APPEND(",\"security_mode\":\"%s\"", s.security_mode);
    if (s.channel) APPEND(",\"channel\":\"%s\"", s.channel);
    const char *ver = effective_version_label();
    if (ver) APPEND(",\"version_label\":\"%s\"", ver);
    if (s.nvs.hash[0]) APPEND(",\"current_hash\":\"%s\"", s.nvs.hash);
    if (s.labels_json) APPEND(",\"labels\":%s", s.labels_json);
    APPEND("}");
#undef APPEND

    return body;
}

/* Returns true when an update offer was stored. */
static bool do_check(void) {
    /* If a prior boot rolled back, try to inform the server FIRST, as its
     * own independent status POST (Arduino ordering): a degraded /check/
     * endpoint must not delay the rollback signal that feeds the server's
     * auto-pause safety. Best-effort; retried until accepted. */
    report_rolled_back_if_pending();

    char *body = build_check_body();
    if (!body) return false;

    char *resp = malloc(SOTA_RESP_BUF_SIZE);
    if (!resp) {
        free(body);
        return false;
    }

    int status = http_post_json("/api/v1/ota/check/", body, resp,
                                SOTA_RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        ESP_LOGD(TAG, "check: HTTP %d", status);
        free(resp);
        return false;
    }

    /* Server confirmed reachable: auto-confirm a running trial and settle
     * the deferred confirmed report (Arduino check() ordering). */
    if (s.in_trial && !s.manual_confirm) {
        do_confirm();
    }
    report_confirmed_if_pending();

    bool got_offer = false;
    bool available = false;
    if (!sota_json_get_bool(resp, "update_available", &available) || !available) {
        char status_str[48];
        bool has_status =
            sota_json_get_string(resp, "status", status_str, sizeof(status_str));
        ESP_LOGI(TAG, "check: no update (%s)",
                 has_status ? status_str : "up_to_date");
        emit_event(SIMPLEOTA_EVENT_CHECK_DONE, has_status ? status_str : NULL,
                   s.nvs.build, 0, 0);
        free(resp);
        return false;
    }

    char *url = malloc(2048); /* pre-signed URLs are long */
    char checksum[80];
    char mode[24], algo[24], key_id[64], sig_b64[128];
    uint32_t build = 0;
    if (!url) {
        free(resp);
        return false;
    }

    if (!sota_json_get_string(resp, "url", url, 2048) ||
        !sota_json_get_string(resp, "checksum", checksum, sizeof(checksum)) ||
        !sota_json_get_u32(resp, "build_number", &build)) {
        ESP_LOGW(TAG, "check: malformed offer payload");
        free(url);
        free(resp);
        return false;
    }

    clear_offer();
    s.offer.url = url; /* ownership moves to the offer */
    strlcpy(s.offer.checksum, checksum, sizeof(s.offer.checksum));
    sota_json_get_string(resp, "deployment_id", s.offer.deployment_id,
                         sizeof(s.offer.deployment_id));
    sota_json_get_string(resp, "version", s.offer.version,
                         sizeof(s.offer.version));
    s.offer.build_number = build;
    sota_json_get_u32(resp, "size", &s.offer.size);

    /* Signed-firmware fields (only meaningful for signed offers). */
    s.offer.offer_signed =
        sota_json_get_string(resp, "security_mode", mode, sizeof(mode)) &&
        strcmp(mode, SIMPLEOTA_SECURITY_SIGNED) == 0;
    if (sota_json_get_string(resp, "signing_key_id", key_id, sizeof(key_id)))
        strlcpy(s.offer.sig_key_id, key_id, sizeof(s.offer.sig_key_id));
    if (s.offer.offer_signed &&
        sota_json_get_string(resp, "signature", sig_b64, sizeof(sig_b64))) {
        /* An algorithm other than ed25519 leaves the signature unusable:
         * verification then fails closed when keys are pinned. */
        bool has_algo =
            sota_json_get_string(resp, "signature_algorithm", algo, sizeof(algo));
        bool algo_ok = !has_algo || !algo[0] || strcmp(algo, "ed25519") == 0;
        if (algo_ok && sota_decode_signature_b64(sig_b64, s.offer.signature)) {
            s.offer.offer_has_signature = true;
        } else {
            ESP_LOGW(TAG, "check: signed offer carries unusable signature (algo=%s)",
                     has_algo ? algo : "?");
        }
    }
    free(resp);

    lowercase(s.offer.checksum);
    s.offer.has = true;
    got_offer = true;
    ESP_LOGI(TAG, "check: offer build=%" PRIu32 " version=%s size=%" PRIu32,
             build, s.offer.version[0] ? s.offer.version : "?", s.offer.size);
    emit_event(SIMPLEOTA_EVENT_UPDATE_AVAILABLE, NULL, build, 0, s.offer.size);
    return got_offer;
}

/* ---------------------------------------------------------------------------
 * Apply (download + verify + flash)
 * ------------------------------------------------------------------------- */

/* Shared failure tail: report + event + drop the offer. Assumes any OTA
 * handle and TLS session have already been released. */
static void finish_failed(const char *reason) {
    ESP_LOGE(TAG, "update failed: %s", reason);
    post_status("failed", reason);
    emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, reason, s.offer.build_number, 0, 0);
    clear_offer();
}

/* Convenience: abort the OTA handle, free resources, post failed, clear. */
static void fail_update(esp_ota_handle_t handle, bool ota_started,
                        esp_http_client_handle_t client,
                        sota_sig_verifier_t *verifier, uint8_t *chunk,
                        const char *reason) {
    if (ota_started) esp_ota_abort(handle);
    /* Free the download TLS session BEFORE the status POST (each mbedTLS
     * session costs tens of KB of heap; two at once can exhaust an ESP32.
     * This exact ordering bug lost 'failed' events in the field on the
     * Arduino client). */
    if (client) esp_http_client_cleanup(client);
    free(verifier);
    free(chunk);
    finish_failed(reason);
}

/* Open the download URL, following up to SOTA_MAX_REDIRECTS redirects.
 * Returns NULL after posting the failure. On success the response headers
 * have been fetched and *content_length is set. */
static esp_http_client_handle_t open_download(int64_t *content_length,
                                              char reason_buf[32]) {
    esp_http_client_config_t cfg = {
        .url = s.offer.url,
        .timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        /* Pre-signed URLs are routinely 500-1500+ chars; the default TX
         * buffer (512) cannot even fit the request line. */
        .buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER,
        .keep_alive_enable = true,
    };
    apply_tls_config(&cfg);
    /* Note: NO Authorization header here. The pre-signed URL embeds its own
     * credentials, and the bearer token must not leak to the storage host. */

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        strlcpy(reason_buf, "https_begin_failed", 32);
        return NULL;
    }

    for (int redirects = 0;; ++redirects) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            strlcpy(reason_buf, "https_begin_failed", 32);
            return NULL;
        }
        *content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 ||
            status == 308) {
            /* The open/read streaming API does NOT auto-follow redirects
             * (only perform() does); walk them manually, bounded. */
            if (redirects >= SOTA_MAX_REDIRECTS) {
                esp_http_client_cleanup(client);
                snprintf(reason_buf, 32, "http_status_%d", status);
                return NULL;
            }
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (status != 200) {
            esp_http_client_cleanup(client);
            snprintf(reason_buf, 32, "http_status_%d", status);
            return NULL;
        }
        return client;
    }
}

static void do_apply(void) {
    if (!s.offer.has) return;

    /* ---- Transport guard first (Arduino ordering: a non-HTTPS URL is
     * insecure_url regardless of what the signed gate would say) ---------- */
    if (strncmp(s.offer.url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "refusing non-HTTPS download URL");
        post_status("failed", "insecure_url");
        emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "insecure_url",
                   s.offer.build_number, 0, 0);
        clear_offer();
        return;
    }

    /* ---- Signed-firmware gate (pre-download; fail-closed) --------------- */
    bool device_signed =
        s.security_mode && strcmp(s.security_mode, SIMPLEOTA_SECURITY_SIGNED) == 0;
    sota_signed_gate_t gate =
        sota_signed_gate(device_signed, s.num_keys, s.offer.offer_signed,
                         s.offer.offer_has_signature);

    if (gate == SOTA_GATE_FAIL_CLOSED) {
        ESP_LOGE(TAG,
                 "signed firmware required but the offer carries no usable "
                 "signature; rejecting before download");
        post_status("failed", "signature_invalid");
        emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "signature_invalid",
                   s.offer.build_number, 0, 0);
        clear_offer();
        return;
    }
    if (gate == SOTA_GATE_WARN_UNVERIFIED) {
        ESP_LOGW(TAG,
                 "applying a SIGNED artifact WITHOUT verification: no public "
                 "key is pinned. Pin the project key via "
                 "simpleota_config_t.signing_keys.");
    }

    sota_sig_verifier_t *verifier = NULL;
    if (gate == SOTA_GATE_VERIFY) {
        verifier = calloc(1, sizeof(*verifier));
        if (!verifier) {
            /* Cannot verify -> fail closed, never fall through unverified. */
            post_status("failed", "signature_invalid");
            emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "signature_invalid",
                       s.offer.build_number, 0, 0);
            clear_offer();
            return;
        }
        /* Key selection: exact key-id match wins; otherwise try all pinned
         * keys (rotation windows). */
        uint8_t selected[SOTA_VERIFIER_MAX_KEYS][32];
        uint8_t num_selected = 0;
        if (s.offer.sig_key_id[0]) {
            for (uint8_t i = 0; i < s.num_keys; ++i) {
                if (strcmp(s.key_ids[i], s.offer.sig_key_id) == 0) {
                    memcpy(selected[0], s.keys[i], 32);
                    num_selected = 1;
                    break;
                }
            }
        }
        if (num_selected == 0) {
            for (uint8_t i = 0; i < s.num_keys; ++i) {
                memcpy(selected[i], s.keys[i], 32);
            }
            num_selected = s.num_keys;
        }
        sota_verifier_begin(verifier, s.offer.signature, selected, num_selected);
    }

    post_status("download_started", NULL);

    char reason[32];
    int64_t content_length = 0;
    esp_http_client_handle_t client = open_download(&content_length, reason);
    if (!client) {
        fail_update(0, false, NULL, verifier, NULL, reason);
        return;
    }

    /* ---- Flash target ----------------------------------------------------- */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || !running) {
        fail_update(0, false, client, verifier, NULL, "update_begin_failed");
        return;
    }

    /* Prefer a known size (incremental erase); never OTA_SIZE_UNKNOWN,
     * whose up-front full-partition erase can starve the idle task. */
    size_t expected = 0;
    if (content_length > 0) {
        expected = (size_t)content_length;
    } else if (s.offer.size > 0) {
        expected = s.offer.size;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(
        dst, expected > 0 ? expected : OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        fail_update(0, false, client, verifier, NULL, "update_begin_failed");
        return;
    }

    uint8_t *chunk = malloc(CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK);
    if (!chunk) {
        fail_update(handle, true, client, verifier, NULL, "update_begin_failed");
        return;
    }

    sota_sha256_ctx sha;
    sota_sha256_init(&sha);

    size_t total = 0;
    int64_t last_progress_us = esp_timer_get_time();
    /* Rate-limit progress events: whole-percent steps when the size is
     * known, every 64 KB otherwise. A per-chunk callback (~380 for a
     * typical image) would push its cost onto the user inline with the
     * download. */
    unsigned last_pct = 100;
    size_t last_emit = 0;

    for (;;) {
        int r = esp_http_client_read(client, (char *)chunk,
                                     CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK);
        if (r > 0) {
            err = esp_ota_write(handle, chunk, (size_t)r);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
                fail_update(handle, true, client, verifier, chunk,
                            "update_write_failed");
                return;
            }
            sota_sha256_update(&sha, chunk, (size_t)r);
            if (verifier) sota_verifier_update(verifier, chunk, (size_t)r);
            total += (size_t)r;
            last_progress_us = esp_timer_get_time();
            if (expected > 0) {
                unsigned pct = (unsigned)((uint64_t)total * 100 / expected);
                if (pct != last_pct) {
                    last_pct = pct;
                    emit_event(SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, NULL,
                               s.offer.build_number, total, expected);
                }
            } else if (total - last_emit >= 65536) {
                last_emit = total;
                emit_event(SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, NULL,
                           s.offer.build_number, total, expected);
            }
        } else if (r == 0) {
            /* End of body (or clean connection close). */
            break;
        } else {
            int64_t stalled_us = esp_timer_get_time() - last_progress_us;
            bool stalled =
                stalled_us >= (int64_t)CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS * 1000;
            fail_update(handle, true, client, verifier, chunk,
                        stalled ? "download_stalled" : "stream_error");
            return;
        }
    }

    bool complete = esp_http_client_is_complete_data_received(client);
    /* Rule: the download TLS session dies BEFORE any further status POST. */
    esp_http_client_cleanup(client);
    client = NULL;

    if (!complete || (expected > 0 && total != expected)) {
        ESP_LOGE(TAG, "short read: got %u of %u", (unsigned)total,
                 (unsigned)expected);
        fail_update(handle, true, NULL, verifier, chunk, "short_read");
        return;
    }

    post_status("downloaded", NULL);

    /* ---- Checksum first (cheap transport diagnosis)... ------------------- */
    unsigned char digest[32];
    sota_sha256_final(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    hex[64] = '\0';

    if (strcmp(hex, s.offer.checksum) != 0) {
        ESP_LOGE(TAG, "checksum mismatch: got %s want %s", hex,
                 s.offer.checksum);
        fail_update(handle, true, NULL, verifier, chunk, "checksum_mismatch");
        return;
    }

    /* ---- ...then the signature: intact-but-unapproved bytes. ------------- */
    if (verifier) {
        bool sig_ok = sota_verifier_final(verifier);
        free(verifier);
        verifier = NULL;
        if (!sig_ok) {
            ESP_LOGE(TAG, "Ed25519 signature verification FAILED");
            fail_update(handle, true, NULL, NULL, chunk, "signature_invalid");
            return;
        }
        ESP_LOGI(TAG, "Ed25519 signature verified");
    }
    free(chunk);
    chunk = NULL;

    /* ---- Commit ----------------------------------------------------------- */
    /* NOTE: after esp_ota_end() the handle is consumed; these failure paths
     * must NOT go through fail_update() (whose esp_ota_abort would act on a
     * spent handle). finish_failed() is the shared no-abort tail. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* Includes ESP_ERR_OTA_VALIDATE_FAILED (bad app image; with Secure
         * Boot enabled also a Secure Boot rejection: deliberately a distinct
         * reason from signature_invalid). */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        finish_failed("update_end_failed");
        return;
    }
    err = esp_ota_set_boot_partition(dst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        finish_failed("update_end_failed");
        return;
    }

    post_status("flashed", NULL);

    /* ---- Arm the trial (single NVS commit) -------------------------------- */
    err = sota_nvs_arm_trial(
        s.offer.build_number, s.offer.checksum, s.offer.version,
        (uint32_t)dst->address, (uint32_t)running->address, s.nvs.build,
        s.nvs.hash, s.nvs.version, s.offer.deployment_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist OTA state: %s", esp_err_to_name(err));
    }

    /* Mirror the armed state into RAM. Without this, the deferred-reboot
     * mode (disable_auto_reboot) kept reporting the OLD build on the next
     * check, so the server re-offered the same image and the device
     * re-downloaded and re-flashed it every interval until the app finally
     * rebooted; it also stripped the deployment context from the manual
     * reboot event. (The Arduino client re-reads NVS every check.) */
    s.nvs.prev_part = (uint32_t)running->address;
    s.nvs.new_part = (uint32_t)dst->address;
    s.nvs.prev_build = s.nvs.build;
    strlcpy(s.nvs.prev_hash, s.nvs.hash, sizeof(s.nvs.prev_hash));
    strlcpy(s.nvs.prev_version, s.nvs.version, sizeof(s.nvs.prev_version));
    s.nvs.build = s.offer.build_number;
    strlcpy(s.nvs.hash, s.offer.checksum, sizeof(s.nvs.hash));
    strlcpy(s.nvs.version, s.offer.version, sizeof(s.nvs.version));
    strlcpy(s.nvs.fail_dep, s.offer.deployment_id, sizeof(s.nvs.fail_dep));
    s.nvs.fail_build = s.offer.build_number;
    s.nvs.conf_pend = 1;
    s.nvs.trial = 1;

    post_status("validated", NULL);

    uint32_t new_build = s.offer.build_number;
    ESP_LOGI(TAG, "update to build %" PRIu32 " flashed", new_build);

    if (s.disable_auto_reboot) {
        s.pending_reboot = true;
        emit_event(SIMPLEOTA_EVENT_BEFORE_REBOOT, NULL, new_build, 0, 0);
        clear_offer();
        return;
    }

    emit_event(SIMPLEOTA_EVENT_BEFORE_REBOOT, NULL, new_build, 0, 0);
    post_status("reboot", NULL);
    clear_offer();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* ---------------------------------------------------------------------------
 * Managed task
 * ------------------------------------------------------------------------- */

static void sota_task(void *arg) {
    (void)arg;

    /* An overdue confirm-timeout (fired between init and start, when
     * there was no task to notify) outranks the polite initial delay. */
    if (s.initial_delay_ms && !s.confirm_timeout_fired) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s.initial_delay_ms));
    }

    while (!s.stop_requested) {
        if (s.confirm_timeout_fired) {
            s.confirm_timeout_fired = false;
            do_rollback_from_timeout(); /* usually does not return */
        }

        bool connected = !s.is_connected || s.is_connected(s.user_ctx);
        if (connected) {
            if (do_check() && !s.stop_requested) {
                do_apply(); /* does not return on auto-reboot success */
            }
        } else {
            ESP_LOGD(TAG, "is_connected() false; skipping check");
        }

        /* Trial boots and pending terminal reports poll fast so confirm /
         * rolled_back land promptly; steady state (including the armed-but-
         * not-yet-rebooted window, trial==1 without a live trial) uses the
         * normal check interval. */
        bool fast = s.in_trial || s.nvs.trial == 2 ||
                    (s.nvs.conf_pend && s.nvs.trial == 0);
        uint32_t wait_s =
            fast ? SOTA_TRIAL_RETRY_INTERVAL_S : s.check_interval_s;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
    }

    s.task = NULL;
    xSemaphoreGive(s.stopped_sem);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static void default_device_id(char out[18]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* Byte-identical format to the Arduino client so a device keeps its
     * identity across a framework migration. */
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

esp_err_t simpleota_init(const simpleota_config_t *config) {
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || !token_looks_safe(config->token)) return ESP_ERR_INVALID_ARG;
    if (config->num_signing_keys > SOTA_VERIFIER_MAX_KEYS)
        return ESP_ERR_INVALID_ARG;

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!config->cert_pem) {
        ESP_LOGE(TAG,
                 "no TLS trust anchor: enable CONFIG_MBEDTLS_CERTIFICATE_"
                 "BUNDLE or provide cert_pem");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    memset(&s, 0, sizeof(s));

    /* Pin signing keys first; an unparseable key fails init (fail closed,
     * never continue silently unpinned). */
    for (uint8_t i = 0; i < config->num_signing_keys; ++i) {
        const simpleota_signing_key_t *k = &config->signing_keys[i];
        if (!k->pem || !sota_parse_ed25519_public_key_pem(k->pem, s.keys[i])) {
            ESP_LOGE(TAG, "signing key %u: invalid Ed25519 public key PEM", i);
            memset(&s, 0, sizeof(s));
            return ESP_ERR_INVALID_ARG;
        }
        if (k->key_id) strlcpy(s.key_ids[i], k->key_id, sizeof(s.key_ids[i]));
        s.num_keys++;
    }

    s.token = dup_or_null(config->token);
    s.base_url = dup_or_null(config->base_url ? config->base_url
                                              : SOTA_DEFAULT_BASE_URL);
    if (config->device_id) {
        s.device_id = dup_or_null(config->device_id);
    } else {
        char mac[18];
        default_device_id(mac);
        s.device_id = dup_or_null(mac);
    }
    s.chip_family = dup_or_null(config->chip_family ? config->chip_family
                                                    : CONFIG_IDF_TARGET);
    s.board_id = dup_or_null(config->board_id);
    s.hardware_revision = dup_or_null(config->hardware_revision);
    s.partition_profile = dup_or_null(config->partition_profile);
    s.channel = dup_or_null(config->channel);
    s.labels_json = dup_or_null(config->labels_json);
    s.security_mode = dup_or_null(config->security_mode);
    s.version_label = dup_or_null(config->version_label);
    if (!s.token || !s.base_url || !s.device_id || !s.chip_family) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.nvs_schema_version =
        config->nvs_schema_version ? config->nvs_schema_version : 1;
    s.check_interval_s = config->check_interval_s ? config->check_interval_s
                                                  : SOTA_DEFAULT_CHECK_INTERVAL_S;
    s.initial_delay_ms = config->initial_delay_ms;
    s.confirm_timeout_s = config->confirm_timeout_s
                              ? config->confirm_timeout_s
                              : SOTA_DEFAULT_CONFIRM_TIMEOUT_S;
    if (s.confirm_timeout_s < 10) s.confirm_timeout_s = 10;
    if (s.confirm_timeout_s > 86400) s.confirm_timeout_s = 86400;
    s.manual_confirm = config->manual_confirm;
    s.disable_auto_reboot = config->disable_auto_reboot;
    s.is_connected = config->is_connected;
    s.event_cb = config->event_cb;
    s.user_ctx = config->user_ctx;
    s.cert_pem = dup_or_null(config->cert_pem);
    if (config->cert_pem && !s.cert_pem) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }
    s.crt_bundle_attach = config->crt_bundle_attach;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!s.cert_pem && !s.crt_bundle_attach)
        s.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    s.stopped_sem = xSemaphoreCreateBinary();
    if (!s.stopped_sem) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sota_nvs_load(&s.nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs load failed: %s (continuing fresh)",
                 esp_err_to_name(err));
    }

    s.initialized = true;
    process_boot_validation();

    ESP_LOGI(TAG,
             "init: device=%s chip=%s build=%" PRIu32 " keys=%u interval=%" PRIu32
             "s",
             s.device_id, s.chip_family, s.nvs.build, s.num_keys,
             s.check_interval_s);
    return ESP_OK;
}

esp_err_t simpleota_start(void) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (s.task) return ESP_OK; /* already running */
    s.stop_requested = false;
    BaseType_t ok =
        xTaskCreate(sota_task, "simpleota", CONFIG_SIMPLEOTA_TASK_STACK_SIZE,
                    NULL, CONFIG_SIMPLEOTA_TASK_PRIORITY, &s.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t simpleota_stop(void) {
    if (!s.initialized || !s.task) return ESP_OK;
    s.stop_requested = true;
    xTaskNotifyGive(s.task);
    /* The task exits at a safe point (never mid flash write is interrupted;
     * the loop checks the flag between operations). */
    if (xSemaphoreTake(s.stopped_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: task did not exit within 60 s");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t simpleota_check_now(void) {
    if (!s.initialized || !s.task) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s.task);
    return ESP_OK;
}

esp_err_t simpleota_confirm(void) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (!s.in_trial) return ESP_OK;
    do_confirm();
    /* Nudge the task so the confirmed report posts promptly. */
    if (s.task) xTaskNotifyGive(s.task);
    return ESP_OK;
}

esp_err_t simpleota_reboot_for_update(void) {
    if (!s.initialized || !s.pending_reboot) return ESP_ERR_INVALID_STATE;
    post_status_for("reboot", NULL, s.nvs.fail_dep, s.nvs.fail_build);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK; /* not reached */
}

bool simpleota_is_trial_boot(void) { return s.in_trial; }

uint32_t simpleota_current_build(void) { return s.nvs.build; }

esp_err_t simpleota_deinit(void) {
    if (s.task) simpleota_stop();
    sota_wdt_disarm();
    stop_confirm_timer();
    clear_offer();
    free(s.token);
    free(s.base_url);
    free(s.device_id);
    free(s.chip_family);
    free(s.board_id);
    free(s.hardware_revision);
    free(s.partition_profile);
    free(s.channel);
    free(s.labels_json);
    free(s.security_mode);
    free(s.version_label);
    free(s.cert_pem);
    if (s.stopped_sem) vSemaphoreDelete(s.stopped_sem);
    memset(&s, 0, sizeof(s));
    return ESP_OK;
}
