/* NOTE: duplicated between examples/basic and examples/signed on purpose:
 * registry-installed examples must be self-contained (create-project-from-
 * example copies one example directory only). Keep both copies in sync. */
#include "wifi_connect.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#if CONFIG_EXAMPLE_USE_OPENETH
#include "esp_eth.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#else
#include "esp_wifi.h"
#endif

#if CONFIG_EXAMPLE_USE_OPENETH
static const char *TAG = "eth";
#else
static const char *TAG = "wifi";
#endif

#define NET_CONNECTED_BIT BIT0

static EventGroupHandle_t s_net_events;
static volatile bool s_connected;
#if CONFIG_EXAMPLE_USE_OPENETH
static char s_device_id[18];
#endif

#if CONFIG_EXAMPLE_USE_OPENETH
static void set_device_id_from_eth_mac(void) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
    } else {
        strlcpy(s_device_id, "qemu-esp32", sizeof(s_device_id));
    }
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id,
                         void *data) {
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_net_events, NET_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
    }
}

esp_err_t example_wifi_connect(void) {
    s_net_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
        return ESP_ERR_NO_MEM;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
#else
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#endif
    if (!mac || !phy) {
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));
    ESP_ERROR_CHECK(
        esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_eth_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &on_eth_event, NULL, NULL));

    set_device_id_from_eth_mac();
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "OpenETH DHCP ...");
    xEventGroupWaitBits(s_net_events, NET_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    return ESP_OK;
}

const char *example_net_device_id(void) {
    return s_device_id[0] ? s_device_id : "qemu-esp32";
}

#else /* !CONFIG_EXAMPLE_USE_OPENETH */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected; retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_net_events, NET_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP");
    }
}

esp_err_t example_wifi_connect(void) {
    s_net_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to %s ...", CONFIG_EXAMPLE_WIFI_SSID);
    xEventGroupWaitBits(s_net_events, NET_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    return ESP_OK;
}

const char *example_net_device_id(void) { return NULL; }

#endif /* CONFIG_EXAMPLE_USE_OPENETH */

bool example_wifi_is_connected(void) { return s_connected; }
