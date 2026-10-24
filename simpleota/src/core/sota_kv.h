/**
 * @file
 * @brief The key-value store the core persists its state in.
 *
 * Implemented by the platform port (NVS namespace "simpleota" on ESP32) and
 * by the host tests' fake. Bound at link time: the core calls these
 * directly. Keys are at most 15 characters, the NVS limit.
 *
 * Each set reaches storage on its own, before it returns: the core orders
 * its writes so that a power cut between any two of them is recoverable.
 */

#ifndef SOTA_KV_H
#define SOTA_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read a value. False, with *out untouched, when the key is missing, has
 *  another type, or (strings) does not fit in cap bytes with its NUL. */
/** @{ */
bool sota_kv_get_u8(const char *key, uint8_t *out);
bool sota_kv_get_u32(const char *key, uint32_t *out);
bool sota_kv_get_str(const char *key, char *out, size_t cap);
/** @} */
/** A blob of exactly len bytes; false for any other length. */
bool sota_kv_get_blob(const char *key, void *out, size_t len);

/** Write a value. False when it did not reach storage. */
/** @{ */
bool sota_kv_set_u8(const char *key, uint8_t value);
bool sota_kv_set_u32(const char *key, uint32_t value);
bool sota_kv_set_str(const char *key, const char *value);
bool sota_kv_set_blob(const char *key, const void *value, size_t len);
/** @} */

/** Remove a key. True when it is gone, also when it was never there. */
bool sota_kv_erase(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_KV_H */
