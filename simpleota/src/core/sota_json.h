/**
 * @file
 * @brief Minimal flat-JSON value extraction for SimpleOTA responses.
 *
 * Finds a "key" among the top-level object's members and reads its string /
 * bool / unsigned value. Keys inside nested objects and arrays are not
 * matched, so a nested field cannot stand in for a top-level one. This is NOT a
 * general JSON parser; it is a C port of the hardware-proven scanner the
 * Arduino client has used since its first release. Chosen over cJSON
 * because the bundled `json` component left the ESP-IDF core in IDF 6, and
 * a vendored copy would risk duplicate symbols in apps that use cJSON
 * themselves.
 *
 * Portable (no platform headers): host-tested in test/host/.
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
 * Extract a string value. Decodes `\n`, `\t` and `\r`; any other backslash
 * escape yields the character after it (`\uXXXX` is not decoded).
 * @return true only when the key exists, is a string, AND the decoded value
 *         fits in out_cap-1 bytes (an oversized value fails loudly rather
 *         than truncating, so a clipped URL can never half-work).
 */
bool sota_json_get_string(const char *json, const char *key, char *out,
                          size_t out_cap);

/**
 * Write src as the inside of a JSON string: `"` and `\` are escaped,
 * newline, carriage return and tab become escapes, and other control
 * characters are dropped.
 * @return true when the result and its terminating NUL fit in out_cap.
 */
bool sota_json_escape(const char *src, char *out, size_t out_cap);

/**
 * True when s is one JSON object whose brackets pair up and whose strings
 * close, nested at most 8 deep, with nothing after it: something that can
 * go into a request body as a member's value without breaking out of it.
 * Not a full validator.
 */
bool sota_json_is_object(const char *s);

/** True when the key is present, whatever its value. */
bool sota_json_has_key(const char *json, const char *key);

/** Extract true/false. */
bool sota_json_get_bool(const char *json, const char *key, bool *out);

/** Extract a non-negative integer (digits only, as the server sends). */
bool sota_json_get_u32(const char *json, const char *key, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_JSON_H */
