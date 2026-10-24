/**
 * sota_apply.c - download, verify and flash an offer, then save the trial
 * record and switch the boot slot, in that order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sota_boot.h"
#include "sota_engine_priv.h"
#include "sota_image.h"
#include "sota_net.h"
#include "sota_sha256.h"
#include "sota_sys.h"
#include "sota_url.h"

/* Download buffer, on the heap while an update is applied. The ESP-IDF
 * component sets SOTA_DL_CHUNK from Kconfig; an Arduino build can pass
 * -DCONFIG_SIMPLEOTA_DOWNLOAD_CHUNK, the same option's name. */
#ifndef SOTA_DL_CHUNK
#ifdef CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK
#define SOTA_DL_CHUNK ((unsigned)CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK)
#else
#define SOTA_DL_CHUNK 4096u
#endif
#endif

/* Progress events at whole-percent steps, or every 64 KB when the size is
 * unknown: one per chunk would put hundreds of callbacks into the download. */
#define PROGRESS_STEP_UNKNOWN 65536u

/* What a failure has to release. */
typedef struct {
    sota_sig_verifier_t *verifier;
    uint8_t *chunk;
    bool downloading;
    bool writing;
} job_t;

static sota_apply_result_t give_up(sota_engine_t *e, job_t *job, const char *reason,
                                   sota_apply_result_t result)
{
    if (job->writing) sota_image_abort();
    /* The download's TLS session goes before the status post opens one:
     * two at once can exhaust an ESP32's heap and lose the report. */
    if (job->downloading) sota_net_dl_close(e->cfg.user);
    free(job->verifier);
    free(job->chunk);
    SOTA_LOGE("update failed: %s", reason);
    sota_engine_post_status(e, "failed", reason, e->offer.deployment, e->offer.build);
    sota_engine_emit(e, SOTA_EVENT_UPDATE_FAILED, reason, e->offer.build, 0, 0);
    sota_engine_clear_offer(e);
    return result;
}

/* The key the offer names, when it is pinned; otherwise every pinned key,
 * for rotation windows. */
static bool start_verifier(sota_engine_t *e, job_t *job)
{
    job->verifier = (sota_sig_verifier_t *)malloc(sizeof(*job->verifier));
    if (!job->verifier) return false;
    const uint8_t(*keys)[32] = (const uint8_t(*)[32])e->keys;
    for (uint8_t i = 0; i < e->num_keys && e->offer.key_id[0]; ++i) {
        if (strcmp(e->key_ids[i], e->offer.key_id) == 0) {
            return sota_verifier_begin(job->verifier, e->offer.signature, keys + i, 1);
        }
    }
    return sota_verifier_begin(job->verifier, e->offer.signature, keys, e->num_keys);
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0fu];
    }
    out[2 * n] = '\0';
}

static void progress(sota_engine_t *e, size_t total, size_t expected, size_t *last)
{
    size_t mark = expected ? (size_t)((uint64_t)total * 100u / expected) : total / PROGRESS_STEP_UNKNOWN;
    if (mark == *last) return;
    *last = mark;
    sota_engine_emit(e, SOTA_EVENT_DOWNLOAD_PROGRESS, NULL, e->offer.build, total, expected);
}

/* Stream the image into the slot, hashing and verifying as it goes. Returns
 * NULL, or the reason it failed. */
static const char *download(sota_engine_t *e, job_t *job, size_t expected,
                            sota_sha256_ctx *sha)
{
    size_t total = 0;
    size_t last = (size_t)-1;
    uint32_t last_byte_ms = sota_sys_now_ms();
    for (;;) {
        int r = sota_net_dl_read(e->cfg.user, job->chunk, SOTA_DL_CHUNK);
        if (r == 0) break;
        if (r < 0) {
            bool stalled = sota_sys_now_ms() - last_byte_ms >= e->cfg.http_timeout_ms;
            if (r == SOTA_NET_AGAIN && !stalled) {
                sota_sys_sleep_ms(1);
                continue;
            }
            return stalled ? "download_stalled" : "stream_error";
        }
        last_byte_ms = sota_sys_now_ms();
        if (!sota_image_write(job->chunk, (size_t)r)) return "update_write_failed";
        sota_sha256_update(sha, job->chunk, (size_t)r);
        if (job->verifier) sota_verifier_update(job->verifier, job->chunk, (size_t)r);
        total += (size_t)r;
        progress(e, total, expected, &last);
    }
    return expected && total != expected ? "short_read" : NULL;
}

/* The trial record goes in before the slot switch, so a power cut never
 * leaves a bootable new image without one (sota_trial.h). Without rollback
 * the new identity goes in after it: a cut in between only gets the same
 * update offered again. */
static sota_apply_result_t commit(sota_engine_t *e, uint32_t slot, uint32_t running)
{
    job_t none = {NULL, NULL, false, false};
    sota_identity_t next;
    next.build = e->offer.build;
    memcpy(next.hash, e->offer.checksum, sizeof(next.hash));
    memcpy(next.version, e->offer.version, sizeof(next.version));

    if (e->cfg.rollback &&
        !sota_state_stage_trial(&e->st.live, &next, slot, running, e->offer.deployment)) {
        return give_up(e, &none, "state_save_failed", SOTA_APPLY_FLASH_FAIL);
    }
    if (!sota_boot_set(slot)) {
        if (e->cfg.rollback) {
            sota_state_set_identity(&e->st.live);
            sota_state_drop_trial();
        }
        return give_up(e, &none, "update_end_failed", SOTA_APPLY_FLASH_FAIL);
    }
    sota_engine_waiting = true;
    if (e->cfg.rollback) {
        sota_state_unstage();
    } else if (!sota_state_accept_update(&next, e->offer.deployment)) {
        SOTA_LOGW("new identity not saved");
    }
    sota_engine_reload(e);

    sota_engine_post_status(e, "flashed", NULL, e->offer.deployment, e->offer.build);
    sota_engine_post_status(e, "validated", NULL, e->offer.deployment, e->offer.build);
    /* The offer's version and deployment stay until the next check. */
    free(e->offer.url);
    e->offer.url = NULL;
    e->has_offer = false;
    SOTA_LOGI("build %u flashed", (unsigned)next.build);
    sota_engine_emit(e, SOTA_EVENT_BEFORE_REBOOT, NULL, next.build, 0, 0);
    if (e->cfg.auto_reboot) {
        sota_engine_post_status(e, "reboot", NULL, e->offer.deployment, e->offer.build);
        sota_sys_sleep_ms(100);
        sota_sys_restart();
    }
    return SOTA_APPLY_OK;
}

/*
 * Open the offer's download, following up to SOTA_NET_MAX_REDIRECTS
 * redirects. The transports do not follow them, so every hop is checked
 * here, the same way for both clients: a redirect may only lead to another
 * https:// URL. Returns the final status, with *reason set when a redirect
 * was refused. e->offer.url ends up as the URL that was opened.
 */
static int open_download(sota_engine_t *e, int64_t *len, const char **reason)
{
    size_t cap = e->offer.url_cap;
    char *loc = (char *)malloc(2 * cap);
    if (!loc) return 0;
    char *next = loc + cap;
    int status = 0;
    for (int hops = 0;; ++hops) {
        loc[0] = '\0';
        status = sota_net_dl_open(e->cfg.user, e->offer.url, len, loc, cap);
        if (!sota_net_is_redirect(status) || hops == SOTA_NET_MAX_REDIRECTS) break;
        sota_url_status_t st = sota_redirect_target(e->offer.url, loc, next, cap);
        if (st != SOTA_URL_OK) {
            *reason = st == SOTA_URL_NOT_HTTPS ? "redirect_not_https" : "redirect_malformed";
            break;
        }
        memcpy(e->offer.url, next, strlen(next) + 1);
    }
    free(loc);
    return status;
}

sota_apply_result_t sota_engine_apply(sota_engine_t *e)
{
    sota_engine_validate_boot(e);
    /* The next slot holds the update waiting for its reboot. */
    if (sota_engine_waiting) return SOTA_APPLY_REBOOT_PENDING;
    /* The other slot holds the image a running trial may return to. */
    if (e->in_trial) return SOTA_APPLY_UNCONFIRMED;
    if (!e->has_offer) return SOTA_APPLY_NO_OFFER;

    job_t job = {NULL, NULL, false, false};
    /* An old build may carry fixed bugs, and its signature still verifies.
     * To go back, release the old code under a new, higher build number. */
    if (e->offer.build <= e->st.live.build) {
        /* The server offers it again on every check until the deployment
         * changes, so it is reported once. */
        if (e->offer.build == e->refused_build && !strcmp(e->offer.deployment, e->refused_dep)) {
            SOTA_LOGD("build %u refused again", (unsigned)e->offer.build);
            sota_engine_clear_offer(e);
            return SOTA_APPLY_DOWNGRADE;
        }
        e->refused_build = e->offer.build;
        memcpy(e->refused_dep, e->offer.deployment, sizeof(e->refused_dep));
        return give_up(e, &job, "downgrade_refused", SOTA_APPLY_DOWNGRADE);
    }
    if (!sota_url_has_scheme(e->offer.url, "https://") || !sota_url_chars_ok(e->offer.url)) {
        return give_up(e, &job, "insecure_url", SOTA_APPLY_FLASH_FAIL);
    }

    const char *mode = e->cfg.device.security_mode;
    switch (sota_signed_gate(mode && strcmp(mode, "signed") == 0, e->num_keys,
                             e->offer.signed_offer, e->offer.has_signature)) {
    case SOTA_GATE_FAIL_CLOSED:
        return give_up(e, &job, "signature_invalid", SOTA_APPLY_SIGNATURE_FAIL);
    case SOTA_GATE_WARN_UNVERIFIED:
        SOTA_LOGW("signed update applied WITHOUT verification: no key is pinned");
        break;
    case SOTA_GATE_VERIFY:
        if (!start_verifier(e, &job)) {
            return give_up(e, &job, "signature_invalid", SOTA_APPLY_SIGNATURE_FAIL);
        }
        break;
    case SOTA_GATE_SKIP:
        break;
    }

    sota_engine_post_status(e, "download_started", NULL, e->offer.deployment, e->offer.build);
    int64_t len = -1;
    const char *refused = NULL;
    int http = open_download(e, &len, &refused);
    if (refused) return give_up(e, &job, refused, SOTA_APPLY_FLASH_FAIL);
    if (http != 200) {
        char reason[24];
        if (http == 0) {
            strcpy(reason, "https_begin_failed");
        } else {
            snprintf(reason, sizeof(reason), "http_status_%d", http);
        }
        return give_up(e, &job, reason, SOTA_APPLY_FLASH_FAIL);
    }
    job.downloading = true;

    size_t expected = len > 0 ? (size_t)len : e->offer.size;
    uint32_t slot = 0, running = 0;
    job.writing = sota_image_begin(expected, &slot, &running);
    job.chunk = (uint8_t *)malloc(SOTA_DL_CHUNK);
    if (!job.writing || !job.chunk) {
        return give_up(e, &job, "update_begin_failed", SOTA_APPLY_FLASH_FAIL);
    }

    sota_sha256_ctx sha;
    sota_sha256_init(&sha);
    const char *failed = download(e, &job, expected, &sha);
    sota_net_dl_close(e->cfg.user);
    job.downloading = false;
    if (failed) return give_up(e, &job, failed, SOTA_APPLY_FLASH_FAIL);
    sota_engine_post_status(e, "downloaded", NULL, e->offer.deployment, e->offer.build);

    /* The checksum first, which tells a broken transfer apart; then the
     * signature, which fails only for intact bytes nobody approved. */
    uint8_t digest[32];
    char hex[65];
    sota_sha256_final(&sha, digest);
    to_hex(digest, sizeof(digest), hex);
    if (strcmp(hex, e->offer.checksum) != 0) {
        return give_up(e, &job, "checksum_mismatch", SOTA_APPLY_CHECKSUM_FAIL);
    }
    if (job.verifier && !sota_verifier_final(job.verifier)) {
        return give_up(e, &job, "signature_invalid", SOTA_APPLY_SIGNATURE_FAIL);
    }
    free(job.verifier);
    free(job.chunk);
    job.verifier = NULL;
    job.chunk = NULL;

    job.writing = false;
    if (!sota_image_finish()) return give_up(e, &job, "update_end_failed", SOTA_APPLY_FLASH_FAIL);
    return commit(e, slot, running);
}
