/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VIDEO_PACKETIZER_H
#define VIDEO_PACKETIZER_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t clip_id;
    uint32_t frame_id;
    uint32_t ts_ms;
    uint16_t width;
    uint16_t height;
} video_frame_meta_t;

esp_err_t video_packetizer_publish_jpeg(const video_frame_meta_t *meta,
                                        const uint8_t *jpeg,
                                        uint32_t jpeg_size);

#ifdef __cplusplus
}
#endif

#endif
