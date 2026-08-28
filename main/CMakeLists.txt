# Sources are added here as each upstream module lands - see docs/PORTING.md
# for the file-by-file map from raomin/ESPAltherma to this tree.
idf_component_register(
    SRCS
        "main.c"
        "althermaserial.c"     # <- include/comm.h  (UART, CRC, protocol I/S)
        "converters.cpp"       # <- include/converters.h + include/def/*.h
        "wifi.c"               # <- src/main.cpp    (setup_wifi/checkWifi)
        "mqtt.c"               # <- include/mqtt.h + mqttserial.h (-> esp-mqtt)
        "settings.c"           # <- src/setup.h     (compile-time -> NVS)
        "web_server.c"         # new: config UI, WiFi diag, values, OTA upload
        # "homeassistant.cpp"  # <- src/homeassistant.cpp (near-verbatim)
        # "control.c"          # <- thermostat / smart grid / safety relay
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mqtt
        esp_http_server
        esp_timer
        app_update
        esp_https_ota
        esp_http_client
        log
)
