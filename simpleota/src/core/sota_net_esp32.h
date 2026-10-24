/**
 * @file
 * @brief The core's sota_net.h on esp_http_client.
 *
 * The engine's user pointer must point at a sota_net_esp32_t, or at a struct
 * whose first member is one.
 */

#ifndef SOTA_NET_ESP32_H
#define SOTA_NET_ESP32_H

#include "esp_err.h"
#include "esp_http_client.h"

/** The transport's TLS settings and its open download. */
typedef struct {
    const char *cert_pem;                       /**< Pinned CA, or NULL. */
    esp_err_t (*crt_bundle_attach)(void *conf); /**< Used without cert_pem. */
    esp_http_client_handle_t dl;                /**< The open download. */
} sota_net_esp32_t;

#endif /* SOTA_NET_ESP32_H */
