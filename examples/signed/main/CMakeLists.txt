idf_component_register(SRCS "main.c" "wifi_connect.c"
                       INCLUDE_DIRS ".")

if(CONFIG_EXAMPLE_USE_OPENETH)
    idf_component_optional_requires(PRIVATE esp_eth)
endif()
