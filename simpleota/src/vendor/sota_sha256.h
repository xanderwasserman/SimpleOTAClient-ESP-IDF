/**
 * sota_sha256.h - minimal FIPS 180-4 SHA-256, no dependencies.
 *
 * Vendored so the component builds identically on every IDF version:
 * IDF <= 5.x exposes the legacy mbedtls/sha256.h API, IDF 6 (mbedTLS 4)
 * is PSA-only, and neither shape is worth a dual code path for hashing a
 * download stream. Validated against the NIST test vectors in
 * test/host/test_signing.c.
 *
 * Public domain style implementation (same construction as the reference
 * in FIPS 180-4); streaming init/update/final interface.
 */

#ifndef SOTA_SHA256_H
#define SOTA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buffer_len;
} sota_sha256_ctx;

void sota_sha256_init(sota_sha256_ctx *ctx);
void sota_sha256_update(sota_sha256_ctx *ctx, const uint8_t *data, size_t len);
void sota_sha256_final(sota_sha256_ctx *ctx, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_SHA256_H */
