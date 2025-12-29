#include "video_streamer.h"
#include "mqtt_video.h"
#include "app_video.h"
#include "flash_store.h"
#include "video_packetizer.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "driver/jpeg_encode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sdkconfig.h"

static const char *TAG = "vid";

typedef struct {
    int video_fd;
    int64_t start_us;
    uint32_t clip_id;
    uint32_t frame_id;
    uint32_t width;
    uint32_t height;
    uint8_t *jpeg_buf;
    size_t jpeg_buf_size;
    jpeg_encoder_handle_t encoder;
    bool encoder_ready;
    bool record_to_flash;
} capture_ctx_t;

static capture_ctx_t s_cap;

static uint32_t new_clip_id(void) { return (uint32_t)esp_random(); }

static esp_err_t jpeg_encoder_init(uint32_t width, uint32_t height)
{
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };

    esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &s_cap.encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoder init failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t out_size = 0;
    jpeg_encode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    s_cap.jpeg_buf = jpeg_alloc_encoder_mem((size_t)width * height * 2, &out_cfg, &out_size);
    if (!s_cap.jpeg_buf || out_size == 0) {
        ESP_LOGE(TAG, "JPEG output buffer alloc failed");
        jpeg_del_encoder_engine(s_cap.encoder);
        s_cap.encoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cap.jpeg_buf_size = out_size;
    s_cap.width = width;
    s_cap.height = height;
    s_cap.encoder_ready = true;
    return ESP_OK;
}

static void jpeg_encoder_deinit(void)
{
    if (s_cap.encoder) {
        jpeg_del_encoder_engine(s_cap.encoder);
        s_cap.encoder = NULL;
    }
    if (s_cap.jpeg_buf) {
        free(s_cap.jpeg_buf);
        s_cap.jpeg_buf = NULL;
    }
    s_cap.jpeg_buf_size = 0;
    s_cap.encoder_ready = false;
}

static void camera_frame_cb(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len)
{
    (void)camera_buf_index;

    if (!s_cap.encoder_ready || s_cap.width != camera_buf_hes || s_cap.height != camera_buf_ves) {
        jpeg_encoder_deinit();
        if (jpeg_encoder_init(camera_buf_hes, camera_buf_ves) != ESP_OK) {
            ESP_LOGE(TAG, "JPEG encoder setup failed");
            return;
        }
    }

    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = CONFIG_P4_JPEG_SUBSAMPLE_420 ? JPEG_DOWN_SAMPLING_YUV420 : JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = CONFIG_P4_JPEG_QUALITY,
        .width = camera_buf_hes,
        .height = camera_buf_ves,
    };

    uint32_t jpeg_size = 0;
    esp_err_t err = jpeg_encoder_process(
        s_cap.encoder,
        &enc_cfg,
        camera_buf,
        camera_buf_len,
        s_cap.jpeg_buf,
        s_cap.jpeg_buf_size,
        &jpeg_size
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t ts_ms = (uint32_t)((esp_timer_get_time() - s_cap.start_us) / 1000);
    video_frame_meta_t meta = {
        .clip_id = s_cap.clip_id,
        .frame_id = s_cap.frame_id,
        .ts_ms = ts_ms,
        .width = (uint16_t)camera_buf_hes,
        .height = (uint16_t)camera_buf_ves,
    };

    if (s_cap.record_to_flash) {
        esp_err_t err = flash_store_write_frame(
            meta.clip_id, meta.frame_id, meta.ts_ms, meta.width, meta.height,
            s_cap.jpeg_buf, jpeg_size
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(err));
            return;
        }
    } else {
        esp_err_t err = video_packetizer_publish_jpeg(&meta, s_cap.jpeg_buf, jpeg_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT publish failed: %s", esp_err_to_name(err));
            return;
        }
    }

    s_cap.frame_id++;
}

static esp_err_t capture_common(int seconds, bool record_to_flash, uint32_t *out_frames, float *out_fps)
{
    const int frames_limit = CONFIG_P4_CAPTURE_FRAMES;
    const bool use_time_limit = seconds > 0;
    const bool use_frame_limit = frames_limit > 0;

    if (!use_time_limit && !use_frame_limit) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.clip_id = new_clip_id();
    s_cap.start_us = esp_timer_get_time();
    s_cap.record_to_flash = record_to_flash;

    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = 1,
                .scl_pin = 8,
                .sda_pin = 7,
            },
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };

    esp_video_init_config_t video_cfg = {
        .csi = &csi_config,
    };

    esp_err_t err = esp_video_init(&video_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    int fd = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT_RGB565);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        ESP_LOGW(TAG, "Try selecting a different camera sensor in menuconfig.");
        return ESP_FAIL;
    }
    s_cap.video_fd = fd;

    err = app_video_register_frame_operation_cb(camera_frame_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video callback register failed: %s", esp_err_to_name(err));
        app_video_close(fd);
        return err;
    }

    err = app_video_set_bufs(fd, 3, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video buffer setup failed: %s", esp_err_to_name(err));
        app_video_close(fd);
        return err;
    }

    err = app_video_stream_task_start(fd, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video stream start failed: %s", esp_err_to_name(err));
        app_video_close(fd);
        return err;
    }

    ESP_LOGI(TAG, "Capture start: clip_id=%" PRIu32 " seconds=%d frames=%d dev=%s mode=%s",
             s_cap.clip_id, seconds, frames_limit, ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
             record_to_flash ? "flash" : "mqtt");

    int64_t end_us = 0;
    if (use_time_limit) {
        end_us = s_cap.start_us + (int64_t)seconds * 1000000;
    }

    while (true) {
        if (use_frame_limit && s_cap.frame_id >= (uint32_t)frames_limit) {
            break;
        }
        if (use_time_limit && esp_timer_get_time() >= end_us) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    app_video_stream_task_stop(fd);
    app_video_wait_video_stop();

    ESP_LOGI(TAG, "Capture end: frames=%" PRIu32, s_cap.frame_id);

    app_video_close(fd);
    jpeg_encoder_deinit();

    if (out_frames) {
        *out_frames = s_cap.frame_id;
    }
    if (out_fps) {
        int64_t elapsed_us = esp_timer_get_time() - s_cap.start_us;
        *out_fps = elapsed_us > 0 ? (float)s_cap.frame_id * 1000000.0f / (float)elapsed_us : 0.0f;
    }

    return ESP_OK;
}

esp_err_t capture_video_seconds(int seconds)
{
    return capture_common(seconds, false, NULL, NULL);
}

esp_err_t record_video_seconds_to_flash(int seconds, uint32_t *out_frames, float *out_fps)
{
    return capture_common(seconds, true, out_frames, out_fps);
}
