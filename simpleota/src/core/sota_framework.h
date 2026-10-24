/**
 * @file
 * @brief What differs between the two builds of the client:
 * the ESP-IDF component, configured by its Kconfig, and the Arduino
 * library, which links against Arduino-ESP32's prebuilt ESP-IDF and has
 * neither a Kconfig nor the certificate bundle.
 */

#ifndef SOTA_FRAMEWORK_H
#define SOTA_FRAMEWORK_H

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

/* The component's Kconfig defaults, for the Arduino library. Kconfig always
 * sets these on ESP-IDF; on Arduino a PlatformIO build flag can. */
#ifndef CONFIG_SIMPLEOTA_TASK_STACK_SIZE
#define CONFIG_SIMPLEOTA_TASK_STACK_SIZE 12288 /**< Managed, polling, rollback and reboot task stack. */
#endif
#ifndef CONFIG_SIMPLEOTA_TASK_PRIORITY
#define CONFIG_SIMPLEOTA_TASK_PRIORITY 5 /**< Managed task priority. */
#endif
#ifndef CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS
#define CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS 15000 /**< Per-request and download-stall timeout. */
#endif
#ifndef CONFIG_SIMPLEOTA_HTTP_TX_BUFFER
#define CONFIG_SIMPLEOTA_HTTP_TX_BUFFER 2048 /**< Fits a pre-signed download URL's request line. */
#endif

#if defined(ARDUINO)
#define SOTA_FRAMEWORK_NAME "arduino"
/* Arduino-ESP32 enables bootloader rollback, so this only matters for a
 * core built without it: the trial still rolls back, in software. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE && !defined(CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK)
#define CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK 1
#endif
#else
/** The framework reported on check-ins: "arduino" or "esp_idf". */
#define SOTA_FRAMEWORK_NAME "esp_idf"
#endif

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK
#define SOTA_ROLLBACK 1
#else
/** 1 when new images run as trials: rolled back by the bootloader, or
 *  without it, by the client. */
#define SOTA_ROLLBACK 0
#endif

/**
 * TLS trust when the application gives none: a CA certificate to pin, or
 * the certificate bundle's attach function. False when there is neither.
 */
bool sota_framework_default_trust(const char **cert_pem,
                                  esp_err_t (**crt_bundle_attach)(void *conf));

/** Called by simpleota_init(): make the client's warnings visible. */
void sota_framework_init_logging(const char *tag);

#endif /* SOTA_FRAMEWORK_H */
