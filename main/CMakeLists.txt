idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif lwip BSP espressif__esp32-camera esp_driver_spi)
