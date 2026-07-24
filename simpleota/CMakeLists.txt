idf_component_register(
    SRCS "src/simpleota.c"
         "src/sota_json.c"
         "src/sota_nvs.c"
         "src/sota_signing.c"
         "src/vendor/monocypher.c"
         "src/vendor/monocypher-ed25519.c"
         "src/vendor/sota_sha256.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/vendor"
    PRIV_REQUIRES esp_http_client
                  mbedtls
                  app_update
                  esp_partition
                  nvs_flash
                  esp_timer
                  esp_app_format
                  esp_hw_support
)
