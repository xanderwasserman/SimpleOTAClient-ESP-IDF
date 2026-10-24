/**
 * sota_protocol.c - see header.
 */

#include "sota_protocol.h"

#include <string.h>

#include "sota_json.h"
#include "sota_signing.h"

/* A JSON writer over a caller's buffer. ok goes false once something does
 * not fit, and every later append is skipped. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

static void put(writer_t *w, const char *s)
{
    size_t n = strlen(s);
    if (!w->ok || n >= w->cap - w->len) {
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, s, n + 1);
    w->len += n;
}

static void put_u32(writer_t *w, uint32_t v)
{
    char digits[11];
    size_t i = sizeof(digits) - 1;
    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    put(w, digits + i);
}

/* "key": after a comma unless it is the first member. */
static void put_key(writer_t *w, const char *key)
{
    put(w, w->len > 1 ? ",\"" : "\"");
    put(w, key);
    put(w, "\":");
}

static void put_str(writer_t *w, const char *key, const char *value)
{
    put_key(w, key);
    put(w, "\"");
    if (w->ok && !sota_json_escape(value, w->buf + w->len, w->cap - w->len)) w->ok = false;
    if (w->ok) w->len += strlen(w->buf + w->len);
    put(w, "\"");
}

static void put_opt_str(writer_t *w, const char *key, const char *value)
{
    if (value && value[0]) put_str(w, key, value);
}

static void put_num(writer_t *w, const char *key, uint32_t value)
{
    put_key(w, key);
    put_u32(w, value);
}

static writer_t writer(char *out, size_t cap)
{
    writer_t w = {out, cap, 0, cap > 0};
    if (cap > 0) out[0] = '\0';
    return w;
}

bool sota_protocol_check_body(const sota_device_info_t *dev, const sota_identity_t *live,
                              const char *version_label, char *out, size_t cap)
{
    writer_t w = writer(out, cap);
    put(&w, "{");
    put_str(&w, "device_id", dev->device_id);
    put_str(&w, "framework", dev->framework);
    put_str(&w, "chip_family", dev->chip_family);
    put_num(&w, "current_build_number", live->build);
    put_num(&w, "nvs_schema_version", dev->nvs_schema_version);
    put_opt_str(&w, "board_id", dev->board_id);
    put_opt_str(&w, "hardware_revision", dev->hardware_revision);
    put_opt_str(&w, "partition_profile", dev->partition_profile);
    put_opt_str(&w, "security_mode", dev->security_mode);
    put_opt_str(&w, "channel", dev->channel);
    put_opt_str(&w, "version_label", version_label);
    put_opt_str(&w, "current_hash", live->hash);
    /* Anything but one object could break out of the member. */
    if (dev->labels_json && sota_json_is_object(dev->labels_json)) {
        put_key(&w, "labels");
        put(&w, dev->labels_json);
    }
    put(&w, "}");
    return w.ok;
}

bool sota_protocol_status_body(const char *device_id, const char *deployment,
                               const char *event, uint32_t build, const char *reason,
                               char *out, size_t cap)
{
    writer_t w = writer(out, cap);
    put(&w, "{");
    put_str(&w, "device_id", device_id);
    put_str(&w, "deployment_id", deployment);
    put_str(&w, "event", event);
    put_num(&w, "build_number", build);
    if (reason) put_str(&w, "reason", reason);
    put(&w, "}");
    return w.ok;
}

static void lowercase(char *s)
{
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

/* An optional string member: empty when absent or too long for its field. */
static void get_opt(const char *resp, const char *key, char *out, size_t cap)
{
    if (!sota_json_get_string(resp, key, out, cap)) out[0] = '\0';
}

static void read_signature(const char *resp, sota_offer_t *offer)
{
    char mode[16], algo[16], sig_b64[128];
    get_opt(resp, "security_mode", mode, sizeof(mode));
    offer->signed_offer = strcmp(mode, "signed") == 0;
    if (!offer->signed_offer || !sota_json_get_string(resp, "signature", sig_b64, sizeof(sig_b64)))
        return;
    /* An algorithm other than ed25519 leaves the signature unusable, and so
     * does one that is not a short string: it cannot be ed25519 either. */
    bool algo_ok = !sota_json_has_key(resp, "signature_algorithm") ||
                   (sota_json_get_string(resp, "signature_algorithm", algo, sizeof(algo)) &&
                    (!algo[0] || strcmp(algo, "ed25519") == 0));
    offer->has_signature = algo_ok && sota_decode_signature_b64(sig_b64, offer->signature);
}

sota_check_result_t sota_protocol_parse_check(const char *resp, sota_offer_t *offer)
{
    char *url = offer->url;
    size_t url_cap = offer->url_cap;
    memset(offer, 0, sizeof(*offer));
    offer->url = url;
    offer->url_cap = url_cap;
    if (url && url_cap) url[0] = '\0';

    bool available = false;
    if (!sota_json_get_bool(resp, "update_available", &available) || !available)
        return SOTA_CHECK_NO_UPDATE;

    if (!url || !sota_json_get_string(resp, "url", url, url_cap) ||
        !sota_json_get_string(resp, "checksum", offer->checksum, sizeof(offer->checksum)) ||
        !sota_json_get_u32(resp, "build_number", &offer->build)) {
        if (url && url_cap) url[0] = '\0';
        return SOTA_CHECK_MALFORMED;
    }
    lowercase(offer->checksum);
    get_opt(resp, "deployment_id", offer->deployment, sizeof(offer->deployment));
    get_opt(resp, "version", offer->version, sizeof(offer->version));
    get_opt(resp, "signing_key_id", offer->key_id, sizeof(offer->key_id));
    sota_json_get_u32(resp, "size", &offer->size);
    read_signature(resp, offer);
    return SOTA_CHECK_OFFER;
}

bool sota_protocol_check_status(const char *resp, char *out, size_t cap)
{
    return sota_json_get_string(resp, "status", out, cap);
}
