/**
 * sota_state.c - see the header and docs/NVS_CONTRACT.md for the keys and
 * the write order.
 */

#include "sota_state.h"

#include <string.h>

#include "sota_kv.h"
#include "sota_trial.h"

typedef char sota_deployment_fits_report[
    sizeof(((sota_state_t *)0)->deployment) == sizeof(((sota_report_t *)0)->deployment) ? 1 : -1];

#define K_BUILD "sota_build"
#define K_HASH "sota_hash"
#define K_VER "sota_ver"
#define K_TRIAL "sota_trial"
#define K_STAGED "sota_staged"
#define K_NEW_PART "sota_new_part"
#define K_PREV_PART "sota_prev_part"
#define K_PREV_BUILD "sota_prev_build"
#define K_PREV_HASH "sota_prev_hash"
#define K_PREV_VER "sota_prev_ver"
#define K_DEP "sota_fail_dep"
#define K_REPORT_BUILD "sota_fail_build"
#define K_CONF_PEND "sota_conf_pend"
#define K_RB_REASON "sota_rb_reason"
#define K_OUTBOX "sota_outbox"
#define K_IMAGE "sota_image"

static void get_str(const char *key, char *out, size_t cap)
{
    if (!sota_kv_get_str(key, out, cap)) out[0] = '\0';
}

/* The version label as stored: empty when it is too long to keep. */
static const char *stored_version(const char *version)
{
    return version && strlen(version) <= SOTA_VERSION_MAX ? version : "";
}

static bool set_identity(const char *k_build, const char *k_hash, const char *k_ver,
                         const sota_identity_t *id)
{
    return sota_kv_set_u32(k_build, id->build) &&
           sota_kv_set_str(k_hash, id->hash) &&
           sota_kv_set_str(k_ver, stored_version(id->version));
}

static bool set_report_context(const char *deployment, uint32_t build)
{
    return sota_kv_set_str(K_DEP, deployment ? deployment : "") &&
           sota_kv_set_u32(K_REPORT_BUILD, build) &&
           sota_kv_set_u8(K_CONF_PEND, 1);
}

void sota_state_load(sota_state_t *st)
{
    memset(st, 0, sizeof(*st));
    sota_kv_get_u32(K_BUILD, &st->live.build);
    get_str(K_HASH, st->live.hash, sizeof(st->live.hash));
    get_str(K_VER, st->live.version, sizeof(st->live.version));
    sota_kv_get_u8(K_TRIAL, &st->trial);
    sota_kv_get_u8(K_STAGED, &st->staged);
    sota_kv_get_u32(K_NEW_PART, &st->new_part);
    sota_kv_get_u32(K_PREV_PART, &st->prev_part);
    sota_kv_get_u32(K_PREV_BUILD, &st->prev.build);
    get_str(K_PREV_HASH, st->prev.hash, sizeof(st->prev.hash));
    get_str(K_PREV_VER, st->prev.version, sizeof(st->prev.version));
    get_str(K_DEP, st->deployment, sizeof(st->deployment));
    sota_kv_get_u32(K_REPORT_BUILD, &st->report_build);
    sota_kv_get_u8(K_CONF_PEND, &st->conf_pend);
    get_str(K_RB_REASON, st->rb_reason, sizeof(st->rb_reason));
    get_str(K_IMAGE, st->image_id, sizeof(st->image_id));
}

bool sota_state_stage_trial(const sota_identity_t *prev, const sota_identity_t *next,
                            uint32_t new_part, uint32_t prev_part,
                            const char *deployment)
{
    bool snapshot = sota_kv_set_u8(K_STAGED, SOTA_STAGED_STARTED) &&
                    sota_kv_set_u32(K_NEW_PART, new_part) &&
                    sota_kv_set_u32(K_PREV_PART, prev_part) &&
                    set_identity(K_PREV_BUILD, K_PREV_HASH, K_PREV_VER, prev) &&
                    sota_kv_set_u8(K_STAGED, SOTA_STAGED_SNAPSHOT);
    bool staged = snapshot &&
                  set_identity(K_BUILD, K_HASH, K_VER, next) &&
                  set_report_context(deployment, next->build) &&
                  sota_kv_erase(K_RB_REASON) &&
                  sota_kv_set_u8(K_TRIAL, SOTA_TRIAL_STAGED);
    if (staged) return true;
    /* Only a complete snapshot was followed by writes to the live identity. */
    if (snapshot) set_identity(K_BUILD, K_HASH, K_VER, prev);
    sota_state_drop_trial();
    return false;
}

bool sota_state_unstage(void)
{
    return sota_kv_erase(K_STAGED);
}

bool sota_state_accept_update(const sota_identity_t *next, const char *deployment)
{
    return set_identity(K_BUILD, K_HASH, K_VER, next) &&
           set_report_context(deployment, next->build);
}

bool sota_state_set_identity(const sota_identity_t *id)
{
    return set_identity(K_BUILD, K_HASH, K_VER, id);
}

bool sota_state_set_image_id(const char *image_id)
{
    return sota_kv_set_str(K_IMAGE, image_id);
}

bool sota_state_mark_rolled_back(const sota_state_t *st, const char *reason)
{
    return set_identity(K_BUILD, K_HASH, K_VER, &st->prev) &&
           sota_kv_set_str(K_RB_REASON, reason ? reason : "confirm_timeout") &&
           sota_kv_set_u8(K_TRIAL, SOTA_TRIAL_ROLLED_BACK);
}

bool sota_state_mark_booted(void)
{
    return sota_kv_set_u8(K_TRIAL, SOTA_TRIAL_BOOTED);
}

bool sota_state_end_trial(void)
{
    return sota_kv_erase(K_TRIAL);
}

bool sota_state_drop_trial(void)
{
    /* sota_trial goes first: once it is gone, what is left is not a trial. */
    static const char *const keys[] = {
        K_TRIAL, K_STAGED, K_NEW_PART, K_PREV_PART, K_PREV_BUILD, K_PREV_HASH,
        K_PREV_VER, K_DEP, K_REPORT_BUILD, K_CONF_PEND, K_RB_REASON,
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        ok = sota_kv_erase(keys[i]) && ok;
    }
    return ok;
}

void sota_state_load_outbox(sota_outbox_t *box)
{
    sota_outbox_t stored;
    if (sota_kv_get_blob(K_OUTBOX, &stored, sizeof(stored)) && sota_outbox_valid(&stored)) {
        *box = stored;
    } else {
        sota_outbox_init(box);
    }
}

bool sota_state_save_outbox(const sota_outbox_t *box)
{
    if (box->count == 0) return sota_kv_erase(K_OUTBOX);
    return sota_kv_set_blob(K_OUTBOX, box, sizeof(*box));
}
