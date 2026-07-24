/**
 * simpleota.h - SimpleOTA managed OTA client for ESP-IDF.
 *
 * Thin client for the SimpleOTA firmware update platform
 * (https://simpleota.com). Polls the check endpoint, streams and flashes
 * offered firmware with on-the-fly SHA-256 and Ed25519 signature
 * verification, reports lifecycle events, and integrates with the ESP-IDF
 * bootloader's app-rollback support for trial installs.
 *
 * The application owns network bring-up (Wi-Fi/Ethernet) and decides when
 * the component may talk to the network via the optional is_connected
 * callback; the component does the rest from its own background task.
 *
 * Quick start:
 *
 *     simpleota_config_t cfg = {
 *         .token = "sota_pk_...",       // project (or device) token
 *     };
 *     ESP_ERROR_CHECK(simpleota_init(&cfg));
 *     ESP_ERROR_CHECK(simpleota_start());
 *
 * Rollback-aware trial installs additionally require, in the PROJECT's
 * sdkconfig (both must ship with the first serial flash; they cannot be
 * enabled over OTA):
 *
 *     CONFIG_PARTITION_TABLE_TWO_OTA=y
 *     CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
 *
 * Without them the component still updates firmware, but without automatic
 * rollback: images are confirmed on the first successful check-in after
 * boot.
 *
 * The component is a process-wide singleton (one instance per device);
 * a second simpleota_init() returns ESP_ERR_INVALID_STATE.
 *
 * Thread-safety: configure and init from one task. The public functions are
 * safe to call from any task after init.
 */

#ifndef SIMPLEOTA_H
#define SIMPLEOTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Security modes, as understood by the SimpleOTA platform. */
#define SIMPLEOTA_SECURITY_BASIC "basic"
#define SIMPLEOTA_SECURITY_TOKEN "token"
#define SIMPLEOTA_SECURITY_SIGNED "signed"

/** Events delivered to the config's event_cb (from the component task). */
typedef enum {
    SIMPLEOTA_EVENT_CHECK_DONE,        /**< A check completed; no update pending. */
    SIMPLEOTA_EVENT_UPDATE_AVAILABLE,  /**< An update was offered; download starts next. */
    SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, /**< bytes_done / bytes_total advanced. */
    SIMPLEOTA_EVENT_UPDATE_FAILED,     /**< Update aborted; reason carries the token. */
    SIMPLEOTA_EVENT_BEFORE_REBOOT,     /**< Flashed + verified. Restart is imminent,
                                            or deferred until
                                            simpleota_reboot_for_update() when
                                            disable_auto_reboot is set. */
    SIMPLEOTA_EVENT_TRIAL_BOOT,        /**< Running an unconfirmed (trial) image. */
    SIMPLEOTA_EVENT_CONFIRMED,         /**< Trial image confirmed good. */
    SIMPLEOTA_EVENT_ROLLING_BACK,      /**< Confirm timeout hit; rolling back now. */
} simpleota_event_id_t;

typedef struct {
    simpleota_event_id_t id;
    const char *reason;   /**< Failure reason token (e.g. "checksum_mismatch") or NULL. */
    uint32_t build_number;/**< Build number the event refers to (0 if n/a). */
    size_t bytes_done;    /**< DOWNLOAD_PROGRESS only. */
    size_t bytes_total;   /**< DOWNLOAD_PROGRESS only; 0 when unknown. */
} simpleota_event_t;

/** A pinned Ed25519 public key for signed-firmware verification. */
typedef struct {
    const char *key_id; /**< The SimpleOTA key id (e.g. "prod-2026"), or NULL. */
    const char *pem;    /**< PEM "BEGIN PUBLIC KEY" text from the dashboard. */
} simpleota_signing_key_t;

/**
 * Configuration. Zero-initialize, then set at least .token. All strings are
 * copied at init; the struct and its strings need not outlive the call.
 */
typedef struct {
    const char *token;             /**< REQUIRED. Project token or per-device token. */
    const char *base_url;          /**< Default "https://simpleota.com". No trailing slash. */
    const char *device_id;         /**< Default: Wi-Fi STA MAC "aa:bb:cc:dd:ee:ff". */
    const char *chip_family;       /**< Default: CONFIG_IDF_TARGET ("esp32", "esp32s3", ...). */
    const char *board_id;          /**< NULL = omitted from check-ins. */
    const char *hardware_revision; /**< NULL = omitted. */
    const char *partition_profile; /**< NULL = omitted. */
    const char *channel;           /**< Release channel; NULL = omitted. */
    const char *labels_json;       /**< Reserved: pre-formed JSON object sent verbatim on
                                        check-ins, but not yet consumed by the platform. */
    const char *security_mode;     /**< SIMPLEOTA_SECURITY_* or NULL. */
    const char *version_label;     /**< NULL = last persisted value from NVS. */
    uint8_t nvs_schema_version;    /**< 0 => 1. */
    uint32_t check_interval_s;     /**< 0 => 3600. */
    uint32_t initial_delay_ms;     /**< Delay before the first check after start(). */
    uint32_t confirm_timeout_s;    /**< 0 => 300. Clamped to [10, 86400]. */
    bool manual_confirm;           /**< true: app must call simpleota_confirm(). */
    bool disable_auto_reboot;      /**< true: app calls simpleota_reboot_for_update(). */

    /** Pinned public keys for signed firmware (0..2; parsed at init,
     *  init fails on an unparseable PEM rather than continuing unpinned). */
    const simpleota_signing_key_t *signing_keys;
    uint8_t num_signing_keys;

    /** Return false to postpone network activity (e.g. Wi-Fi down). NULL = always try. */
    bool (*is_connected)(void *user_ctx);
    /** Lifecycle events; called from the component task. Keep it quick. */
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;

    /** TLS trust: default is the ESP-IDF certificate bundle. Set cert_pem to
     *  pin a specific CA instead (then the bundle is not used). */
    const char *cert_pem;
    esp_err_t (*crt_bundle_attach)(void *conf); /**< Override bundle fn; NULL = default. */
} simpleota_config_t;

/**
 * Initialize the component: copy config, apply defaults, parse and pin
 * signing keys, load persisted state, and run boot-time trial validation
 * (detects rollbacks, arms the confirm timeout for trial boots).
 *
 * Call AFTER nvs_flash_init(). Returns ESP_ERR_INVALID_ARG for a missing
 * token, an unparseable signing key, or a token containing control
 * characters; ESP_ERR_INVALID_STATE when already initialized or when
 * neither the certificate bundle nor a cert_pem is available.
 */
esp_err_t simpleota_init(const simpleota_config_t *config);

/** Start the managed background task (check on interval, apply, report).
 *  Call promptly after simpleota_init(): on a trial boot the confirm-timeout
 *  rollback is executed by this task, so supervision is inactive until it
 *  runs. */
esp_err_t simpleota_start(void);

/** Stop the managed task. Blocks briefly; never interrupts a flash write. */
esp_err_t simpleota_stop(void);

/** Wake the managed task to check immediately. */
esp_err_t simpleota_check_now(void);

/**
 * Confirm the currently running trial image (manual_confirm mode, called
 * after the app's own health checks pass). Cancels rollback, reports
 * "confirmed". No-op when not in a trial boot.
 */
esp_err_t simpleota_confirm(void);

/** Reboot into a downloaded update when disable_auto_reboot was set. */
esp_err_t simpleota_reboot_for_update(void);

/** True while running an unconfirmed trial image. */
bool simpleota_is_trial_boot(void);

/** The build number this device currently reports (0 = none yet). */
uint32_t simpleota_current_build(void);

/** Stop (if needed) and release all resources. Mainly for tests. */
esp_err_t simpleota_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEOTA_H */
