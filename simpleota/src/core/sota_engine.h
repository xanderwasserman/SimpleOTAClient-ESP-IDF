/**
 * @file
 * @brief The update engine both clients run: boot validation,
 * check-ins, applying an update, the trial and its confirm or rollback,
 * and the deferred reports.
 *
 * The public API layer (port/esp32/simpleota.c) owns a sota_engine_t and
 * calls these functions. A device runs one engine at a time
 * (sota_engine_init()). The engine reaches the platform only through the
 * link-time interfaces sota_kv.h, sota_image.h, sota_boot.h, sota_sys.h,
 * sota_net.h and sota_app.h, which the port implements.
 *
 * Polling use: sota_engine_check(), then sota_engine_apply() when it
 * offers an update. A managed task calls sota_engine_step() and waits the
 * seconds it returns, waking early on sota_app_wake().
 */

#ifndef SOTA_ENGINE_H
#define SOTA_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "sota_protocol.h"
#include "sota_report.h"
#include "sota_signing.h"
#include "sota_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTA_DEFAULT_CHECK_INTERVAL_S 3600u  /**< Seconds between check-ins. */
#define SOTA_DEFAULT_CONFIRM_TIMEOUT_S 300u  /**< Seconds a trial has to confirm. */
#define SOTA_DEFAULT_HTTP_TIMEOUT_MS 15000u  /**< Per-request timeout. */
/** Seconds between check-ins during a trial, and while a report waits for
 *  one of its quick retries. */
#define SOTA_TRIAL_RETRY_INTERVAL_S 10u

/** sota_engine_apply() results, the same values as simpleota_result_t. */
typedef enum {
    SOTA_APPLY_OK = 0,             /**< Flashed; the reboot is next. */
    SOTA_APPLY_CHECKSUM_FAIL = 1,  /**< The download did not match its sha256. */
    SOTA_APPLY_FLASH_FAIL = 2,     /**< Network, URL, flash or state write failure. */
    SOTA_APPLY_NO_OFFER = 3,       /**< No offer from a successful check. */
    SOTA_APPLY_SIGNATURE_FAIL = 4, /**< Required signature missing or wrong. */
    SOTA_APPLY_REBOOT_PENDING = 5, /**< A flashed update waits for its reboot. */
    SOTA_APPLY_UNCONFIRMED = 6,    /**< The running image is on trial; the offer is kept. */
    SOTA_APPLY_DOWNGRADE = 7,      /**< The offer's build is not above the running one. */
} sota_apply_result_t;

/** What sota_engine_confirm() did. */
typedef enum {
    SOTA_CONFIRM_DONE,     /**< The trial is over and its report queued. */
    SOTA_CONFIRM_NO_TRIAL, /**< Nothing to confirm, or a rollback is under way. */
    SOTA_CONFIRM_FAILED,   /**< The bootloader could not record it; the trial goes on. */
} sota_confirm_result_t;

/** Strings are borrowed: they must outlive the engine. */
typedef struct {
    const char *token;          /**< Project or device token. */
    const char *server_url;     /**< NULL: SOTA_DEFAULT_SERVER_URL. */
    sota_device_info_t device;  /**< Sent on every check-in. */
    const char *version_label;  /**< NULL: the stored one. */
    uint32_t image_build;       /**< This image's build, reported when SimpleOTA
                                     did not install it (a USB flash). 0: unknown. */
    uint32_t check_interval_s;  /**< 0: the default. */
    uint32_t confirm_timeout_s; /**< 0: the default. Clamped to [10, 86400]. */
    uint32_t http_timeout_ms;   /**< The transport's; sizes the watchdog margin. */
    bool auto_confirm;          /**< A trial is confirmed by its first successful check-in. */
    bool auto_reboot;           /**< apply() restarts after a successful flash. */
    bool rollback;              /**< Run new images as trials. */
    void *user;                 /**< Passed to sota_net.h and sota_app.h. */
} sota_config_t;

/** One engine's state. Members are for the core, its API layer and its
 *  tests; nothing else should touch them. */
typedef struct sota_engine {
    sota_config_t cfg;          /**< As given to sota_engine_init(). */
    uint8_t keys[SOTA_VERIFIER_MAX_KEYS][32];  /**< Pinned Ed25519 keys. */
    char key_ids[SOTA_VERIFIER_MAX_KEYS][64];  /**< Their key ids, empty when none. */
    uint8_t num_keys;           /**< Keys pinned. */

    sota_state_t st;            /**< As stored, kept current. */
    sota_outbox_t outbox;       /**< Guarded by sota_sys_lock(). */
    uint32_t report_failures;   /**< Report sends with no response this boot. */

    sota_offer_t offer;         /**< offer.url is heap, NULL without an offer. */
    bool has_offer;             /**< offer holds an update from the last check. */
    bool last_check_ok;         /**< The last check-in got a 2xx. */
    bool validated;             /**< sota_engine_validate_boot() has run. */
    bool auth_warned;           /**< A rejected token was logged once. */
    uint32_t refused_build;     /**< The offer last refused as a downgrade, */
    char refused_dep[sizeof(((sota_offer_t *)0)->deployment)]; /**< reported once per boot. */

    void *timer;                /**< The confirm timer, from sota_sys_timer_start(). */
    volatile bool in_trial;     /**< The running image is an unconfirmed trial. */
    volatile bool ending;          /**< A confirm or rollback has claimed the trial. */
    volatile bool timeout_pending; /**< The confirm timeout fired, not handled yet. */
} sota_engine_t;

/** Set up an engine. No storage or network access. False for a token with
 *  control characters, an unusable server URL, or while another engine is
 *  set up: a device has one trial record, so it runs one engine until
 *  sota_engine_deinit(). */
bool sota_engine_init(sota_engine_t *e, const sota_config_t *cfg);

/** Pin an Ed25519 public key (PEM). key_id may be NULL. False when the
 *  PEM does not parse or two keys are pinned already. */
bool sota_engine_add_key(sota_engine_t *e, const char *key_id, const char *pem);

/** Work out what this boot means for a saved trial: start supervising it,
 *  queue its report, or roll back. Runs once; check() and apply() call it. */
void sota_engine_validate_boot(sota_engine_t *e);

/** Check in. True when an update is offered. Sends queued reports first,
 *  except during a trial, and confirms a trial when auto_confirm is set. */
bool sota_engine_check(sota_engine_t *e);

/** Download, verify and flash the offer. Restarts on success when
 *  auto_reboot is set. */
sota_apply_result_t sota_engine_apply(sota_engine_t *e);

/** Confirm the running trial image. */
sota_confirm_result_t sota_engine_confirm(sota_engine_t *e);

/** Called by the port's timer, from any context: note the timeout and
 *  sota_app_wake(). */
void sota_engine_timeout_fired(sota_engine_t *e);

/** Roll back when the confirm timeout has fired. Call from a task that
 *  may block: the managed task, or the API layer after sota_app_wake(). */
void sota_engine_service(sota_engine_t *e);

/** One pass of a managed task: service, then check and apply when
 *  connected. Events report the outcome. Returns the seconds to wait
 *  before the next pass. */
uint32_t sota_engine_step(sota_engine_t *e, bool connected);

/** With auto_reboot off: post "reboot" for the waiting update and restart.
 *  Returns false, without restarting, when no update is waiting. */
bool sota_engine_reboot_for_update(sota_engine_t *e);

/** Stop the timer and the watchdog, free the offer, and let another engine
 *  be set up. */
void sota_engine_deinit(sota_engine_t *e);

/** A flashed update waits for its reboot (for this boot, every engine). */
bool sota_engine_update_waiting(void);

/** For Arduino's verifyRollbackLater(): the running image is on probation
 *  and is a SimpleOTA trial. Reads storage; NVS must be initialised. */
bool sota_engine_hold_pending_image(void);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_ENGINE_H */
