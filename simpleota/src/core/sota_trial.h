/**
 * @file
 * @brief What a boot means for a saved trial record.
 *
 * The trial record uses NVS keys that every release of both clients reads
 * and writes: sota_trial (1 = trial, 3 = a trial whose image has booted,
 * 2 = a rollback happened),
 * sota_new_part (where the update went) and sota_prev_part (the partition
 * the update replaced). Arduino releases up to 0.6.0 write only
 * sota_prev_part; ESP-IDF releases write both. The classifier uses
 * sota_new_part when it is there and falls back to sota_prev_part, so a
 * record from any earlier release of either client reads the way that
 * release meant it.
 *
 * The apply path also writes sota_staged before the boot slot switches and
 * removes it straight after, so a record whose slot switch never happened
 * can be told apart from a rollback. A release that does not know
 * sota_staged ignores it, and a record written without it reads like any
 * other, so updates in either direction between releases read the same way.
 *
 * Any trial value other than 0 and 2 is a trial, so a release that does not
 * know 3 reads it as one.
 *
 * No platform types, so the host tests can include this header.
 */

#ifndef SOTA_TRIAL_H
#define SOTA_TRIAL_H

#include <stdbool.h>
#include <stdint.h>

/** What a boot means for the saved trial record, and what to do about it. */
typedef enum {
    SOTA_BOOT_NONE = 0,       /**< No trial record. */
    SOTA_BOOT_TRIAL,          /**< Running the new image: supervise it. */
    SOTA_BOOT_ROLLED_BACK,    /**< Not on the new image: restore identity, report. */
    SOTA_BOOT_REPORT_PENDING, /**< A previous boot recorded a rollback: report it. */
    SOTA_BOOT_WAITING,        /**< Flashed, not booted yet (deep-sleep wake). */
    SOTA_BOOT_ABANDONED,      /**< The slot switch never happened: drop quietly. */
    SOTA_BOOT_INCOMPLETE,     /**< The record cannot be checked: drop it. */
    SOTA_BOOT_TRIAL_RESET,    /**< The new image reset before it was confirmed: roll back. */
    SOTA_BOOT_ROLLBACK_CUT,   /**< A rollback was saved but its slot switch never happened. */
} sota_boot_outcome_t;

/** sota_trial values. */
enum {
    SOTA_TRIAL_STAGED = 1, /**< The new image has not booted yet. */
    SOTA_TRIAL_ROLLED_BACK = 2, /**< A rollback was decided and saved. */
    SOTA_TRIAL_BOOTED = 3, /**< The new image booted once; any reset ends the trial. */
};

/**
 * sota_staged values. The apply path writes STARTED first, then the
 * previous image's identity, then SNAPSHOT, and only then overwrites the
 * live identity with the new build's.
 */
enum {
    SOTA_STAGED_STARTED = 1,  /**< The record is being written. */
    SOTA_STAGED_SNAPSHOT = 2, /**< The previous image's identity is saved. */
};

/** The trial record and the boot's facts that sota_classify_boot() reads. */
typedef struct {
    uint8_t trial;          /**< sota_trial. */
    uint8_t staged;         /**< sota_staged, 0 when missing. */
    uint32_t new_part;      /**< sota_new_part, 0 when missing. */
    uint32_t prev_part;     /**< sota_prev_part, 0 when missing. */
    uint32_t running_part;  /**< Running partition address, 0 when unknown. */
    uint32_t boot_part;     /**< Partition the next boot selects, 0 when unknown. */
    bool deep_sleep_wake;   /**< This boot is a wake from deep sleep. */
} sota_boot_facts_t;

/** Whether the running image is the update's, from sota_new_part or else
 *  sota_prev_part. False when neither is known. */
static inline bool sota_on_new_image(const sota_boot_facts_t *f)
{
    if (f->new_part != 0) return f->running_part == f->new_part;
    return f->prev_part != 0 && f->running_part != f->prev_part;
}

/** Classify this boot against the trial record. Pure, so the host tests
 *  cover every power-cut point. */
static inline sota_boot_outcome_t sota_classify_boot(const sota_boot_facts_t *f)
{
    /* sota_staged is written first and sota_trial last, so staged with no
     * trial is a record cut short before the slot switch. */
    if (f->trial == 0) return f->staged ? SOTA_BOOT_ABANDONED : SOTA_BOOT_NONE;
    /* A rollback saves sota_trial = 2 before it leaves the new image, so
     * still running that image means power was lost in between. */
    if (f->trial == SOTA_TRIAL_ROLLED_BACK) {
        return f->running_part != 0 && f->prev_part != 0 && sota_on_new_image(f)
                   ? SOTA_BOOT_ROLLBACK_CUT
                   : SOTA_BOOT_REPORT_PENDING;
    }
    if (f->running_part == 0 || (f->new_part == 0 && f->prev_part == 0)) {
        return SOTA_BOOT_INCOMPLETE;
    }
    bool on_new = sota_on_new_image(f);

    /* Like the bootloader's probation, a trial ends at the first reset of
     * the new image, whatever the cause: a crash, a watchdog, a power loss,
     * a restart. Without that, an image that resets before it is confirmed
     * gets a fresh confirm timeout on every boot, and never rolls back.
     * Deep sleep counts too: where the bootloader resumes the image on a
     * wake without validating it (CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_
     * SLEEP), the trial would otherwise outlive any number of wakes. */
    if (on_new) {
        return f->trial == SOTA_TRIAL_BOOTED || f->deep_sleep_wake ? SOTA_BOOT_TRIAL_RESET
                                                                   : SOTA_BOOT_TRIAL;
    }

    /* A deep-sleep wake can resume the old image while the boot slot already
     * points at the new one, so the slot switch did happen. */
    if (f->deep_sleep_wake && f->boot_part != 0 &&
        (f->new_part != 0 ? f->boot_part == f->new_part
                          : f->boot_part != f->running_part)) {
        return SOTA_BOOT_WAITING;
    }
    if (f->staged) return SOTA_BOOT_ABANDONED;
    /* Any other reset that is not on the new image means it did not run,
     * which is a rollback. */
    return SOTA_BOOT_ROLLED_BACK;
}

/**
 * An abandoned record puts the previous identity back only when it was
 * saved in full. Before that, the live identity was not touched.
 */
static inline bool sota_abandon_restores_identity(uint8_t staged)
{
    return staged >= SOTA_STAGED_SNAPSHOT;
}

/**
 * Arduino's verifyRollbackLater(): keep an image the bootloader still has
 * on probation (pending verify) only when it is a SimpleOTA trial. Any
 * other pending image, such as one written by ArduinoOTA, is left for the
 * Arduino core to accept.
 */
static inline bool sota_hold_pending_image(bool image_pending, sota_boot_outcome_t outcome)
{
    return image_pending &&
           (outcome == SOTA_BOOT_TRIAL || outcome == SOTA_BOOT_TRIAL_RESET ||
            outcome == SOTA_BOOT_ROLLBACK_CUT);
}

/** esp_reset_reason_t values used for rollback reasons. */
enum {
    SOTA_RST_POWERON = 1,
    SOTA_RST_SW = 3,
    SOTA_RST_PANIC = 4,
    SOTA_RST_INT_WDT = 5,
    SOTA_RST_TASK_WDT = 6,
    SOTA_RST_WDT = 7,
    SOTA_RST_DEEPSLEEP = 8,
    SOTA_RST_BROWNOUT = 9,
};

/**
 * The reset was a crash or a watchdog. A trial that crashed before it could
 * save its first boot (SOTA_TRIAL_BOOTED), such as in a global constructor,
 * has failed it too. Other resets cannot be told apart from the restart
 * into the update.
 */
static inline bool sota_reset_is_crash(int reset_reason)
{
    return reset_reason == SOTA_RST_PANIC || reset_reason == SOTA_RST_INT_WDT ||
           reset_reason == SOTA_RST_TASK_WDT || reset_reason == SOTA_RST_WDT;
}

/** Reason sent with rolled_back, from the reset that ended the trial. */
static inline const char *sota_rollback_reason(int reset_reason)
{
    switch (reset_reason) {
    case SOTA_RST_PANIC: return "panic";
    case SOTA_RST_INT_WDT: return "int_wdt";
    case SOTA_RST_TASK_WDT: return "task_wdt";
    case SOTA_RST_WDT: return "watchdog";
    case SOTA_RST_BROWNOUT: return "brownout";
    case SOTA_RST_POWERON: return "power_on";
    case SOTA_RST_DEEPSLEEP: return "deep_sleep";
    default: return "reset";
    }
}

#endif /* SOTA_TRIAL_H */
