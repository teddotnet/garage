#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t capture_video_seconds(int seconds);
esp_err_t record_video_seconds_to_flash(int seconds, uint32_t *out_frames, float *out_fps);
