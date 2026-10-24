/**
 * @file
 * @brief The URLs the client may use: the server base URL for
 * check-ins and status posts, and where a download redirect may lead.
 * Everything is https://; the offer's own download URL is checked on the
 * apply path.
 *
 * No platform types, so the host tests can include this header.
 */

#ifndef SOTA_URL_H
#define SOTA_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/** The SimpleOTA platform, used when the application sets no base URL. */
#define SOTA_DEFAULT_SERVER_URL "https://simpleota.com"
/** Longest base URL accepted, without the terminating NUL. */
#define SOTA_SERVER_URL_MAX_LEN 256u

/** Why a server URL was accepted or refused. */
typedef enum {
    SOTA_URL_OK = 0,        /**< Usable. */
    SOTA_URL_NOT_HTTPS = 1, /**< A plain http:// URL. */
    SOTA_URL_MALFORMED = 2, /**< Empty, too long, no host, or bad characters. */
} sota_url_status_t;

/** True when url starts with scheme, ignoring the case of the URL. */
static inline bool sota_url_has_scheme(const char *url, const char *scheme)
{
    for (; *scheme; ++url, ++scheme) {
        char c = *url >= 'A' && *url <= 'Z' ? (char)(*url - 'A' + 'a') : *url;
        if (c != *scheme) return false;
    }
    return true;
}

/** True when url has no spaces or control characters, which could split
 *  a request line. */
static inline bool sota_url_chars_ok(const char *url)
{
    for (; *url; ++url) {
        unsigned char c = (unsigned char)*url;
        if (c <= 0x20u || c == 0x7fu) return false;
    }
    return true;
}

/**
 * An https:// URL with a host, at most SOTA_SERVER_URL_MAX_LEN characters,
 * and no spaces or control characters.
 */
static inline sota_url_status_t sota_validate_server_url(const char *url)
{
    if (!url || !url[0]) return SOTA_URL_MALFORMED;
    size_t len = strlen(url);
    if (len > SOTA_SERVER_URL_MAX_LEN) return SOTA_URL_MALFORMED;
    if (!sota_url_chars_ok(url)) return SOTA_URL_MALFORMED;
    if (sota_url_has_scheme(url, "http://") && !sota_url_has_scheme(url, "https://"))
        return SOTA_URL_NOT_HTTPS;
    if (!sota_url_has_scheme(url, "https://")) return SOTA_URL_MALFORMED;
    const char *slash = strchr(url + 8, '/');
    return (slash ? (size_t)(slash - url) : len) == 8u ? SOTA_URL_MALFORMED : SOTA_URL_OK;
}

/** The status as a short token for logs: "ok", "not_https" or "malformed". */
static inline const char *sota_url_status_name(int status)
{
    if (status == SOTA_URL_NOT_HTTPS) return "not_https";
    return status == SOTA_URL_OK ? "ok" : "malformed";
}

/**
 * Where a download redirect from base leads, into out. location is an
 * absolute https:// URL, or a path starting with '/' on base's host. A
 * redirect to plain HTTP is SOTA_URL_NOT_HTTPS; out is untouched unless
 * the result is SOTA_URL_OK. out must not overlap base or location.
 */
static inline sota_url_status_t sota_redirect_target(const char *base, const char *location,
                                                     char *out, size_t cap)
{
    if (!base || !location || !location[0] || !out || !sota_url_chars_ok(location)) {
        return SOTA_URL_MALFORMED;
    }
    size_t origin = 0;
    if (location[0] == '/' && location[1] != '/') {
        if (!sota_url_has_scheme(base, "https://")) return SOTA_URL_NOT_HTTPS;
        const char *slash = strchr(base + 8, '/');
        origin = slash ? (size_t)(slash - base) : strlen(base);
    } else if (sota_url_has_scheme(location, "http://")) {
        return SOTA_URL_NOT_HTTPS;
    } else if (!sota_url_has_scheme(location, "https://") || location[8] == '/' || !location[8]) {
        return SOTA_URL_MALFORMED;
    }
    size_t len = strlen(location);
    if (origin + len >= cap) return SOTA_URL_MALFORMED;
    memcpy(out, base, origin);
    memcpy(out + origin, location, len + 1);
    return SOTA_URL_OK;
}

/**
 * base + path into out, with trailing slashes of base removed. Returns
 * false, leaving out untouched, when it does not fit.
 */
static inline bool sota_build_api_url(const char *base, const char *path, char *out,
                                      size_t out_len)
{
    if (!base || !path || !out || out_len == 0) return false;
    size_t n = strlen(base);
    while (n > 8u && base[n - 1] == '/') --n;
    size_t plen = strlen(path);
    if (n >= out_len || plen > (out_len - 1u) - n) return false;
    memcpy(out, base, n);
    memcpy(out + n, path, plen + 1u);
    return true;
}

#endif /* SOTA_URL_H */
