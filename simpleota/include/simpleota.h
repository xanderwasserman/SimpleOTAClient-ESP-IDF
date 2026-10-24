/**
 * @file
 * @brief The SimpleOTA client for ESP32, the same API in the ESP-IDF
 * component and the Arduino library.
 *
 * Thin client for the SimpleOTA firmware update platform
 * (https://simpleota.com). Polls the check endpoint, streams and flashes
 * offered firmware with on-the-fly SHA-256 and Ed25519 signature
 * verification, reports lifecycle events, and runs every new image as a
 * trial that rolls back unless it is confirmed.
 *
 * The application owns network bring-up (Wi-Fi/Ethernet) and decides when
 * the client may talk to the network via the optional is_connected
 * callback; the client does the rest from its own background task
 * (managed mode). In polling mode the application calls simpleota_check()
 * and simpleota_apply() from its own task instead, and confirms a trial with
 * simpleota_confirm(). Each runs on a short-lived task with
 * CONFIG_SIMPLEOTA_TASK_STACK_SIZE while the caller waits, so the caller's
 * stack size does not matter.
 *
 * Quick start:
 * @code
 * simpleota_config_t cfg = {0};      // {} in an Arduino sketch
 * cfg.token = "sota_pk_...";        // project (or device) token
 * cfg.is_connected = wifi_up;       // or start after the IP arrives
 * ESP_ERROR_CHECK(simpleota_init(&cfg));
 * ESP_ERROR_CHECK(simpleota_start());
 * @endcode
 *
 * The device needs a partition table with two OTA app slots, which
 * Arduino-ESP32's default has. On ESP-IDF, rollback by the bootloader needs
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, which Arduino-ESP32 sets. Both
 * ship with the first serial flash and cannot change over OTA. Without
 * bootloader rollback the client rolls a failed trial back itself
 * (CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK): on the confirm timeout, or on any
 * reset before the confirm, as the bootloader would. It cannot catch an
 * image that crashes before simpleota_init() on every boot.
 *
 * The client is a process-wide singleton (one instance per device);
 * a second simpleota_init() returns ESP_ERR_INVALID_STATE.
 *
 * Thread-safety: configure and init from one task. After init the public
 * functions are safe to call from any task, except that polling mode and
 * simpleota_set_server_url() belong to one task of the application's.
 *
 * Logs use the tag "simpleota". Warnings and errors show by default;
 * esp_log_level_set("simpleota", ESP_LOG_DEBUG) after simpleota_init()
 * shows every step.
 *
 * Compatibility, which every later 1.x release keeps:
 * - A configuration field left at zero (or NULL, or false) gets its default.
 *   New fields follow the same rule, so a config filled from `{0}` keeps
 *   building and keeps its behaviour.
 * - Functions, fields, result values and event ids are only added, never
 *   changed or removed; check SIMPLEOTA_API_VERSION before using a newer
 *   one.
 * - The `reason` tokens listed at simpleota_event_t are stable. New tokens
 *   may be added, so treat an unknown one as a generic failure.
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

/**
 * @name API version
 * The version of this API, not of either package: the minor version grows
 * when something is added, the major one only for a breaking change. Both
 * the Arduino library and the ESP-IDF component ship this header unchanged.
 * @code
 * #if SIMPLEOTA_API_VERSION >= SIMPLEOTA_API_VERSION_NUM(1, 1)
 * // use something added in 1.1
 * #endif
 * @endcode
 * @{
 */
#define SIMPLEOTA_API_VERSION_MAJOR 1 /**< Breaking changes. */
#define SIMPLEOTA_API_VERSION_MINOR 0 /**< Additions. */
/** A version as one comparable number. */
#define SIMPLEOTA_API_VERSION_NUM(major, minor) ((major) * 1000 + (minor))
/** This header's version, as SIMPLEOTA_API_VERSION_NUM() gives it. */
#define SIMPLEOTA_API_VERSION \
    SIMPLEOTA_API_VERSION_NUM(SIMPLEOTA_API_VERSION_MAJOR, SIMPLEOTA_API_VERSION_MINOR)
/** @} */

/**
 * The security mode a device reports on check-ins. The server offers it only
 * artifacts of the same mode, or any artifact when it reports none.
 */
typedef enum {
    SIMPLEOTA_SECURITY_NONE = 0, /**< Report none: every artifact matches. */
    SIMPLEOTA_SECURITY_BASIC,    /**< Unsigned artifacts, verified by checksum. */
    SIMPLEOTA_SECURITY_SIGNED,   /**< Signed artifacts only, each verified against a
                                      pinned key before it can boot. Needs at least
                                      one signing key. */
} simpleota_security_t;

/** What simpleota_apply() did. The values are stable across releases. */
typedef enum {
    SIMPLEOTA_OK = 0,             /**< Flashed and verified; the reboot is next. */
    SIMPLEOTA_CHECKSUM_FAIL = 1,  /**< The download did not match its SHA-256. */
    SIMPLEOTA_FLASH_FAIL = 2,     /**< Network, URL, flash or state write failure. */
    SIMPLEOTA_NO_OFFER = 3,       /**< No offer from a successful simpleota_check(). */
    SIMPLEOTA_SIGNATURE_FAIL = 4, /**< Required signature missing or wrong. */
    SIMPLEOTA_REBOOT_PENDING = 5, /**< A flashed update waits for
                                       simpleota_reboot_for_update(). */
    SIMPLEOTA_UNCONFIRMED = 6,    /**< The running image is on trial: confirm it,
                                       then apply again. The offer is kept. */
    SIMPLEOTA_DOWNGRADE = 7,      /**< The offered build is not above the running
                                       one; refused (anti-rollback). */
} simpleota_result_t;

/** Events delivered to simpleota_config_t::event_cb. */
typedef enum {
    SIMPLEOTA_EVENT_CHECK_DONE,        /**< A check completed; no update offered.
                                            reason: the server's status. */
    SIMPLEOTA_EVENT_UPDATE_AVAILABLE,  /**< An update was offered. The managed task
                                            downloads it next, unless a trial runs. */
    SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, /**< bytes_done / bytes_total advanced. */
    SIMPLEOTA_EVENT_UPDATE_FAILED,     /**< Update aborted; reason carries the token. */
    SIMPLEOTA_EVENT_BEFORE_REBOOT,     /**< Flashed + verified. Restart is imminent,
                                            or deferred until
                                            simpleota_reboot_for_update() when
                                            disable_auto_reboot is set. */
    SIMPLEOTA_EVENT_TRIAL_BOOT,        /**< Running an unconfirmed (trial) image. */
    SIMPLEOTA_EVENT_CONFIRMED,         /**< The trial image's confirmed report
                                            was delivered. */
    SIMPLEOTA_EVENT_ROLLING_BACK,      /**< The trial failed; the previous image
                                            starts next. reason says why. */
} simpleota_event_id_t;

/**
 * One lifecycle event, valid only during the event_cb call.
 *
 * `reason` for SIMPLEOTA_EVENT_UPDATE_FAILED, which the dashboard shows too:
 * - `downgrade_refused`: the offered build is not above the running one.
 * - `insecure_url`: the download URL is not https://.
 * - `signature_invalid`: a required signature is missing or wrong.
 * - `https_begin_failed`: the download connection could not be opened.
 * - `http_status_<N>`: the download returned HTTP N, or a fourth redirect.
 * - `redirect_not_https`, `redirect_malformed`: a download redirect led to
 *   plain HTTP, or had no usable Location.
 * - `update_begin_failed`: no second OTA slot, or the image is too big.
 * - `update_write_failed`: a flash write failed.
 * - `stream_error`, `download_stalled`, `short_read`: the download broke,
 *   received nothing for CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS, or ended early.
 * - `checksum_mismatch`: the download did not match its SHA-256.
 * - `update_end_failed`: the image failed ESP-IDF's check, or the boot slot
 *   could not be switched.
 * - `state_save_failed`: the trial record could not be saved.
 *
 * `reason` for SIMPLEOTA_EVENT_ROLLING_BACK: `confirm_timeout`, or the reset
 * that ended the trial: `panic`, `int_wdt`, `task_wdt`, `watchdog`,
 * `brownout`, `power_on`, `deep_sleep` or `reset`.
 *
 * For SIMPLEOTA_EVENT_CHECK_DONE it is the server's status, such as
 * `up_to_date`, which the platform defines.
 */
typedef struct {
    simpleota_event_id_t id; /**< What happened. */
    const char *reason;      /**< A reason token (e.g. "checksum_mismatch"), the
                                  server's status for CHECK_DONE, or NULL. */
    uint32_t build_number;   /**< The build the event is about, 0 when none. */
    size_t bytes_done;       /**< DOWNLOAD_PROGRESS only. */
    size_t bytes_total;      /**< DOWNLOAD_PROGRESS, and UPDATE_AVAILABLE (the
                                  offer's size); 0 when unknown. */
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
    const char *token;             /**< Required. Project token or per-device token. */
    const char *base_url;          /**< Default: "https://simpleota.com". Must be https://
                                        with a host; anything else fails init. */
    const char *device_id;         /**< Default: Wi-Fi STA MAC "aa:bb:cc:dd:ee:ff". */
    const char *chip_family;       /**< Default: CONFIG_IDF_TARGET ("esp32", "esp32s3", ...),
                                        the chip the firmware is built for. */
    const char *board_id;          /**< Board model. Default: left out of check-ins. */
    const char *hardware_revision; /**< Board revision. Default: left out. */
    const char *partition_profile; /**< Partition layout name. Default: left out. */
    const char *channel;           /**< Release channel. Default: left out. */
    const char *labels_json;       /**< Reserved: pre-formed JSON object sent verbatim on
                                        check-ins, but not yet consumed by the platform.
                                        Left out, with a warning, when it is not one
                                        JSON object. */
    simpleota_security_t security_mode; /**< Default: SIMPLEOTA_SECURITY_NONE.
                                             SIGNED needs a signing key. */
    const char *version_label;     /**< Default: the label stored with the update that
                                        installed this image. */
    uint32_t image_build;          /**< This image's build number, reported when
                                        SimpleOTA did not install it (a USB flash).
                                        Default: unknown, and such an image reports 0.
                                        Set it only in the exact image you upload,
                                        to the build SimpleOTA gives it. */
    uint8_t nvs_schema_version;    /**< The data schema this firmware expects. Default: 1. */
    uint32_t check_interval_s;     /**< Seconds between checks. Default: 3600. */
    uint32_t initial_delay_ms;     /**< Delay before the first check after start().
                                        Default: none. Not a wait for the IP; see
                                        is_connected. */
    uint32_t confirm_timeout_s;    /**< Seconds a trial has to confirm. Default: 300.
                                        Clamped to [10, 86400]. */
    bool manual_confirm;           /**< Managed mode: true = the app calls
                                        simpleota_confirm(); false = the first
                                        successful check-in confirms. Polling mode always
                                        needs simpleota_confirm(). */
    bool disable_auto_reboot;      /**< true: the app calls simpleota_reboot_for_update(). */

    /** Pinned public keys for signed firmware, at most 2. Parsed at init,
     *  which fails on a PEM that does not parse rather than run unpinned. */
    const simpleota_signing_key_t *signing_keys;
    uint8_t num_signing_keys;      /**< Entries in signing_keys. */

    /** Return false while the link is down (e.g. no IP yet). It is polled
     *  every second, and the check runs as soon as it returns true.
     *  NULL: always try. start() must then come after the IP arrives, or
     *  the first check fails and the next is check_interval_s later. */
    bool (*is_connected)(void *user_ctx);
    /** Lifecycle events, called on the client's task that runs the update:
     *  the managed task, or a polling call's task while the caller waits.
     *  Keep it quick, and don't call the client from it except
     *  simpleota_reboot_for_update(). */
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;                /**< Passed to is_connected and event_cb. */

    /** TLS trust, always verified. Default: the ESP-IDF certificate bundle,
     *  or on Arduino, which does not ship it, the ISRG Root X1 and X2
     *  certificates that SimpleOTA's hosts chain to. Set cert_pem to pin a
     *  specific CA, or several concatenated, instead (then the bundle is not
     *  used). */
    const char *cert_pem;
    esp_err_t (*crt_bundle_attach)(void *conf); /**< Another bundle's attach function.
                                                     Default: ESP-IDF's bundle. */
} simpleota_config_t;

/**
 * Initialize the client: copy config, apply defaults, parse and pin
 * signing keys, load persisted state, and run boot-time trial validation
 * (detects rollbacks, arms the confirm timeout for trial boots).
 *
 * Call it as early as possible, after nvs_flash_init() (Arduino-ESP32 has
 * already called it): on a trial boot the confirm timeout starts here.
 *
 * @param config  The configuration; copied, so it need not outlive the call.
 * @retval ESP_OK                 Ready.
 * @retval ESP_ERR_INVALID_ARG    A missing token or one with control
 *                                characters, an unparseable signing key,
 *                                a security_mode out of range or SIGNED
 *                                without a key, or a base_url that is not
 *                                https://.
 * @retval ESP_ERR_INVALID_STATE  Already initialized, or neither the
 *                                certificate bundle nor a cert_pem is
 *                                available.
 * @retval ESP_ERR_NO_MEM         Out of memory.
 */
esp_err_t simpleota_init(const simpleota_config_t *config);

/**
 * Start the managed background task: check on the interval, apply what is
 * offered, confirm a trial on its first successful check-in (unless
 * manual_confirm), and post reports. On a trial boot the confirm timeout
 * rolls back also without the task. Calling it again does nothing.
 *
 * @retval ESP_OK                 Running.
 * @retval ESP_ERR_INVALID_STATE  Not initialized.
 * @retval ESP_ERR_NO_MEM         The task could not be created.
 */
esp_err_t simpleota_start(void);

/**
 * Stop the managed task. Waits for its current step, up to 60 s, so a
 * download or flash write is never cut short.
 *
 * @retval ESP_OK           Stopped, or was not running.
 * @retval ESP_ERR_TIMEOUT  The task is still running after 60 s.
 */
esp_err_t simpleota_stop(void);

/**
 * Wake the managed task to check immediately.
 *
 * @retval ESP_OK                 The task was woken.
 * @retval ESP_ERR_INVALID_STATE  The managed task is not running.
 */
esp_err_t simpleota_check_now(void);

/**
 * Polling mode: check in, and keep any offer for simpleota_apply(). Sends
 * queued reports first, except during a trial.
 * @code
 * bool update = false;
 * if (simpleota_check(&update) == ESP_OK && update) simpleota_apply();
 * @endcode
 *
 * @param[out] update_available  Set to true when an update is offered, and
 *                               to false otherwise, whatever is returned.
 * @retval ESP_OK                 The server answered. An offer it sent that
 *                                cannot be used logs a warning and counts as
 *                                no update.
 * @retval ESP_FAIL               No response, or not a 2xx: try again later.
 * @retval ESP_ERR_INVALID_ARG    update_available is NULL.
 * @retval ESP_ERR_INVALID_STATE  Not initialized, or (with a warning) the
 *                                managed task runs.
 * @retval ESP_ERR_NO_MEM         No memory for the task the check runs on.
 */
esp_err_t simpleota_check(bool *update_available);

/**
 * Polling mode: download, verify and flash the offer from the last
 * simpleota_check(), then reboot unless disable_auto_reboot is set.
 *
 * @return What happened. With auto-reboot, a successful apply restarts the
 *         device before it returns. SIMPLEOTA_NO_OFFER, with a warning,
 *         while the managed task runs. SIMPLEOTA_FLASH_FAIL, with an error
 *         and the offer kept, when there is no memory for its task.
 */
simpleota_result_t simpleota_apply(void);

/**
 * Point check-ins and status posts at another SimpleOTA server. Firmware
 * downloads still use the offer's pre-signed URL. Not while
 * simpleota_check() or simpleota_apply() runs on another task.
 *
 * @param url  The new base URL; same rules as base_url: https:// with a host.
 * @retval ESP_OK                 Switched.
 * @retval ESP_ERR_INVALID_ARG    Rejected; the previous URL is kept.
 * @retval ESP_ERR_INVALID_STATE  Before init, or while the managed task runs.
 * @retval ESP_ERR_NO_MEM         Out of memory; the previous URL is kept.
 */
esp_err_t simpleota_set_server_url(const char *url);

/**
 * @return The version label of the last offer, or "" when there is none.
 *         Valid until the next check.
 */
const char *simpleota_last_offered_version(void);

/**
 * Confirm the running trial image, after the app's own health checks pass
 * (manual_confirm, or polling mode). Marks the image valid in the
 * bootloader, cancels the confirm timer, and queues the "confirmed" report.
 *
 * @retval ESP_OK                 Confirmed, or there is no trial.
 * @retval ESP_FAIL               The bootloader could not record the image
 *                                as good; the trial keeps running, so call
 *                                again.
 * @retval ESP_ERR_INVALID_STATE  Not initialized.
 */
esp_err_t simpleota_confirm(void);

/**
 * Reboot into a downloaded update when disable_auto_reboot was set. Until
 * then the client does not download another update, because the next
 * update slot holds the one waiting for this reboot.
 *
 * Stops the background task, posts the "reboot" status from a short-lived
 * task with CONFIG_SIMPLEOTA_TASK_STACK_SIZE (so any caller's stack is
 * enough), and restarts.
 *
 * @retval ESP_ERR_INVALID_STATE  No update is waiting. Otherwise it does
 *                                not return.
 */
esp_err_t simpleota_reboot_for_update(void);

/** @return true while running an unconfirmed trial image. */
bool simpleota_is_trial_boot(void);

/**
 * @return The build number this device reports: 0 before its first update
 *         from SimpleOTA, and before simpleota_init().
 */
uint32_t simpleota_current_build(void);

/**
 * Stop the managed task if needed and release all resources. Mainly for
 * tests.
 *
 * @retval ESP_OK                 Released; simpleota_init() may run again.
 * @retval ESP_ERR_TIMEOUT        The task did not stop; nothing is released.
 * @retval ESP_ERR_INVALID_STATE  Called from event_cb on the managed task.
 */
esp_err_t simpleota_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEOTA_H */
