#include "mqtt_video.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_video";

static esp_mqtt_client_handle_t s_client;

esp_err_t mqtt_video_init(void)
{
    if (CONFIG_P4_MQTT_BROKER_URI[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_P4_MQTT_BROKER_URI,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "MQTT started: %s", CONFIG_P4_MQTT_BROKER_URI);
    return ESP_OK;
}

esp_err_t mqtt_video_publish_chunk(const uint8_t *data, size_t len)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;

    int msg_id = esp_mqtt_client_publish(
        s_client,
        CONFIG_P4_MQTT_TOPIC,
        (const char *)data,
        (int)len,
        0,  // qos
        0   // retain
    );

    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
