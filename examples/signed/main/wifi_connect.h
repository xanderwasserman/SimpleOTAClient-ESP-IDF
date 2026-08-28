/* NOTE: duplicated between examples/basic and examples/signed on purpose:
 * registry-installed examples must be self-contained (create-project-from-
 * example copies one example directory only). Keep both copies in sync. */
/* Minimal self-contained network bring-up for the SimpleOTA examples.
 * Default is Wi-Fi STA. CONFIG_EXAMPLE_USE_OPENETH selects OpenETH + DHCP
 * for Espressif QEMU, which does not emulate the ESP32 Wi-Fi radio.
 * (Deliberately not IDF's protocol_examples_common, which is unavailable to
 * out-of-tree projects.) */

#ifndef EXAMPLE_WIFI_CONNECT_H
#define EXAMPLE_WIFI_CONNECT_H

#include <stdbool.h>
#include "esp_err.h"

/** Connect to the SSID from menuconfig, or bring up OpenETH + DHCP when
 *  CONFIG_EXAMPLE_USE_OPENETH is set. Blocks until an IP is acquired
 *  (retrying forever on Wi-Fi). */
esp_err_t example_wifi_connect(void);

/** True while the link is connected with an IP address. */
bool example_wifi_is_connected(void);

/** Stable SimpleOTA device_id. NULL on the Wi-Fi path (component default
 *  is the Wi-Fi STA MAC). Ethernet MAC, or "qemu-esp32", on OpenETH. */
const char *example_net_device_id(void);

#endif
