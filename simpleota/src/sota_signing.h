/**
 * sota_signing.h - Ed25519 firmware-signature helpers for the SimpleOTA
 * ESP-IDF component.
 *
 * C port of SimpleOTAClient-Arduino's SimpleOTASigning.{h,cpp} (v0.4.0),
 * semantics byte-identical; the Arduino module is the reference contract.
 * Portable (no ESP-IDF dependencies) so it can be unit-tested on a host
 * machine with plain cc/g++; see test/host/test_signing.c.
 *
 * The SimpleOTA server delivers, for artifacts uploaded with security_mode
 * "signed", a base64 Ed25519 signature computed over the raw firmware bytes
 * exactly as downloaded. The device pins the project's PUBLIC key (it is not
 * a secret) and must verify the signature over the downloaded bytes BEFORE
 * the new image is marked bootable.
 *
 * Verification is incremental (Monocypher's streaming Ed25519 check) so the
 * image never has to fit in RAM: the apply path feeds the same chunks it
 * writes to the OTA partition.
 */

#ifndef SOTA_SIGNING_H
#define SOTA_SIGNING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "monocypher-ed25519.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of the signed-firmware policy decision (see sota_signed_gate). */
typedef enum {
    SOTA_GATE_SKIP,             /**< Not a signed update; apply normally. */
    SOTA_GATE_VERIFY,           /**< Verify the signature during download. */
    SOTA_GATE_FAIL_CLOSED,      /**< A signature is required but absent/unusable; reject now. */
    SOTA_GATE_WARN_UNVERIFIED,  /**< Signed offer but no key to check it; apply + warn. */
} sota_signed_gate_t;

/**
 * Decide how to handle a (possibly signed) offer, from device policy and the
 * offer's own claims. Pure and host-testable.
 *
 * The anti-downgrade guarantee lives here: a device that declares signed
 * mode AND has a key pinned MUST see a usable signature, no matter what the
 * offer claims. A compromised server or MITM that strips the security_mode /
 * signature fields from the response therefore cannot silently downgrade the
 * device to checksum-only flashing; it gets SOTA_GATE_FAIL_CLOSED. When the
 * device is not enforcing (not in signed mode, or no key pinned), the
 * offer's own declaration is trusted, which keeps basic-to-signed fleet
 * migration flowing.
 *
 * @param device_reports_signed  Device is configured with security_mode "signed".
 * @param num_keys               Count of pinned public keys (0..2).
 * @param offer_signed           The offer's security_mode is "signed".
 * @param offer_has_signature    A usable (decoded, ed25519) signature is present.
 */
sota_signed_gate_t sota_signed_gate(bool device_reports_signed, uint8_t num_keys,
                                    bool offer_signed, bool offer_has_signature);

/**
 * Parse a PEM "BEGIN PUBLIC KEY" (SubjectPublicKeyInfo) Ed25519 key, the
 * exact format the SimpleOTA dashboard and API hand out. The DER inside an
 * Ed25519 SPKI is fixed-size (44 bytes: a constant 12-byte header followed
 * by the 32 raw key bytes), so no ASN.1 parser is needed.
 *
 * @param pem  Null-terminated PEM text (armor lines + base64 body).
 * @param out  Receives the raw 32-byte public key on success.
 * @return true on success; false for malformed PEM, wrong key type, or
 *         wrong length. `out` is untouched on failure.
 */
bool sota_parse_ed25519_public_key_pem(const char *pem, uint8_t out[32]);

/**
 * Decode a base64 Ed25519 signature (as sent in the OTA check response)
 * into its raw 64 bytes.
 * @return true only if the input is valid base64 of exactly 64 bytes.
 */
bool sota_decode_signature_b64(const char *b64, uint8_t out[64]);

/**
 * Streaming Ed25519 verifier over one firmware image, for up to two
 * candidate public keys (key rotation windows).
 *
 * Runs one Monocypher incremental check context per candidate key; the
 * signature is accepted if it verifies under ANY candidate. Contexts are a
 * few hundred bytes each (embedded SHA-512 state); the whole struct is
 * ~1.5 KB, so allocate it on the heap or in static state, not on a task
 * stack next to the download buffer.
 *
 * Usage: sota_verifier_begin() once, sota_verifier_update() per downloaded
 * chunk, sota_verifier_final() once.
 */
#define SOTA_VERIFIER_MAX_KEYS 2

typedef struct {
    crypto_check_ed25519_ctx ctx[SOTA_VERIFIER_MAX_KEYS];
    uint8_t num_keys;
} sota_sig_verifier_t;

/**
 * @param v         Verifier state (zero-init not required; begin() resets it).
 * @param sig       Raw 64-byte signature from the OTA offer.
 * @param keys      Array of raw 32-byte public keys.
 * @param num_keys  1..SOTA_VERIFIER_MAX_KEYS candidate keys (extra ignored).
 * @return false (verifier inert; final() will fail) when num_keys is 0.
 */
bool sota_verifier_begin(sota_sig_verifier_t *v, const uint8_t sig[64],
                         const uint8_t keys[][32], uint8_t num_keys);

/** Feed the next chunk of the image, in download order. */
void sota_verifier_update(sota_sig_verifier_t *v, const uint8_t *data, size_t len);

/** @return true if the signature verifies under any candidate key. */
bool sota_verifier_final(sota_sig_verifier_t *v);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_SIGNING_H */
