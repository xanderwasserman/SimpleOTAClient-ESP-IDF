/**
 * Host-side unit tests for sota_signing (no ESP-IDF, no framework).
 *
 * Build & run (from the repository root; mirrors the CI host-tests job):
 *
 *   make -C test/host test
 *
 * Test vectors are reused verbatim from SimpleOTAClient-Arduino's
 * test/native/test_signing.cpp so both clients prove the identical
 * contract. Covers:
 *  - RFC 8032 pure-Ed25519 test vectors through the streaming interface.
 *  - A vector generated with the actual SimpleOTA server stack (Python
 *    `cryptography`): PEM public key + base64 signature over a 3037-byte
 *    message, proving the server-signed artifact verifies on this C path.
 *  - Negative cases: tampered message, wrong key, truncated/garbage
 *    signature, malformed PEM, non-Ed25519 DER.
 *  - Chunk-boundary invariance: 1-byte streaming == one-shot.
 *  - Two-key rotation acceptance (correct key in either slot).
 *  - The full sota_signed_gate truth table, including anti-downgrade.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sota_json.h"
#include "sota_sha256.h"
#include "sota_signing.h"

static int g_failures = 0;

#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("PASS  %s\n", name);                          \
        } else {                                                 \
            printf("FAIL  %s (line %d)\n", name, __LINE__);      \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    if (strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* Verify `msg` against `sig` with a single key, feeding `chunk`-sized pieces. */
static bool verify_chunked(const uint8_t sig[64], const uint8_t key[32],
                           const uint8_t *msg, size_t len, size_t chunk) {
    uint8_t keys[1][32];
    memcpy(keys[0], key, 32);
    sota_sig_verifier_t v;
    sota_verifier_begin(&v, sig, keys, 1);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        sota_verifier_update(&v, msg + off, n);
    }
    return sota_verifier_final(&v);
}

/* ---------------------------------------------------------------------------
 * Vectors (verbatim from SimpleOTAClient-Arduino test/native/test_signing.cpp)
 * ------------------------------------------------------------------------- */

/* RFC 8032 section 7.1, TEST 2: 1-byte message 0x72. */
static const char *k_rfc8032_pub2 =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char *k_rfc8032_sig2 =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
static const uint8_t k_rfc8032_msg2[1] = {0x72};

/* RFC 8032 section 7.1, TEST 3: 2-byte message af82. */
static const char *k_rfc8032_pub3 =
    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025";
static const char *k_rfc8032_sig3 =
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
    "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a";
static const uint8_t k_rfc8032_msg3[2] = {0xaf, 0x82};

/* Vector generated with the SimpleOTA server stack (Python `cryptography`,
 * the exact library that signs verification material in production). The
 * message is 3037 bytes of (i*7+13)&0xFF, deliberately not chunk-aligned. */
static const char *k_server_pub_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAB2HJU+VQ7QbclCJ1QOZAYKeSbt78tVkU74pDwJVYh0Y=\n"
    "-----END PUBLIC KEY-----\n";
static const char *k_server_sig_b64 =
    "RnV5622C7mDIKzs73+bTbKKAaBNDgFrpFg9EcfB/nAbT+QzLRzznHavLSjgjeEMc"
    "0NmhNtKk9qUOESyG4FqmDg==";
static const char *k_other_pub_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEArGD0GjJUgwBdqQe1YXY0JIwlNdC/0g/ViSArBa9fXDg=\n"
    "-----END PUBLIC KEY-----\n";
#define K_SERVER_MSG_LEN 3037

static void fill_server_msg(uint8_t *buf) {
    for (size_t i = 0; i < K_SERVER_MSG_LEN; ++i) {
        buf[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }
}

/* An RSA-2048 SPKI PEM (valid PEM + DER, wrong algorithm): must be rejected. */
static const char *k_rsa_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Z3VS5JJcds3xfn/ygWy\n"
    "F0mLxGZk3GG9tS+0RrGXi9K+PT47r1Vv7WeStkGxT2AwLYCSzMPOGDJbfBRWJVkI\n"
    "b3nDCONVn0DDF7VkxSTBEXTvJqqu69R2M8Xr32NnCzUleC0eq/eqhBpc8dSKUeqB\n"
    "8sJyxnywjm14sffJ4XXm3Or92BpTn6DkYA1KYIRLBLl3a3H9uKGoZbrPnozI7bMK\n"
    "aTgm27u2rBFsAy4pkxbDpgvzrSGRUJPTUAqxvjV/xnkTMQb8DTFXOLd06taWxNRJ\n"
    "eolbrTBTThLqbP2vAgMt2/oW1kKzXd1Y+HYX+GY1Kb2ARi/1zwqi/ArNErHVCLnK\n"
    "5QIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

/* ------------------------------------------------------------------------- */

/* SHA-256 vectors: FIPS 180-4 / NIST CAVP. */
static bool sha256_hex_equals(const uint8_t digest[32], const char *hex) {
    char got[65];
    for (int i = 0; i < 32; ++i) sprintf(got + 2 * i, "%02x", digest[i]);
    got[64] = '\0';
    return strcmp(got, hex) == 0;
}

static void run_sha256_tests(void) {
    uint8_t digest[32];
    sota_sha256_ctx ctx;

    sota_sha256_init(&ctx);
    sota_sha256_final(&ctx, digest);
    CHECK(sha256_hex_equals(
              digest,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256: empty message (NIST)");

    sota_sha256_init(&ctx);
    sota_sha256_update(&ctx, (const uint8_t *)"abc", 3);
    sota_sha256_final(&ctx, digest);
    CHECK(sha256_hex_equals(
              digest,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256: 'abc' (NIST)");

    const char *two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sota_sha256_init(&ctx);
    /* 1-byte streaming exercises every buffer boundary. */
    for (const char *p = two_block; *p; ++p) {
        sota_sha256_update(&ctx, (const uint8_t *)p, 1);
    }
    sota_sha256_final(&ctx, digest);
    CHECK(sha256_hex_equals(
              digest,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "sha256: two-block message, 1-byte chunks (NIST)");

    /* One million 'a', streamed in odd-sized chunks. */
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    sota_sha256_init(&ctx);
    for (int i = 0; i < 1000; ++i) {
        sota_sha256_update(&ctx, chunk, sizeof(chunk));
    }
    sota_sha256_final(&ctx, digest);
    CHECK(sha256_hex_equals(
              digest,
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "sha256: one million 'a' (NIST)");
}

/* Parse a realistic /check/ offer response with the scanner the component
 * actually uses (same construction as the hardware-proven Arduino one). */
static void run_json_tests(void) {
    const char *offer =
        "{\"update_available\": true, \"build_number\": 42, "
        "\"version\": \"2026.29.10\", "
        "\"url\": \"https://storage.example.com/fw.bin?X-Amz-Signature=abc123\", "
        "\"size\": 1048576, "
        "\"checksum\": \"AB12cd34\", "
        "\"deployment_id\": \"d3adb33f-1234\", "
        "\"security_mode\": \"signed\", "
        "\"signing_key_id\": \"prod-2026\", "
        "\"signature_algorithm\": \"ed25519\", "
        "\"signature\": \"c2ln\", "
        "\"message\": \"quoted \\\"url\\\" inside a string\"}";

    bool b = false;
    uint32_t u = 0;
    char buf[128];

    CHECK(sota_json_get_bool(offer, "update_available", &b) && b,
          "json: update_available true");
    CHECK(sota_json_get_u32(offer, "build_number", &u) && u == 42,
          "json: build_number");
    CHECK(sota_json_get_u32(offer, "size", &u) && u == 1048576, "json: size");
    CHECK(sota_json_get_string(offer, "url", buf, sizeof(buf)) &&
              strcmp(buf,
                     "https://storage.example.com/fw.bin?X-Amz-Signature="
                     "abc123") == 0,
          "json: url with query string");
    CHECK(sota_json_get_string(offer, "version", buf, sizeof(buf)) &&
              strcmp(buf, "2026.29.10") == 0,
          "json: version");
    CHECK(sota_json_get_string(offer, "security_mode", buf, sizeof(buf)) &&
              strcmp(buf, "signed") == 0,
          "json: security_mode");
    CHECK(!sota_json_get_string(offer, "missing_key", buf, sizeof(buf)),
          "json: absent key fails");
    CHECK(!sota_json_get_string(offer, "url", buf, 16),
          "json: oversized value fails loudly (no truncation)");
    CHECK(!sota_json_get_bool(offer, "version", &b),
          "json: string value is not a bool");

    const char *no_update =
        "{\"update_available\": false, \"status\": \"up_to_date\", "
        "\"message\": \"Device is on the latest build.\"}";
    CHECK(sota_json_get_bool(no_update, "update_available", &b) && !b,
          "json: update_available false");
    CHECK(sota_json_get_string(no_update, "status", buf, sizeof(buf)) &&
              strcmp(buf, "up_to_date") == 0,
          "json: status token");

    const char *accepted = "{\"accepted\": true, \"final_status\": \"confirmed\"}";
    CHECK(sota_json_get_bool(accepted, "accepted", &b) && b,
          "json: status accepted");

    /* Escapes decode like the Arduino client. */
    const char *esc = "{\"v\": \"a\\nb\\tc\\\"d\"}";
    CHECK(sota_json_get_string(esc, "v", buf, sizeof(buf)) &&
              strcmp(buf, "a\nb\tc\"d") == 0,
          "json: escape decoding");
}

int main(void) {
    printf("sota_signing host tests\n=======================\n");

    run_json_tests();
    run_sha256_tests();

    /* --- PEM parsing ------------------------------------------------------ */
    uint8_t server_key[32], other_key[32], scratch[32];
    CHECK(sota_parse_ed25519_public_key_pem(k_server_pub_pem, server_key),
          "parse server public key PEM");
    CHECK(sota_parse_ed25519_public_key_pem(k_other_pub_pem, other_key),
          "parse second public key PEM");
    CHECK(!sota_parse_ed25519_public_key_pem("not a pem at all", scratch),
          "reject garbage PEM");
    CHECK(!sota_parse_ed25519_public_key_pem(
              "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n",
              scratch),
          "reject too-short DER");
    CHECK(!sota_parse_ed25519_public_key_pem(k_rsa_pem, scratch),
          "reject non-Ed25519 (RSA) SPKI");
    CHECK(!sota_parse_ed25519_public_key_pem(NULL, scratch), "reject NULL PEM");

    /* --- signature decoding ------------------------------------------------ */
    uint8_t server_sig[64];
    CHECK(sota_decode_signature_b64(k_server_sig_b64, server_sig),
          "decode server signature base64");
    uint8_t tmp[64];
    CHECK(!sota_decode_signature_b64("QUJD", tmp), "reject 3-byte signature");
    CHECK(!sota_decode_signature_b64("@@!!", tmp), "reject invalid base64");
    CHECK(!sota_decode_signature_b64(NULL, tmp), "reject NULL signature");
    {
        /* Truncated: drop the last 4 chars (3 bytes) of a valid signature. */
        char trunc[96];
        strncpy(trunc, k_server_sig_b64, sizeof(trunc));
        trunc[sizeof(trunc) - 1] = 0;
        trunc[strlen(trunc) - 4] = 0;
        CHECK(!sota_decode_signature_b64(trunc, tmp), "reject truncated signature");
    }

    /* --- RFC 8032 vectors via streaming ------------------------------------ */
    {
        uint8_t pub[32], sig[64];
        CHECK(hex_to_bytes(k_rfc8032_pub2, pub, 32) &&
                  hex_to_bytes(k_rfc8032_sig2, sig, 64),
              "load RFC 8032 test 2 vector");
        CHECK(verify_chunked(sig, pub, k_rfc8032_msg2, 1, 1),
              "RFC 8032 test 2 verifies (streaming)");
        CHECK(hex_to_bytes(k_rfc8032_pub3, pub, 32) &&
                  hex_to_bytes(k_rfc8032_sig3, sig, 64),
              "load RFC 8032 test 3 vector");
        CHECK(verify_chunked(sig, pub, k_rfc8032_msg3, 2, 1),
              "RFC 8032 test 3 verifies (1-byte chunks)");
        /* Tamper: flip one bit. */
        uint8_t bad[2] = {(uint8_t)(k_rfc8032_msg3[0] ^ 0x01), k_rfc8032_msg3[1]};
        CHECK(!verify_chunked(sig, pub, bad, 2, 2),
              "RFC 8032 test 3 rejects tampered message");
    }

    /* --- server-stack vector ------------------------------------------------ */
    uint8_t *msg = (uint8_t *)malloc(K_SERVER_MSG_LEN);
    fill_server_msg(msg);

    CHECK(verify_chunked(server_sig, server_key, msg, K_SERVER_MSG_LEN, 4096),
          "server-signed message verifies (4 KB chunks, apply-style)");
    CHECK(verify_chunked(server_sig, server_key, msg, K_SERVER_MSG_LEN,
                         K_SERVER_MSG_LEN),
          "server-signed message verifies (one shot)");
    CHECK(verify_chunked(server_sig, server_key, msg, K_SERVER_MSG_LEN, 1),
          "server-signed message verifies (1-byte chunks)");

    msg[1500] ^= 0x80; /* tamper mid-image */
    CHECK(!verify_chunked(server_sig, server_key, msg, K_SERVER_MSG_LEN, 1024),
          "tampered image rejected");
    msg[1500] ^= 0x80; /* restore */

    CHECK(!verify_chunked(server_sig, other_key, msg, K_SERVER_MSG_LEN, 1024),
          "wrong public key rejected");
    CHECK(!verify_chunked(server_sig, server_key, msg, K_SERVER_MSG_LEN - 1, 1024),
          "short image rejected");

    /* --- multi-key rotation -------------------------------------------------- */
    {
        uint8_t keys[2][32];
        sota_sig_verifier_t v;

        /* Correct key in slot 1 (rotation: old key first, new key second). */
        memcpy(keys[0], other_key, 32);
        memcpy(keys[1], server_key, 32);
        sota_verifier_begin(&v, server_sig, keys, 2);
        sota_verifier_update(&v, msg, K_SERVER_MSG_LEN);
        CHECK(sota_verifier_final(&v),
              "two pinned keys: matching key in slot 1 accepted");

        /* Correct key in slot 0. */
        memcpy(keys[0], server_key, 32);
        memcpy(keys[1], other_key, 32);
        sota_verifier_begin(&v, server_sig, keys, 2);
        sota_verifier_update(&v, msg, K_SERVER_MSG_LEN);
        CHECK(sota_verifier_final(&v),
              "two pinned keys: matching key in slot 0 accepted");

        /* Neither key matches. */
        memcpy(keys[0], other_key, 32);
        memcpy(keys[1], other_key, 32);
        sota_verifier_begin(&v, server_sig, keys, 2);
        sota_verifier_update(&v, msg, K_SERVER_MSG_LEN);
        CHECK(!sota_verifier_final(&v), "two wrong keys rejected");

        /* Zero keys: inert verifier fails closed. */
        CHECK(!sota_verifier_begin(&v, server_sig, keys, 0),
              "begin() with zero keys is inert");
        CHECK(!sota_verifier_final(&v), "final() with zero keys fails");
    }

    free(msg);

    /* --- signed-firmware policy gate ---------------------------------------- */
    /* sota_signed_gate(device_reports_signed, num_keys, offer_signed,
     *                  offer_has_signature) */

    /* Enforcing device (reports signed + key pinned): signature mandatory. */
    CHECK(sota_signed_gate(true, 1, true, true) == SOTA_GATE_VERIFY,
          "gate: signed device + key + valid signed offer -> VERIFY");
    CHECK(sota_signed_gate(true, 1, true, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + key + signed offer, no usable sig -> FAIL_CLOSED");
    /* The anti-downgrade case: server strips security_mode/signature. */
    CHECK(sota_signed_gate(true, 1, false, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + key + UNSIGNED offer -> FAIL_CLOSED (no downgrade)");
    CHECK(sota_signed_gate(true, 2, false, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + 2 keys + unsigned offer -> FAIL_CLOSED");

    /* Signed device but no key pinned yet (misconfig / mid-migration). */
    CHECK(sota_signed_gate(true, 0, true, false) == SOTA_GATE_WARN_UNVERIFIED,
          "gate: signed device, no key, signed offer -> WARN_UNVERIFIED");
    CHECK(sota_signed_gate(true, 0, false, false) == SOTA_GATE_SKIP,
          "gate: signed device, no key, unsigned offer -> SKIP");

    /* Non-enforcing device (not in signed mode): trust the offer. */
    CHECK(sota_signed_gate(false, 0, false, false) == SOTA_GATE_SKIP,
          "gate: basic device, basic offer -> SKIP");
    CHECK(sota_signed_gate(false, 0, true, false) == SOTA_GATE_WARN_UNVERIFIED,
          "gate: basic device, no key, signed offer -> WARN_UNVERIFIED (migration)");
    CHECK(sota_signed_gate(false, 1, true, true) == SOTA_GATE_VERIFY,
          "gate: basic device, key pinned, valid signed offer -> VERIFY (defensive)");
    CHECK(sota_signed_gate(false, 1, true, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: basic device, key pinned, signed offer w/o sig -> FAIL_CLOSED");
    CHECK(sota_signed_gate(false, 1, false, false) == SOTA_GATE_SKIP,
          "gate: basic device, key pinned, basic offer -> SKIP");

    printf("=======================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
