/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "flash_uploader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "video_packetizer.h"

static const char *TAG = "flash_uploader";

#ifndef CONFIG_P4_FLASH_MOUNT_PATH
#define CONFIG_P4_FLASH_MOUNT_PATH "/spiffs"
#endif
#ifndef CONFIG_P4_FLASH_UPLOAD_ENABLE
#define CONFIG_P4_FLASH_UPLOAD_ENABLE 0
#endif
#ifndef CONFIG_P4_FLASH_UPLOAD_PERIOD_MS
#define CONFIG_P4_FLASH_UPLOAD_PERIOD_MS 1000
#endif

static bool parse_meta(const char *name, video_frame_meta_t *meta)
{
    if (!name || !meta) return false;

    unsigned clip_id = 0;
    unsigned frame_id = 0;
    unsigned ts_ms = 0;
    unsigned width = 0;
    unsigned height = 0;

    int matched = sscanf(
        name,
        "clip%u_frame%u_ts%u_w%u_h%u.jpg",
        &clip_id, &frame_id, &ts_ms, &width, &height
    );

    if (matched != 5) return false;

    meta->clip_id = clip_id;
    meta->frame_id = frame_id;
    meta->ts_ms = ts_ms;
    meta->width = (uint16_t)width;
    meta->height = (uint16_t)height;
    return true;
}

static esp_err_t publish_file(const char *path, const video_frame_meta_t *meta)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;

    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (read != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t err = video_packetizer_publish_jpeg(meta, buf, (uint32_t)read);
    free(buf);
    return err;
}

static void uploader_task(void *arg)
{
    (void)arg;
    const TickType_t delay = pdMS_TO_TICKS(CONFIG_P4_FLASH_UPLOAD_PERIOD_MS);

    while (true) {
        DIR *d = opendir(CONFIG_P4_FLASH_MOUNT_PATH);
        if (!d) {
            ESP_LOGW(TAG, "No flash directory");
            vTaskDelay(delay);
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            video_frame_meta_t meta;
            if (!parse_meta(ent->d_name, &meta)) {
                continue;
            }

            char path[128];
            snprintf(path, sizeof(path), "%s/%s", CONFIG_P4_FLASH_MOUNT_PATH, ent->d_name);

            if (publish_file(path, &meta) == ESP_OK) {
                unlink(path);
            }
        }

        closedir(d);
        vTaskDelay(delay);
    }
}

esp_err_t flash_uploader_start(void)
{
    if (!CONFIG_P4_FLASH_UPLOAD_ENABLE) return ESP_OK;

    BaseType_t ok = xTaskCreate(
        uploader_task,
        "flash_uploader",
        4096,
        NULL,
        5,
        NULL
    );

    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
