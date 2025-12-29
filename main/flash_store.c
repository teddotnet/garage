/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "flash_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"

static const char *TAG = "flash_store";

#ifndef CONFIG_P4_FLASH_MOUNT_PATH
#define CONFIG_P4_FLASH_MOUNT_PATH "/spiffs"
#endif

esp_err_t flash_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_P4_FLASH_MOUNT_PATH,
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }

    return ESP_OK;
}

esp_err_t flash_store_write_frame(uint32_t clip_id,
                                  uint32_t frame_id,
                                  uint32_t ts_ms,
                                  uint16_t width,
                                  uint16_t height,
                                  const uint8_t *data,
                                  size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    char path[128];
    snprintf(path, sizeof(path),
             "%s/clip%u_frame%u_ts%u_w%u_h%u.jpg",
             CONFIG_P4_FLASH_MOUNT_PATH,
             (unsigned)clip_id,
             (unsigned)frame_id,
             (unsigned)ts_ms,
             (unsigned)width,
             (unsigned)height);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write %s (%u/%u)", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}
