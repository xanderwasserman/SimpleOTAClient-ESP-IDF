/**
 * sota_nvs.h - NVS persistence for the SimpleOTA component.
 *
 * Namespace "simpleota". Key names and semantics mirror
 * SimpleOTAClient-Arduino v0.4.0 (all keys are 15 characters or fewer, the
 * NVS limit):
 *
 *   sota_build      u32   build number of the running (committed) image
 *   sota_hash       str   sha256 hex of the running image
 *   sota_ver        str   version label of the running image
 *   sota_trial      u8    0 steady / 1 trial armed / 2 rolled-back, report pending
 *   sota_new_part   u32   flash address of the partition the new image went to
 *   sota_prev_part  u32   flash address of the partition we came from
 *   sota_prev_build u32   pre-OTA build number (snapshot for rollback restore)
 *   sota_prev_hash  str   pre-OTA hash
 *   sota_prev_ver   str   pre-OTA version label
 *   sota_fail_dep   str   deployment id the deferred confirmed/rolled_back
 *                         report refers to
 *   sota_fail_build u32   build number for that report
 *   sota_conf_pend  u8    1 when a "confirmed" report has not been accepted yet
 *   sota_rb_reason  str   reason token recorded when a rollback is detected
 *                         ("confirm_timeout", "watchdog", or "boot_failed");
 *                         reported with the deferred rolled_back event
 */

#ifndef SOTA_NVS_H
#define SOTA_NVS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTA_NVS_STR_MAX 96 /* bounds for hash (64 hex) / version / deployment id */

/** Everything the component persists, read in one shot at init. */
typedef struct {
    uint32_t build;
    char hash[SOTA_NVS_STR_MAX];
    char version[SOTA_NVS_STR_MAX];
    uint8_t trial; /* 0 / 1 / 2 */
    uint32_t new_part;
    uint32_t prev_part;
    uint32_t prev_build;
    char prev_hash[SOTA_NVS_STR_MAX];
    char prev_version[SOTA_NVS_STR_MAX];
    char fail_dep[SOTA_NVS_STR_MAX];
    uint32_t fail_build;
    uint8_t conf_pend;
    char rb_reason[24];
} sota_nvs_state_t;

/** Load all persisted state; missing keys yield zeros/empty strings.
 *  Returns ESP_OK also for a fresh device with no namespace yet. */
esp_err_t sota_nvs_load(sota_nvs_state_t *out);

/**
 * Arm a trial install in ONE commit (power-loss atomicity): snapshots the
 * outgoing image identity (prev_*), records the new image identity as the
 * live one (build/hash/version), the partition addresses, the deferred
 * report context (fail_dep/fail_build, conf_pend=1), and trial=1.
 */
esp_err_t sota_nvs_arm_trial(uint32_t new_build, const char *new_hash,
                             const char *new_version, uint32_t new_part,
                             uint32_t prev_part, uint32_t prev_build,
                             const char *prev_hash, const char *prev_version,
                             const char *deployment_id);

/**
 * Roll the live identity back to the prev_* snapshot and set trial=2
 * (rolled-back, report pending). Restores UNCONDITIONALLY: prev_build==0 and
 * empty strings are valid fresh-device state (restoring only-when-nonzero
 * was a real Arduino bug that left rolled-back devices reporting the new
 * build forever, so the server never re-offered anything).
 */
esp_err_t sota_nvs_rollback_restore(const sota_nvs_state_t *snapshot,
                                    const char *reason);

/** Clear trial bookkeeping (trial flag, partition addrs, prev_* snapshot)
 *  but deliberately KEEP fail_dep/fail_build/conf_pend: those persist until
 *  the server accepts the deferred report. */
esp_err_t sota_nvs_clear_trial(void);

/** Clear the deferred-report context after the server accepted it. */
esp_err_t sota_nvs_clear_report(void);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_NVS_H */
