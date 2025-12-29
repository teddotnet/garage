/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "video_packetizer.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_video.h"

static const char *TAG = "pkt";

#define VID_MAGIC 0x56494430u   // 'VID0'
#define CHUNK_MAX 2048

// FourCC helper (MJPG)
#define FCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define FOURCC_MJPG FCC('M','J','P','G')

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t clip_id;
    uint32_t frame_id;
    uint32_t ts_ms;
    uint16_t chunk_id;
    uint16_t chunk_count;
    uint32_t frame_size;
    uint32_t fourcc;
    uint16_t width;
    uint16_t height;
} vid_hdr_t;
#pragma pack(pop)

esp_err_t video_packetizer_publish_jpeg(const video_frame_meta_t *meta,
                                        const uint8_t *jpeg,
                                        uint32_t jpeg_size)
{
    if (!meta || !jpeg || jpeg_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t chunk_count = (jpeg_size + CHUNK_MAX - 1) / CHUNK_MAX;

    for (uint16_t chunk_id = 0; chunk_id < chunk_count; chunk_id++) {
        size_t off = (size_t)chunk_id * CHUNK_MAX;
        size_t remain = jpeg_size - off;
        size_t take = remain > CHUNK_MAX ? CHUNK_MAX : remain;

        uint8_t pkt[sizeof(vid_hdr_t) + CHUNK_MAX];
        vid_hdr_t hdr = {
            .magic = VID_MAGIC,
            .clip_id = meta->clip_id,
            .frame_id = meta->frame_id,
            .ts_ms = meta->ts_ms,
            .chunk_id = chunk_id,
            .chunk_count = chunk_count,
            .frame_size = jpeg_size,
            .fourcc = FOURCC_MJPG,
            .width = meta->width,
            .height = meta->height,
        };

        memcpy(pkt, &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), jpeg + off, take);

        esp_err_t err = mqtt_video_publish_chunk(pkt, sizeof(hdr) + take);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT publish failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}
