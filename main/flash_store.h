/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t flash_store_init(void);
esp_err_t flash_store_write_frame(uint32_t clip_id,
                                  uint32_t frame_id,
                                  uint32_t ts_ms,
                                  uint16_t width,
                                  uint16_t height,
                                  const uint8_t *data,
                                  size_t len);

#ifdef __cplusplus
}
#endif

#endif
