/**
 * sota_signing.c - implementation. See header for the contract.
 *
 * C port of SimpleOTAClient-Arduino's SimpleOTASigning.cpp (v0.4.0);
 * behavior is kept byte-identical to the Arduino reference. Kept free of
 * ESP-IDF headers on purpose: everything here compiles with plain cc for
 * the host-side unit tests in test/host/.
 */

#include "sota_signing.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * base64 (decode only)
 * ------------------------------------------------------------------------- */

/* Returns the 0..63 value of a base64 digit, or -1 for anything else
 * (including '=' padding, which the caller handles explicitly). */
static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 from `in` (whitespace/newlines skipped, '=' padding
 * tolerated at the end) into `out`, which holds `out_cap` bytes. Returns the
 * number of bytes written, or -1 on any malformed input or overflow. */
static int b64_decode(const char *in, uint8_t *out, size_t out_cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;
    bool saw_pad = false;

    for (const char *p = in; *p; ++p) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') { saw_pad = true; continue; }
        if (saw_pad) return -1;  /* data after padding */
        int v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_cap) return -1;
            out[written++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    /* Leftover bits must be zero padding only (canonical-ish check). */
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return -1;
    return (int)written;
}

/* ---------------------------------------------------------------------------
 * Signed-firmware policy
 * ------------------------------------------------------------------------- */

sota_signed_gate_t sota_signed_gate(bool device_reports_signed, uint8_t num_keys,
                                    bool offer_signed, bool offer_has_signature) {
    /* Enforcing: the device declares signed mode and can actually verify.
     * A signature is then mandatory regardless of what the offer claims,
     * so a stripped-signature response fails closed rather than
     * downgrading. */
    if (device_reports_signed && num_keys > 0) {
        return offer_has_signature ? SOTA_GATE_VERIFY : SOTA_GATE_FAIL_CLOSED;
    }
    /* Not enforcing: trust the offer's own declaration. */
    if (!offer_signed) {
        return SOTA_GATE_SKIP;
    }
    if (num_keys == 0) {
        /* Signed offer but nothing to check it against (e.g. a device
         * mid-migration that has not pinned a key yet). Best effort. */
        return SOTA_GATE_WARN_UNVERIFIED;
    }
    /* Keys pinned and the offer claims signed: a usable signature is
     * expected; its absence is treated as a rejectable anomaly. */
    return offer_has_signature ? SOTA_GATE_VERIFY : SOTA_GATE_FAIL_CLOSED;
}

/* ---------------------------------------------------------------------------
 * PEM parsing
 * ------------------------------------------------------------------------- */

/* DER header of an Ed25519 SubjectPublicKeyInfo (RFC 8410):
 *   SEQUENCE(42) { SEQUENCE(5) { OID 1.3.101.112 }, BIT STRING(33) 0x00 ... }
 * The full structure is always exactly 44 bytes: these 12, then the raw key. */
static const uint8_t k_ed25519_spki_header[12] = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00,
};

bool sota_parse_ed25519_public_key_pem(const char *pem, uint8_t out[32]) {
    if (!pem) return false;

    const char *begin = strstr(pem, "-----BEGIN PUBLIC KEY-----");
    if (!begin) return false;
    begin += strlen("-----BEGIN PUBLIC KEY-----");
    const char *end = strstr(begin, "-----END PUBLIC KEY-----");
    if (!end) return false;

    /* Copy the base64 body into a bounded scratch buffer. An Ed25519 SPKI
     * body is 60 base64 chars; allow generous slack for line breaks. */
    char body[128];
    size_t n = (size_t)(end - begin);
    if (n >= sizeof(body)) return false;
    memcpy(body, begin, n);
    body[n] = '\0';

    uint8_t der[64];
    int der_len = b64_decode(body, der, sizeof(der));
    if (der_len != 44) return false;
    if (memcmp(der, k_ed25519_spki_header, sizeof(k_ed25519_spki_header)) != 0) {
        return false;  /* valid PEM but not an Ed25519 public key */
    }
    memcpy(out, der + sizeof(k_ed25519_spki_header), 32);
    return true;
}

bool sota_decode_signature_b64(const char *b64, uint8_t out[64]) {
    if (!b64) return false;
    uint8_t buf[80];
    int n = b64_decode(b64, buf, sizeof(buf));
    if (n != 64) return false;
    memcpy(out, buf, 64);
    return true;
}

/* ---------------------------------------------------------------------------
 * sota_sig_verifier_t
 * ------------------------------------------------------------------------- */

bool sota_verifier_begin(sota_sig_verifier_t *v, const uint8_t sig[64],
                         const uint8_t keys[][32], uint8_t num_keys) {
    v->num_keys = (num_keys > SOTA_VERIFIER_MAX_KEYS) ? SOTA_VERIFIER_MAX_KEYS
                                                      : num_keys;
    for (uint8_t i = 0; i < v->num_keys; ++i) {
        crypto_ed25519_check_init((crypto_check_ctx_abstract *)&v->ctx[i], sig,
                                  keys[i]);
    }
    return v->num_keys > 0;
}

void sota_verifier_update(sota_sig_verifier_t *v, const uint8_t *data, size_t len) {
    for (uint8_t i = 0; i < v->num_keys; ++i) {
        crypto_ed25519_check_update((crypto_check_ctx_abstract *)&v->ctx[i], data,
                                    len);
    }
}

bool sota_verifier_final(sota_sig_verifier_t *v) {
    /* Run EVERY context to completion (no early return) so timing does not
     * depend on which candidate key matched. */
    bool ok = false;
    for (uint8_t i = 0; i < v->num_keys; ++i) {
        if (crypto_ed25519_check_final((crypto_check_ctx_abstract *)&v->ctx[i]) == 0) {
            ok = true;
        }
    }
    v->num_keys = 0;  /* contexts are consumed; require a fresh begin() */
    return ok;
}
