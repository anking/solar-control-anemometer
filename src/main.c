#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "config.h"
#include "led_status.h"
#include "wifi_manager.h"
#include "wifi_config.h"
#include "http_server.h"
#include "anemometer.h"
#include "mqtt_bridge.h"

static const char *TAG = "main";

static void status_broadcaster_task(void *arg)
{
    char buf[448];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        anemometer_reading_t r;
        anemometer_get(&r);

        int len = snprintf(buf, sizeof(buf),
            "{\"valid\":%s,\"voltage_v\":%.3f,\"raw_mv\":%d,\"peak_mv\":%d,"
            "\"saturated\":%s,"
            "\"mph\":%.2f,\"kmh\":%.2f,"
            "\"mph_avg\":%.2f,\"kmh_avg\":%.2f,"
            "\"gust_mph\":%.2f,"
            "\"mph_per_volt\":%.2f,\"zero_offset_mv\":%d,"
            "\"samples\":%lu}",
            r.valid ? "true" : "false",
            r.voltage_v, r.raw_mv, r.peak_mv,
            r.saturated ? "true" : "false",
            r.wind_mph, r.wind_kmh, r.wind_mph_avg, r.wind_kmh_avg, r.gust_mph,
            r.mph_per_volt, r.zero_offset_mv,
            (unsigned long)r.sample_count);

        if (len > 0 && len < (int)sizeof(buf)) {
            http_server_ws_broadcast_status(buf, (size_t)len);
        }

        // Push the same reading to MQTT (no-op if not connected).
        mqtt_bridge_publish_reading(&r);
    }
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_BROWNOUT) {
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Anemometer ESP32-C3 v%s (cores: %d, rev: %d)",
             FIRMWARE_VERSION, chip_info.cores, chip_info.revision);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }

    // LED first so we can show WiFi state during init.
    ret = led_status_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(ret));

    // Anemometer ISR + sampler.
    ret = anemometer_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Anemometer init failed: %s", esp_err_to_name(ret));

    // WiFi (STA + AP fallback hotspot).
    ret = wifi_manager_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Wi-Fi manager init failed: %s", esp_err_to_name(ret));

    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    if (wifi_config_get(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
        ESP_LOGI(TAG, "Connecting to saved Wi-Fi: %s", saved_ssid);
        wifi_manager_start(saved_ssid, saved_pass);
    } else {
        ESP_LOGW(TAG, "No saved Wi-Fi credentials - AP-only mode");
        wifi_manager_start("", "");
    }

    // HTTP server (dashboard + APIs + /ws).
    ret = http_server_start();
    if (ret != ESP_OK) ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(ret));

    // MQTT scaffold (only starts if a host was previously saved).
    mqtt_bridge_init();

    // 1 Hz WebSocket push of current wind reading.
    xTaskCreate(status_broadcaster_task, "ws_status", 4096, NULL, 5, NULL);

    wifi_status_t wifi_status;
    wifi_manager_get_status(&wifi_status);
    if (wifi_status.connected) {
        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "  Dashboard: http://%s/", wifi_status.ip);
        if (wifi_status.hostname[0]) {
            ESP_LOGI(TAG, "  mDNS:      http://%s.local/", wifi_status.hostname);
        }
        ESP_LOGI(TAG, "======================================");
    } else if (wifi_status.ap_active) {
        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "  Join hotspot: %s", wifi_status.ap_ssid);
        ESP_LOGI(TAG, "  Dashboard:    http://%s/", wifi_status.ap_ip);
        ESP_LOGI(TAG, "======================================");
    }

    // Idle loop — periodic heartbeat log.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        wifi_status_t s;
        wifi_manager_get_status(&s);
        anemometer_reading_t r;
        anemometer_get(&r);
        ESP_LOGI(TAG, "Heartbeat: wifi=%s ip=%s v=%.3fV wind=%.1f mph (avg %.1f, gust %.1f) heap=%lu",
                 s.connected ? "ok" : "down", s.ip,
                 r.voltage_v, r.wind_mph, r.wind_mph_avg, r.gust_mph,
                 (unsigned long)esp_get_free_heap_size());
    }
}
