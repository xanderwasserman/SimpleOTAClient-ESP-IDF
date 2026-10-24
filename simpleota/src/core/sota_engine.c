/**
 * sota_engine.c - boot validation, the trial, check-ins and reports. The
 * apply pipeline is in sota_apply.c.
 */

#include "sota_engine.h"

#include <stdlib.h>
#include <string.h>

#include "sota_boot.h"
#include "sota_engine_priv.h"
#include "sota_json.h"
#include "sota_net.h"
#include "sota_sys.h"
#include "sota_trial.h"
#include "sota_url.h"
#include "sota_wdt_calc.h"

#define CHECK_BODY_CAP 1024u
#define CHECK_RESP_CAP 4096u
#define OFFER_URL_CAP 2048u
#define STATUS_BODY_CAP 384u
#define RETRY_DELAY_MS 2000u

bool sota_engine_waiting;

/* No control characters: the token goes into a header. */
static bool token_ok(const char *t)
{
    if (!t || !t[0]) return false;
    for (; *t; ++t) {
        unsigned char c = (unsigned char)*t;
        if (c < 0x20u || c == 0x7fu) return false;
    }
    return true;
}

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The trial record and the boot slot belong to the device, so one engine
 * runs them. A second would start its own trial and timer for the same
 * record, and could flash while the first one confirms. */
static sota_engine_t *s_owner;

/* An engine whose set-up failed does nothing: it never loads the record,
 * so it can neither start a trial, nor confirm, flash or report. */
static bool running(const sota_engine_t *e)
{
    return e == s_owner;
}

bool sota_engine_init(sota_engine_t *e, const sota_config_t *cfg)
{
    memset(e, 0, sizeof(*e));
    if (s_owner && s_owner != e) {
        SOTA_LOGE("another SimpleOTA client is already running");
        return false;
    }
    e->cfg = *cfg;
    sota_outbox_init(&e->outbox);
    if (!e->cfg.server_url) e->cfg.server_url = SOTA_DEFAULT_SERVER_URL;
    if (!e->cfg.check_interval_s) e->cfg.check_interval_s = SOTA_DEFAULT_CHECK_INTERVAL_S;
    if (!e->cfg.confirm_timeout_s) e->cfg.confirm_timeout_s = SOTA_DEFAULT_CONFIRM_TIMEOUT_S;
    /* Below 10 s leaves no time to debug a trial on a serial console; above
     * 24 h is a bug, not a configuration. */
    e->cfg.confirm_timeout_s = clamp(e->cfg.confirm_timeout_s, 10u, 86400u);
    if (!e->cfg.http_timeout_ms) e->cfg.http_timeout_ms = SOTA_DEFAULT_HTTP_TIMEOUT_MS;
    if (!token_ok(e->cfg.token) || sota_validate_server_url(e->cfg.server_url) != SOTA_URL_OK) {
        return false;
    }
    s_owner = e;
    return true;
}

bool sota_engine_add_key(sota_engine_t *e, const char *key_id, const char *pem)
{
    if (e->num_keys >= SOTA_VERIFIER_MAX_KEYS ||
        !sota_parse_ed25519_public_key_pem(pem, e->keys[e->num_keys]) ||
        !sota_copy_field(e->key_ids[e->num_keys], sizeof(e->key_ids[0]), key_id)) {
        return false;
    }
    e->num_keys++;
    return true;
}

void sota_engine_emit(sota_engine_t *e, sota_event_id_t id, const char *reason,
                      uint32_t build, size_t done, size_t total)
{
    sota_event_t ev = {id, reason, build, done, total};
    sota_app_event(e->cfg.user, &ev);
}

void sota_engine_reload(sota_engine_t *e)
{
    sota_state_load(&e->st);
}

void sota_engine_clear_offer(sota_engine_t *e)
{
    free(e->offer.url);
    memset(&e->offer, 0, sizeof(e->offer));
    e->has_offer = false;
}

/* One attempt during a trial: the confirm timeout's rollback may be
 * waiting behind this request, and the watchdog margin covers one. */
static int post(sota_engine_t *e, const char *path, const char *body, char *resp, size_t cap)
{
    char url[SOTA_SERVER_URL_MAX_LEN + 32u];
    if (!sota_build_api_url(e->cfg.server_url, path, url, sizeof(url))) return -1;
    int attempts = e->in_trial ? 1 : 2;
    for (int i = 0; i < attempts; ++i) {
        if (i) sota_sys_sleep_ms(RETRY_DELAY_MS);
        int status = sota_net_post(e->cfg.user, url, e->cfg.token, body, resp, cap);
        if (status <= 0) continue;
        if ((status == 401 || status == 403) && !e->auth_warned) {
            e->auth_warned = true;
            SOTA_LOGW("server rejected the token (HTTP %d)", status);
        }
        return status;
    }
    return -1;
}

int sota_engine_post_status(sota_engine_t *e, const char *event, const char *reason,
                            const char *deployment, uint32_t build)
{
    char body[STATUS_BODY_CAP];
    if (!deployment || !deployment[0] ||
        !sota_protocol_status_body(e->cfg.device.device_id, deployment, event, build, reason,
                                   body, sizeof(body))) {
        return -1;
    }
    SOTA_LOGI("status %s build %u%s%s", event, (unsigned)build, reason ? " " : "",
              reason ? reason : "");
    return post(e, SOTA_PATH_STATUS, body, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Reports (sota_report.h): queued once a boot knows how a trial ended, sent
 * at the start of later check-ins until the server answers.
 * ------------------------------------------------------------------------- */

static void queue_report(sota_engine_t *e, const char *event, const char *reason,
                         const char *deployment, uint32_t build)
{
    sota_sys_lock();
    bool queued = sota_outbox_push(&e->outbox, event, reason, deployment, build);
    if (queued) sota_state_save_outbox(&e->outbox);
    sota_sys_unlock();
    if (!queued) SOTA_LOGW("%s report has no deployment; dropped", event);
}

/* The lock is not held while posting: the report is copied out, and popped
 * only when a report queued meanwhile has not pushed it out already. */
static void send_reports(sota_engine_t *e)
{
    for (;;) {
        sota_report_t r;
        sota_sys_lock();
        bool have = e->outbox.count > 0;
        if (have) r = e->outbox.reports[0];
        sota_sys_unlock();
        if (!have) return;

        int status = sota_engine_post_status(e, r.event, r.reason[0] ? r.reason : NULL,
                                             r.deployment, r.build);
        if (!sota_report_delivered(status)) {
            e->report_failures++;
            return;
        }
        e->report_failures = 0;
        sota_sys_lock();
        if (e->outbox.count > 0 && memcmp(&e->outbox.reports[0], &r, sizeof(r)) == 0) {
            sota_outbox_pop(&e->outbox);
            sota_state_save_outbox(&e->outbox);
        }
        sota_sys_unlock();
        if (strcmp(r.event, "confirmed") == 0) {
            sota_engine_emit(e, SOTA_EVENT_CONFIRMED, NULL, r.build, 0, 0);
        }
    }
}

/* The record's update is accepted: queue its confirmed report and drop it. */
static void accept_record(sota_engine_t *e)
{
    queue_report(e, "confirmed", NULL, e->st.deployment, e->st.report_build);
    sota_state_drop_trial();
    sota_engine_reload(e);
}

/* The record ended in a rollback: report it and drop it. */
static void report_rollback(sota_engine_t *e, const char *reason)
{
    queue_report(e, "rolled_back", reason, e->st.deployment, e->st.report_build);
    sota_state_drop_trial();
    sota_engine_reload(e);
}

/* ---------------------------------------------------------------------------
 * The trial. A confirm and the confirm timeout can race: whichever claims
 * the trial first ends it.
 * ------------------------------------------------------------------------- */

static bool claim_trial(sota_engine_t *e)
{
    sota_sys_lock();
    bool claimed = e->in_trial && !e->ending;
    if (claimed) e->ending = true;
    sota_sys_unlock();
    return claimed;
}

/* The previous identity goes live and the next boot reports the rollback.
 * A step that fails still ends in the restart: a broken image left running
 * as an endless trial would be worse than a partial restore. */
SOTA_NORETURN static void roll_back(sota_engine_t *e, const char *reason)
{
    SOTA_LOGW("%s: rolling back to build %u", reason, (unsigned)e->st.prev.build);
    sota_engine_emit(e, SOTA_EVENT_ROLLING_BACK, reason, e->st.prev.build, 0, 0);
    sota_sys_timer_stop(&e->timer);
    if (!sota_state_mark_rolled_back(&e->st, reason)) SOTA_LOGE("rollback state not saved");
    sota_boot_rollback(e->st.prev_part);
}

static void start_trial(sota_engine_t *e, bool staged)
{
    /* Power was lost after the slot switch, before sota_staged was removed. */
    if (staged) sota_state_unstage();
    if (!e->cfg.rollback) {
        SOTA_LOGI("rollback disabled; accepting build %u", (unsigned)e->st.live.build);
        sota_boot_mark_valid();
        accept_record(e);
        return;
    }
    /* From here on, any reset of this image ends the trial in a rollback. */
    if (!sota_state_mark_booted()) SOTA_LOGW("trial boot not saved");
    e->in_trial = true;
    /* A hang that stops the confirm timer still resets the chip: the
     * bootloader then returns to the previous image, or this function's
     * caller rolls back a trial that booted after a watchdog. */
    if (sota_sys_wdt_arm(e->cfg.confirm_timeout_s, sota_wdt_floor_sec(e->cfg.http_timeout_ms))) {
        SOTA_LOGW("confirm timeout is longer than the chip watchdog can wait");
    }
    if (!sota_sys_timer_start(&e->timer, e->cfg.confirm_timeout_s, e)) {
        SOTA_LOGE("no confirm timer");
        e->ending = true;
        roll_back(e, "confirm_timeout");
    }
    SOTA_LOGI("trial of build %u; confirm within %u s", (unsigned)e->st.live.build,
              (unsigned)e->cfg.confirm_timeout_s);
    sota_engine_emit(e, SOTA_EVENT_TRIAL_BOOT, NULL, e->st.live.build, 0, 0);
}

/* Why a trial that is not on its new image ended: the reason a rollback
 * saved, then a slot the bootloader marked invalid, then the reset. */
static const char *rollback_cause(const sota_engine_t *e, int reset_reason)
{
    if (e->st.rb_reason[0]) return e->st.rb_reason;
    if (e->st.new_part && sota_boot_slot_invalid(e->st.new_part)) return "confirm_timeout";
    return sota_rollback_reason(reset_reason);
}

static sota_boot_facts_t boot_facts(const sota_state_t *st, const sota_boot_info_t *info)
{
    sota_boot_facts_t f = {
        st->trial, st->staged, st->new_part, st->prev_part,
        info->running_part, info->boot_part, info->reset_reason == SOTA_RST_DEEPSLEEP,
    };
    return f;
}

typedef char sota_image_id_fits_state[
    sizeof(((sota_state_t *)0)->image_id) == SOTA_IMAGE_ID_LEN + 1 ? 1 : -1];

/* Note the running image. One that changed with no update to account for
 * it came another way: a serial flash, perhaps of another project, so the
 * stored build no longer describes it. It reports the build it declares,
 * or 0: a stored build above its own would refuse every offer. */
static void note_image(sota_engine_t *e, const sota_boot_info_t *info,
                       sota_boot_outcome_t outcome)
{
    if (!info->image_id[0] || !strcmp(info->image_id, e->st.image_id)) return;
    if (outcome == SOTA_BOOT_NONE && !e->st.conf_pend) {
        sota_identity_t id = {e->cfg.image_build, "", ""};
        SOTA_LOGW("running an image SimpleOTA did not install; reporting build %u",
                  (unsigned)id.build);
        sota_state_set_identity(&id);
    }
    sota_state_set_image_id(info->image_id);
    sota_engine_reload(e);
}

void sota_engine_validate_boot(sota_engine_t *e)
{
    if (e->validated || !running(e)) return;
    e->validated = true;
    sota_engine_reload(e);
    sota_sys_lock();
    sota_state_load_outbox(&e->outbox);
    sota_sys_unlock();
    if (sota_engine_waiting) return;

    sota_boot_info_t info;
    sota_boot_info(&info);
    sota_sys_wdt_quiesce();
    sota_boot_facts_t facts = boot_facts(&e->st, &info);

    sota_boot_outcome_t outcome = sota_classify_boot(&facts);
    note_image(e, &info, outcome);
    switch (outcome) {
    case SOTA_BOOT_NONE:
        /* An update applied without a trial left its report context. */
        if (e->st.conf_pend) {
            sota_boot_mark_valid();
            accept_record(e);
        }
        return;
    case SOTA_BOOT_ROLLBACK_CUT:
        /* The rollback is saved, so the boot that follows reports it. A
         * previous image that cannot be selected would restart forever. */
        SOTA_LOGW("finishing a rollback that power loss cut short");
        if (info.pending || sota_boot_set(e->st.prev_part)) sota_boot_rollback(e->st.prev_part);
        SOTA_LOGE("previous image cannot be selected; staying on this one");
        sota_state_drop_trial();
        sota_engine_reload(e);
        return;
    case SOTA_BOOT_REPORT_PENDING:
        report_rollback(e, e->st.rb_reason[0] ? e->st.rb_reason : "confirm_timeout");
        return;
    case SOTA_BOOT_ROLLED_BACK: {
        const char *cause = rollback_cause(e, info.reset_reason);
        SOTA_LOGW("back on the previous image (%s)", cause);
        sota_state_set_identity(&e->st.prev);
        report_rollback(e, cause);
        return;
    }
    case SOTA_BOOT_WAITING:
        sota_engine_waiting = true;
        return;
    case SOTA_BOOT_ABANDONED:
        SOTA_LOGW("update cut short before its slot switch; dropping it");
        if (sota_abandon_restores_identity(e->st.staged)) sota_state_set_identity(&e->st.prev);
        sota_state_drop_trial();
        sota_engine_reload(e);
        return;
    case SOTA_BOOT_INCOMPLETE:
        SOTA_LOGW("dropping a trial record that cannot be checked");
        sota_state_drop_trial();
        sota_engine_reload(e);
        return;
    case SOTA_BOOT_TRIAL_RESET:
    case SOTA_BOOT_TRIAL:
        if (e->cfg.rollback &&
            (outcome == SOTA_BOOT_TRIAL_RESET || sota_reset_is_crash(info.reset_reason))) {
            e->in_trial = true;
            e->ending = true;
            roll_back(e, sota_rollback_reason(info.reset_reason));
        }
        start_trial(e, facts.staged != 0);
        return;
    }
}

sota_confirm_result_t sota_engine_confirm(sota_engine_t *e)
{
    sota_engine_validate_boot(e);
    if (!claim_trial(e)) return SOTA_CONFIRM_NO_TRIAL;
    if (!sota_boot_mark_valid()) {
        SOTA_LOGW("bootloader did not accept the image; the trial goes on");
        sota_sys_lock();
        e->ending = false;
        sota_sys_unlock();
        sota_engine_service(e); /* a timeout that fired meanwhile */
        return SOTA_CONFIRM_FAILED;
    }
    /* A power loss from here on no longer rolls back: the next boot queues
     * the confirmed report. */
    if (!sota_state_end_trial()) SOTA_LOGW("trial end not saved");
    sota_sys_wdt_disarm();
    sota_sys_timer_stop(&e->timer);
    sota_sys_lock();
    e->in_trial = false;
    e->ending = false;
    e->timeout_pending = false;
    sota_sys_unlock();
    SOTA_LOGI("build %u confirmed", (unsigned)e->st.live.build);
    accept_record(e);
    return SOTA_CONFIRM_DONE;
}

void sota_engine_timeout_fired(sota_engine_t *e)
{
    e->timeout_pending = true;
    sota_app_wake(e->cfg.user);
}

void sota_engine_service(sota_engine_t *e)
{
    if (!e->timeout_pending) return;
    sota_sys_lock();
    bool claimed = e->in_trial && !e->ending;
    if (claimed) e->ending = true;
    /* A confirm in progress keeps it: when the confirm fails, it calls
     * this again. */
    if (claimed || !e->in_trial) e->timeout_pending = false;
    sota_sys_unlock();
    if (claimed) roll_back(e, "confirm_timeout");
}

/* ---------------------------------------------------------------------------
 * Check-in
 * ------------------------------------------------------------------------- */

bool sota_engine_check(sota_engine_t *e)
{
    if (!running(e)) return false;
    sota_engine_validate_boot(e);
    /* During a trial the reports wait, so the check-in is the only request
     * the confirm timeout can wait behind. */
    if (!e->in_trial) send_reports(e);
    sota_engine_clear_offer(e);
    e->last_check_ok = false;

    const char *labels = e->cfg.device.labels_json;
    if (labels && labels[0] && !sota_json_is_object(labels)) {
        SOTA_LOGW("labels are not a JSON object; checking in without them");
    }
    size_t body_cap = CHECK_BODY_CAP + (labels ? strlen(labels) : 0);
    const char *version = e->cfg.version_label ? e->cfg.version_label : e->st.live.version;
    char *body = (char *)malloc(body_cap);
    char *resp = (char *)malloc(CHECK_RESP_CAP);
    int status = -1;
    if (body && resp &&
        sota_protocol_check_body(&e->cfg.device, &e->st.live, version, body, body_cap)) {
        status = post(e, SOTA_PATH_CHECK, body, resp, CHECK_RESP_CAP);
    }
    free(body);
    if (status < 200 || status >= 300) {
        SOTA_LOGI("check: failed (HTTP %d)", status);
        free(resp);
        return false;
    }
    e->last_check_ok = true;

    /* The server answered: the image boots, connects and authenticates. */
    if (e->in_trial && e->cfg.auto_confirm && sota_engine_confirm(e) == SOTA_CONFIRM_DONE) {
        send_reports(e);
    }

    e->offer.url = (char *)malloc(OFFER_URL_CAP);
    e->offer.url_cap = e->offer.url ? OFFER_URL_CAP : 0;
    sota_check_result_t r = sota_protocol_parse_check(resp, &e->offer);
    if (r == SOTA_CHECK_OFFER) {
        e->has_offer = true;
        SOTA_LOGI("check: offer build=%u %s", (unsigned)e->offer.build, e->offer.version);
        sota_engine_emit(e, SOTA_EVENT_UPDATE_AVAILABLE, NULL, e->offer.build, 0,
                         e->offer.size);
    } else if (r == SOTA_CHECK_MALFORMED) {
        SOTA_LOGW("check: unusable offer");
    } else {
        char server_status[32];
        bool has = sota_protocol_check_status(resp, server_status, sizeof(server_status));
        SOTA_LOGI("check: no update (%s)", has ? server_status : "no status");
        sota_engine_emit(e, SOTA_EVENT_CHECK_DONE, has ? server_status : NULL,
                         e->st.live.build, 0, 0);
    }
    free(resp);
    if (!e->has_offer) sota_engine_clear_offer(e);
    return e->has_offer;
}

uint32_t sota_engine_step(sota_engine_t *e, bool connected)
{
    sota_engine_service(e);
    if (connected && sota_engine_check(e) && !e->in_trial && !sota_engine_waiting) {
        sota_engine_apply(e);
    }
    sota_sys_lock();
    bool report_pending = e->outbox.count > 0;
    sota_sys_unlock();
    return sota_poll_interval_s(connected, e->in_trial, report_pending, e->report_failures,
                                SOTA_TRIAL_RETRY_INTERVAL_S, e->cfg.check_interval_s);
}

bool sota_engine_reboot_for_update(sota_engine_t *e)
{
    if (!running(e) || !sota_engine_waiting) return false;
    sota_engine_post_status(e, "reboot", NULL, e->st.deployment, e->st.report_build);
    sota_sys_sleep_ms(100);
    sota_sys_restart();
}

void sota_engine_deinit(sota_engine_t *e)
{
    sota_engine_clear_offer(e);
    /* The watchdog is the device's: only the running engine stops it. */
    if (!running(e)) return;
    sota_sys_timer_stop(&e->timer);
    sota_sys_wdt_disarm();
    s_owner = NULL;
}

bool sota_engine_update_waiting(void)
{
    return sota_engine_waiting;
}

bool sota_engine_hold_pending_image(void)
{
    sota_state_t st;
    sota_state_load(&st);
    sota_boot_info_t info;
    sota_boot_info(&info);
    sota_boot_facts_t facts = boot_facts(&st, &info);
    return sota_hold_pending_image(info.pending, sota_classify_boot(&facts));
}
