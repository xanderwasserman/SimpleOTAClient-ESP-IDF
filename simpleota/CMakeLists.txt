# The watchdog HAL component is hal through IDF 5, and esp_hal_wdt from IDF 6.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(sota_priv_wdt esp_hal_wdt)
else()
    set(sota_priv_wdt hal)
endif()

# The client is the shared core, vendored as plain files by its sync script:
# src/core, and its public header in include.
file(GLOB sota_core_srcs "${CMAKE_CURRENT_LIST_DIR}/src/core/*.c")

idf_component_register(
    SRCS ${sota_core_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/core"
    PRIV_REQUIRES esp_http_client
                  mbedtls
                  app_update
                  esp_partition
                  nvs_flash
                  esp_timer
                  esp_hw_support
                  ${sota_priv_wdt}
)

# Shown when the project is configured, not compiled, so -Werror builds pass.
if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    if(CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK)
        message(WARNING "simpleota: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is off, so "
                "failed updates are rolled back in software, which misses an image "
                "that crashes before simpleota_init() on every boot. Enable it on "
                "devices that have not shipped yet: the bootloader is not updated over the air.")
    else()
        message(WARNING "simpleota: rollback is off. Every update is accepted on its first boot.")
    endif()
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SOTA_DL_CHUNK=${CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK}u
    SOTA_LOG_LEVEL=${CONFIG_LOG_MAXIMUM_LEVEL})
