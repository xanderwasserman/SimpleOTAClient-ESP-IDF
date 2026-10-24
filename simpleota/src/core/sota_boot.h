/**
 * @file
 * @brief Which image runs and which boots next. Implemented by the
 * platform port (esp_ota_* on ESP32) and the host tests' fake.
 */

#ifndef SOTA_BOOT_H
#define SOTA_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Marks a function that does not return, in C and C++. */
#if defined(__cplusplus) && __cplusplus >= 201103L
#define SOTA_NORETURN [[noreturn]]
#elif defined(__GNUC__)
#define SOTA_NORETURN __attribute__((noreturn))
#else
#define SOTA_NORETURN
#endif

/** Hex characters of an image fingerprint. */
#define SOTA_IMAGE_ID_LEN 16u

/** This boot, as the bootloader left it. Addresses are 0 when unknown. */
typedef struct {
    /** Fingerprint of the running image (its ELF SHA-256, in hex), empty
     *  when unknown. Tells an image SimpleOTA installed from one flashed
     *  some other way. */
    char image_id[SOTA_IMAGE_ID_LEN + 1];
    uint32_t running_part; /**< The running slot's flash address. */
    uint32_t boot_part;    /**< The slot the next boot selects. */
    int reset_reason;      /**< esp_reset_reason_t, see SOTA_RST_* in sota_trial.h. */
    bool pending;          /**< The bootloader holds the running image on probation. */
} sota_boot_info_t;

/** Fill info with this boot's facts. */
void sota_boot_info(sota_boot_info_t *info);

/** True when the bootloader has marked the slot at part invalid. */
bool sota_boot_slot_invalid(uint32_t part);

/** Boot the slot at part from the next reset on. */
bool sota_boot_set(uint32_t part);

/** Tell the bootloader the running image is good. True also when it was
 *  not on probation. */
bool sota_boot_mark_valid(void);

/**
 * Leave the running image for the one at prev_part and restart: through
 * the bootloader when it holds the running image on probation, otherwise
 * by switching the slot. Restarts even when a step fails.
 */
SOTA_NORETURN void sota_boot_rollback(uint32_t prev_part);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_BOOT_H */
