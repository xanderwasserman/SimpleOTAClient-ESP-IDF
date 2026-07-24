/* NOTE: duplicated between examples/basic and examples/signed on purpose:
 * registry-installed examples must be self-contained (create-project-from-
 * example copies one example directory only). Keep both copies in sync. */
/* Minimal self-contained Wi-Fi station bring-up for the SimpleOTA examples.
 * (Deliberately not IDF's protocol_examples_common, which is unavailable to
 * out-of-tree projects.) */

#ifndef EXAMPLE_WIFI_CONNECT_H
#define EXAMPLE_WIFI_CONNECT_H

#include <stdbool.h>
#include "esp_err.h"

/** Connect to the SSID from menuconfig; blocks until an IP is acquired
 *  (retrying forever). */
esp_err_t example_wifi_connect(void);

/** True while the station is connected with an IP address. */
bool example_wifi_is_connected(void);

#endif
