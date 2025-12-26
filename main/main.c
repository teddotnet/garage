#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "ethernet.h"
#include "mqtt_video.h"
#include "video_streamer.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_EXT_CONN_ENABLE
#include "esp_extconn.h"
#endif

static const char *TAG = "app";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#ifdef CONFIG_ESP_EXT_CONN_ENABLE
    esp_extconn_config_t ext_config = ESP_EXTCONN_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_extconn_init(&ext_config));
#endif

    err = ethernet_start_and_wait();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mqtt_video_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(err));
        return;
    }

    err = capture_video_seconds(CONFIG_P4_CAPTURE_SECONDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video capture failed: %s", esp_err_to_name(err));
        return;
    }
}
