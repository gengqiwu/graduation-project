set(src_dirs
    LED
    WIFI
    MIC
    YUYIN
    AI_CHAT
    SPEAKER
    LCD
    CAMERA)

set(include_dirs
    LED
    WIFI
    MIC
    YUYIN
    AI_CHAT
    SPEAKER
    LCD
    CAMERA)

set(requires
    driver
    esp_wifi
    esp_event
    esp_netif
    lwip
    nvs_flash
    esp_driver_i2s
    esp-sr
    led_strip
    esp_http_server
    esp_http_client
    esp-tls
    esp_websocket_client
    mbedtls
    json
    esp_driver_spi
    espressif__esp32-camera)

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)