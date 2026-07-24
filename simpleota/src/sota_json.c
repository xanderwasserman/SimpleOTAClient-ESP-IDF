/* sota_json.c - see header. Port of the Arduino client's scanner. */

#include "sota_json.h"

#include <string.h>

/* Find `"key"` at any nesting depth and set *val_start to the first
 * non-whitespace character after the colon. Skips over other quoted strings
 * honoring backslash escapes so embedded quotes cannot end the scan early. */
static bool json_find_value(const char *s, const char *key, size_t *val_start) {
    size_t klen = strlen(key);
    size_t n = strlen(s);
    size_t p = 0;
    while (p < n) {
        char c = s[p];
        if (c == '"') {
            size_t q = p + 1;
            bool matches = (q + klen < n) && (memcmp(s + q, key, klen) == 0) &&
                           (s[q + klen] == '"');
            if (matches) {
                size_t look = q + klen + 1;
                while (look < n && (s[look] == ' ' || s[look] == '\t' ||
                                    s[look] == '\n' || s[look] == '\r')) {
                    ++look;
                }
                if (look < n && s[look] == ':') {
                    size_t v = look + 1;
                    while (v < n && (s[v] == ' ' || s[v] == '\t' ||
                                     s[v] == '\n' || s[v] == '\r')) {
                        ++v;
                    }
                    *val_start = v;
                    return true;
                }
            }
            /* Not our key; skip the rest of this string. */
            p = q;
            while (p < n && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < n) p += 2;
                else ++p;
            }
            if (p < n) ++p; /* past closing quote */
        } else {
            ++p;
        }
    }
    return false;
}

bool sota_json_get_string(const char *json, const char *key, char *out,
                          size_t out_cap) {
    if (!json || !out || out_cap == 0) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    size_t n = strlen(json);
    if (p >= n || json[p] != '"') return false;
    ++p;
    size_t w = 0;
    while (p < n && json[p] != '"') {
        char c = json[p];
        char decoded;
        if (c == '\\' && p + 1 < n) {
            char esc = json[p + 1];
            if (esc == 'n') decoded = '\n';
            else if (esc == 't') decoded = '\t';
            else if (esc == 'r') decoded = '\r';
            else decoded = esc;
            p += 2;
        } else {
            decoded = c;
            ++p;
        }
        if (w + 1 >= out_cap) return false; /* would truncate: fail loudly */
        out[w++] = decoded;
    }
    out[w] = '\0';
    return true;
}

bool sota_json_get_bool(const char *json, const char *key, bool *out) {
    if (!json || !out) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    if (strncmp(json + p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(json + p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool sota_json_get_u32(const char *json, const char *key, uint32_t *out) {
    if (!json || !out) return false;
    size_t p;
    if (!json_find_value(json, key, &p)) return false;
    size_t n = strlen(json);
    uint32_t v = 0;
    bool any = false;
    while (p < n && json[p] >= '0' && json[p] <= '9') {
        v = v * 10u + (uint32_t)(json[p] - '0');
        ++p;
        any = true;
    }
    if (!any) return false;
    *out = v;
    return true;
}
