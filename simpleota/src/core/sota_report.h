/**
 * @file
 * @brief Delivery and polling policy for deferred status reports.
 *
 * "confirmed" and "rolled_back" are telemetry. What the server says about
 * them never changes what the device does. A report is delivered once the
 * server returns any HTTP response. Without a response it stays pending
 * and is sent again on later checks: a few quick retries, then at the
 * normal check interval.
 *
 * No platform types, so the host tests can include this header.
 */

#ifndef SOTA_REPORT_H
#define SOTA_REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** Quick retries for a pending report before it waits for the check interval. */
#define SOTA_REPORT_QUICK_RETRIES 3u

/** Seconds between is_connected() polls while the network is down. */
#define SOTA_CONNECT_POLL_S 1u

/** A report is delivered once the server returns any HTTP response. */
static inline bool sota_report_delivered(int http_status)
{
    return http_status > 0;
}

/**
 * Seconds until the next check.
 *
 * connected:       is_connected() allowed the check that just ran. While it
 *                  does not, it is polled every SOTA_CONNECT_POLL_S, so the
 *                  check runs as soon as the network is up instead of a
 *                  whole check interval later.
 * in_trial:        a trial is running, so checks come every quick_s until it
 *                  is confirmed or rolled back.
 * report_pending:  a confirmed or rolled_back report is waiting to be sent.
 * report_failures: sends of that report that got no response this boot.
 */
static inline uint32_t sota_poll_interval_s(bool connected, bool in_trial,
                                            bool report_pending,
                                            uint32_t report_failures,
                                            uint32_t quick_s,
                                            uint32_t check_interval_s)
{
    if (!connected) return SOTA_CONNECT_POLL_S;
    if (in_trial) return quick_s;
    if (report_pending && report_failures < SOTA_REPORT_QUICK_RETRIES) return quick_s;
    return check_interval_s;
}

/*
 * Outbox. Once a boot has worked out what happened to a trial, the report
 * moves here and the trial keys are cleared, so a later update never
 * overwrites a report that has not been sent. Stored as one NVS blob. When
 * full, the oldest report makes room for the new one.
 */
#define SOTA_OUTBOX_VERSION 1u /**< Layout of the stored outbox blob. */
#define SOTA_OUTBOX_SLOTS 2u   /**< Reports the outbox holds. */

/** One status report waiting to be sent. Part of the stored blob. */
typedef struct {
    char event[12];      /**< "confirmed" or "rolled_back". */
    char reason[24];     /**< Empty when the event has none. */
    char deployment[64]; /**< The deployment the report is about. */
    uint32_t build;      /**< The build the report is about. */
} sota_report_t;

/** The outbox, oldest report first. Stored as one blob. */
typedef struct {
    uint8_t version;     /**< SOTA_OUTBOX_VERSION. */
    uint8_t count;       /**< Reports in use. */
    sota_report_t reports[SOTA_OUTBOX_SLOTS]; /**< The reports, oldest first. */
} sota_outbox_t;

/** Copy src into a fixed field. Returns false when it does not fit. */
static inline bool sota_copy_field(char *dst, size_t cap, const char *src)
{
    size_t len = src ? strlen(src) : 0;
    if (len >= cap) return false;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/** Empty the outbox. */
static inline void sota_outbox_init(sota_outbox_t *box)
{
    memset(box, 0, sizeof(*box));
    box->version = SOTA_OUTBOX_VERSION;
}

/** True when field has a NUL within cap bytes. */
static inline bool sota_field_terminated(const char *field, size_t cap)
{
    return memchr(field, '\0', cap) != NULL;
}

/** True when a loaded blob has this layout, a count it can hold, and only
 *  reports that sota_outbox_push() could have added. */
static inline bool sota_outbox_valid(const sota_outbox_t *box)
{
    if (box->version != SOTA_OUTBOX_VERSION || box->count > SOTA_OUTBOX_SLOTS) return false;
    for (uint8_t i = 0; i < box->count; ++i) {
        const sota_report_t *r = &box->reports[i];
        if (!sota_field_terminated(r->event, sizeof(r->event)) ||
            !sota_field_terminated(r->reason, sizeof(r->reason)) ||
            !sota_field_terminated(r->deployment, sizeof(r->deployment)) ||
            !r->event[0] || !r->deployment[0] || r->build == 0) {
            return false;
        }
    }
    return true;
}

/** Add a report at the back. One without an event, deployment or build,
 *  or whose fields do not fit, is not added; returns false then. */
static inline bool sota_outbox_push(sota_outbox_t *box, const char *event,
                                    const char *reason, const char *deployment,
                                    uint32_t build)
{
    sota_report_t r;
    memset(&r, 0, sizeof(r));
    if (!event || !event[0] || !deployment || !deployment[0] || build == 0) return false;
    if (!sota_copy_field(r.event, sizeof(r.event), event) ||
        !sota_copy_field(r.reason, sizeof(r.reason), reason) ||
        !sota_copy_field(r.deployment, sizeof(r.deployment), deployment)) {
        return false;
    }
    r.build = build;
    if (box->count == SOTA_OUTBOX_SLOTS) {
        memmove(&box->reports[0], &box->reports[1],
                sizeof(sota_report_t) * (SOTA_OUTBOX_SLOTS - 1));
        box->count--;
    }
    box->reports[box->count++] = r;
    return true;
}

/** Remove the report at the front. */
static inline void sota_outbox_pop(sota_outbox_t *box)
{
    if (box->count == 0) return;
    memmove(&box->reports[0], &box->reports[1],
            sizeof(sota_report_t) * (SOTA_OUTBOX_SLOTS - 1));
    box->count--;
    memset(&box->reports[box->count], 0, sizeof(sota_report_t));
}

#endif /* SOTA_REPORT_H */
