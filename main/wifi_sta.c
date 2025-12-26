// main/wifi_sta.c
//
// ESP32-P4 note:
// ESP32-P4 has no native Wi-Fi radio. If your board does not include a supported
// external Wi-Fi companion, esp_wifi will not be able to scan/connect.
// This file is a SAFE "doesn't crash" implementation:
//
// - It never calls esp_read_mac(ESP_MAC_WIFI_STA) (your log shows ESP_ERR_NOT_SUPPORTED)
// - It sets WPA2 minimum authmode threshold
// - It optionally locks to a BSSID/channel if you provide them
// - It will try to connect and report failures, but returns ESP_ERR_NOT_SUPPORTED
//   if Wi-Fi isn't actually present on the hardware.
//
// Paste this whole file as main/wifi_sta.c

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi_sta.h"
#include "sdkconfig.h"

#ifndef WIFI_STA_MAXIMUM_RETRY
#define WIFI_STA_MAXIMUM_RETRY 10
#endif

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static bool parse_bssid(const char *str, uint8_t out[6])
{
    if (!str || !out) return false;
    int vals[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (vals[i] < 0 || vals[i] > 255) return false;
        out[i] = (uint8_t)vals[i];
    }
    return true;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> esp_wifi_connect()");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d)", disc->reason);
        } else {
            ESP_LOGW(TAG, "Disconnected");
        }

        if (s_retry_num < WIFI_STA_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi... (%d/%d)", s_retry_num, WIFI_STA_MAXIMUM_RETRY);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_connect() retry failed: %s", esp_err_to_name(err));
            }
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event) {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Got IP");
        }
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

// Public API
esp_err_t wifi_sta_start_and_wait(void)
{
    // Make sure netif/event loop exist (safe to call multiple times)
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    // Init Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        // This matches what you saw: esp_read_mac(WIFI_STA) not supported, scan finds 0 APs, etc.
        ESP_LOGW(TAG, "esp_wifi_init(): Wi-Fi not supported. Enable esp_wifi_remote/esp_extconn for ESP32-P4.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_ERROR_CHECK(err);

    // Register handlers
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Configure credentials
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    // SSID/PASS (truncate safely)
    if (CONFIG_P4_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty. Set it in menuconfig.");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy((char *)wifi_config.sta.ssid, CONFIG_P4_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_P4_WIFI_PASS, sizeof(wifi_config.sta.password));

    // IMPORTANT: force WPA2 minimum (what you asked)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Optional: lock to BSSID + channel
#if CONFIG_P4_WIFI_USE_BSSID_LOCK
    {
        uint8_t bssid[6];
        if (!parse_bssid(CONFIG_P4_WIFI_BSSID, bssid)) {
            ESP_LOGE(TAG, "Invalid BSSID string: \"%s\"", CONFIG_P4_WIFI_BSSID);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(wifi_config.sta.bssid, bssid, 6);
        wifi_config.sta.bssid_set = true;
        wifi_config.sta.channel = CONFIG_P4_WIFI_CHANNEL;

        ESP_LOGI(TAG,
                 "Connecting to SSID=\"%s\" BSSID=%02X:%02X:%02X:%02X:%02X:%02X ch=%d (WPA2 min)",
                 CONFIG_P4_WIFI_SSID,
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 CONFIG_P4_WIFI_CHANNEL);
    }
#else
    ESP_LOGI(TAG, "Connecting to SSID=\"%s\" (WPA2 min)", CONFIG_P4_WIFI_SSID);
#endif

    // Recommended for STA-only apps
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connect or fail
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi failed after %d retries", WIFI_STA_MAXIMUM_RETRY);
        return ESP_FAIL;
    }

    // Should never happen
    return ESP_FAIL;
}
