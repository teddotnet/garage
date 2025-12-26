#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t mqtt_video_init(void);
esp_err_t mqtt_video_publish_chunk(const uint8_t *data, size_t len);
