/**
 * sota_json.h - minimal flat-JSON value extraction for SimpleOTA responses.
 *
 * C port of the (hardware-proven) scanner in SimpleOTAClient-Arduino: finds
 * a top-level "key" and reads its string / bool / unsigned value. This is
 * NOT a general JSON parser; it is deliberately the same code path the
 * Arduino client uses against the same server responses, chosen over cJSON
 * because the bundled `json` component moved to the registry in IDF 6 and a
 * vendored copy would risk duplicate-symbol clashes with apps that use
 * cJSON themselves.
 *
 * Portable (no ESP-IDF headers): host-tested in test/host/.
 */

#ifndef SOTA_JSON_H
#define SOTA_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extract a string value. Handles \n, \t, \r and pass-through of other
 * backslash escapes (matching the Arduino client).
 * @return true only when the key exists, is a string, AND the decoded value
 *         fits in out_cap-1 bytes (an oversized value fails loudly rather
 *         than truncating, so a clipped URL can never half-work).
 */
bool sota_json_get_string(const char *json, const char *key, char *out,
                          size_t out_cap);

/** Extract true/false. */
bool sota_json_get_bool(const char *json, const char *key, bool *out);

/** Extract a non-negative integer (digits only, as the server sends). */
bool sota_json_get_u32(const char *json, const char *key, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_JSON_H */
