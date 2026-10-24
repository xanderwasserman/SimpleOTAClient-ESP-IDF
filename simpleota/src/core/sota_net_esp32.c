/**
 * sota_net_esp32.c - the core's HTTP transport (sota_net.h) on
 * esp_http_client, for both the ESP-IDF component and the Arduino library.
 */

#include "sota_net_esp32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sota_framework.h"

#include "sota_net.h"

static void client_config(const sota_net_esp32_t *n, const char *url,
                          esp_http_client_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS;
    cfg->buffer_size = 2048;
    /* Pre-signed URLs run to 1.5 KB; the default 512 cannot hold the
     * request line. */
    cfg->buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER;
    if (n->cert_pem) {
        cfg->cert_pem = n->cert_pem;
    } else {
        cfg->crt_bundle_attach = n->crt_bundle_attach;
    }
}

int sota_net_post(void *user, const char *url, const char *token, const char *body,
                  char *resp, size_t cap)
{
    /* Sized to the token: a fixed buffer would fail every request for a
     * long token as if the network were down. */
    size_t auth_len = strlen("Bearer ") + strlen(token) + 1;
    char *auth = malloc(auth_len);
    if (!auth) return -1;
    snprintf(auth, auth_len, "Bearer %s", token);

    esp_http_client_config_t cfg;
    client_config((const sota_net_esp32_t *)user, url, &cfg);
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(auth);
        return -1;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");

    int status = -1;
    size_t len = strlen(body);
    if (esp_http_client_open(c, (int)len) == ESP_OK &&
        esp_http_client_write(c, body, (int)len) == (int)len &&
        esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        size_t total = 0;
        while (resp && total + 1 < cap) {
            int r = esp_http_client_read(c, resp + total, (int)(cap - 1 - total));
            if (r <= 0) break;
            total += (size_t)r;
        }
        if (resp && cap) resp[total] = '\0';
    }
    esp_http_client_cleanup(c);
    free(auth);
    return status;
}

/* No Authorization header: the pre-signed URL carries its own credentials,
 * and the token must not reach the storage host. A redirect is handed back
 * to the core, which checks where it leads. */
int sota_net_dl_open(void *user, const char *url, int64_t *len, char *location,
                     size_t location_cap)
{
    sota_net_esp32_t *n = (sota_net_esp32_t *)user;
    esp_http_client_config_t cfg;
    client_config(n, url, &cfg);
    cfg.keep_alive_enable = true;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return 0;
    }
    *len = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* set_redirection resolves the Location header against url. */
        if (sota_net_is_redirect(status) && location_cap &&
            (esp_http_client_set_redirection(c) != ESP_OK ||
             esp_http_client_get_url(c, location, (int)location_cap) != ESP_OK)) {
            location[0] = '\0';
        }
        esp_http_client_cleanup(c);
        return status;
    }
    if (*len <= 0) *len = -1;
    n->dl = c;
    return status;
}

int sota_net_dl_read(void *user, uint8_t *buf, size_t cap)
{
    sota_net_esp32_t *n = (sota_net_esp32_t *)user;
    /* Blocks up to the HTTP timeout; the core reports an error after that
     * long without a byte as download_stalled. */
    int r = esp_http_client_read(n->dl, (char *)buf, (int)cap);
    if (r > 0) return r;
    if (r == 0 && esp_http_client_is_complete_data_received(n->dl)) return 0;
    return SOTA_NET_ERROR;
}

void sota_net_dl_close(void *user)
{
    sota_net_esp32_t *n = (sota_net_esp32_t *)user;
    if (n->dl) esp_http_client_cleanup(n->dl);
    n->dl = NULL;
}
