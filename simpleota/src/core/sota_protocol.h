/**
 * @file
 * @brief The SimpleOTA HTTP API's request bodies and the check
 * response.
 *
 *   POST /api/v1/ota/check/   check-in; the response may offer an update
 *   POST /api/v1/ota/status/  one lifecycle event of a deployment
 *
 * Request bodies are written into a caller's buffer, with every string
 * value escaped, so a value from the server or the integrator can never
 * break the JSON the server parses. Nothing here allocates.
 */

#ifndef SOTA_PROTOCOL_H
#define SOTA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sota_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTA_PATH_CHECK "/api/v1/ota/check/"   /**< Check-in path, after the base URL. */
#define SOTA_PATH_STATUS "/api/v1/ota/status/" /**< Status-event path, after the base URL. */

/** What the device says about itself on every check-in. NULL members are
 *  left out of the body; device_id, framework and chip_family are required. */
typedef struct {
    const char *device_id;          /**< Stable per-unit identifier. */
    const char *framework;          /**< "arduino" or "esp_idf" */
    const char *chip_family;        /**< "esp32", "esp32s3", ... */
    const char *board_id;           /**< Board model. */
    const char *hardware_revision;  /**< Board revision. */
    const char *partition_profile;  /**< Partition layout name. */
    const char *channel;            /**< Release channel. */
    const char *security_mode;      /**< "basic" or "signed", or NULL to report none. */
    const char *labels_json;        /**< A JSON object, embedded verbatim. */
    uint8_t nvs_schema_version;     /**< On-device data schema the firmware expects. */
} sota_device_info_t;

/** An update the server offered. */
typedef struct {
    char *url;                      /**< Caller's buffer: pre-signed URLs run to 1.5 KB. */
    size_t url_cap;                 /**< Size of the url buffer. */
    char checksum[SOTA_HASH_HEX_LEN + 1]; /**< sha256 hex, lowercased */
    char deployment[64];            /**< Empty when the server sent none. */
    char version[SOTA_VERSION_MAX + 1];   /**< Empty when absent or too long. */
    char key_id[64];                /**< signing_key_id, empty when absent. */
    uint32_t build;                 /**< The build number SimpleOTA assigned. */
    uint32_t size;                  /**< 0 when the server sent none. */
    bool signed_offer;              /**< security_mode is "signed". */
    bool has_signature;             /**< signature decoded to 64 ed25519 bytes. */
    uint8_t signature[64];          /**< The raw signature, when has_signature. */
} sota_offer_t;

/** What a check response says. */
typedef enum {
    SOTA_CHECK_NO_UPDATE,   /**< update_available is false or missing. */
    SOTA_CHECK_OFFER,       /**< *offer holds an update. */
    SOTA_CHECK_MALFORMED,   /**< Offered, but url, checksum or build is unusable. */
} sota_check_result_t;

/**
 * The check-in body. version_label is the label to report (the configured
 * one, else the stored one), NULL or empty to leave it out. Returns false
 * when the body does not fit in cap bytes.
 */
bool sota_protocol_check_body(const sota_device_info_t *dev, const sota_identity_t *live,
                              const char *version_label, char *out, size_t cap);

/** One status event's body. reason NULL leaves it out. False when it does
 *  not fit. */
bool sota_protocol_status_body(const char *device_id, const char *deployment,
                               const char *event, uint32_t build, const char *reason,
                               char *out, size_t cap);

/**
 * Read a 2xx check response. offer->url and url_cap must be set; every
 * other member is overwritten. A signature that is not base64 of 64 bytes,
 * or names an algorithm other than ed25519, leaves has_signature false, so
 * the signed-firmware gate fails closed when keys are pinned.
 */
sota_check_result_t sota_protocol_parse_check(const char *resp, sota_offer_t *offer);

/** The server's "status" when no update is offered ("up_to_date", ...);
 *  false when absent. */
bool sota_protocol_check_status(const char *resp, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_PROTOCOL_H */
