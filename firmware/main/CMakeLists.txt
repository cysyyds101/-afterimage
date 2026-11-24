idf_component_register(SRCS "main.c" "voice_client.c" "net_sync.c" "font_cn_18.c" INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_websocket_client mbedtls spiffs esp_timer esp_http_server
             esp_http_client espressif__cjson esp_driver_gpio espressif__esp_new_jpeg
             espressif__esp_lcd_touch
             waveshare__esp32_s3_touch_amoled_2_06 lvgl__lvgl
    PRIV_REQUIRES esp_driver_usb_serial_jtag)
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
