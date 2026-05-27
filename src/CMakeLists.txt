idf_component_register(
    SRCS
        "main.c"
        "led_status.c"
        "wifi_manager.c"
        "wifi_config.c"
        "nvs_store.c"
        "http_server.c"
        "anemometer.c"
        "mqtt_bridge.c"
    INCLUDE_DIRS "."
)

target_add_binary_data(${COMPONENT_LIB} "index.html" TEXT)
