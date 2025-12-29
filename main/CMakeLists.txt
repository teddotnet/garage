idf_component_register(
    SRCS "main.c"
         "ethernet.c"
         "mqtt_video.c"
         "video_packetizer.c"
         "video_streamer.c"
         "flash_store.c"
         "flash_uploader.c"
         "app_video.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_eth esp_netif esp_wifi mqtt nvs_flash esp_driver_jpeg spiffs esp_video
)
