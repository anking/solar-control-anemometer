#include "power_mgr.h"
#include "config.h"
#include "nvs_store.h"
#include "anemometer.h"
#include "mqtt_bridge.h"
#include "wifi_manager.h"
#include "http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "power_mgr";

#define NVS_KEY_MODE      "pwr_mode"
#define NVS_KEY_INTERVAL  "sleep_int"

// ---- RTC-retained backlog ---------------------------------------------------
// Survives deep sleep; cleared on a true power cycle (magic mismatch).
#define BACKLOG_MAGIC 0x57494e44u  // "WIND"
RTC_DATA_ATTR static uint32_t       s_rtc_magic;
RTC_DATA_ATTR static uint16_t       s_backlog_count;
RTC_DATA_ATTR static wind_summary_t s_backlog[SLEEP_BACKLOG_MAX];

static power_mode_t s_mode = POWER_MODE_ACTIVE;
static uint16_t     s_interval_s = SLEEP_DEFAULT_INTERVAL_S;

// Transient (not persisted): seconds of dashboard maintenance window requested
// via a {"ui":...} command, consumed once on the next sleep-mode wake.
static uint32_t     s_maint_window_s = 0;

static void backlog_init_if_cold(void)
{
    if (s_rtc_magic != BACKLOG_MAGIC) {
        s_rtc_magic = BACKLOG_MAGIC;
        s_backlog_count = 0;
    }
}

static void backlog_push(const wind_summary_t *s)
{
    if (s_backlog_count >= SLEEP_BACKLOG_MAX) {
        // Drop the oldest to make room — recent data is more useful.
        memmove(&s_backlog[0], &s_backlog[1],
                (SLEEP_BACKLOG_MAX - 1) * sizeof(wind_summary_t));
        s_backlog_count = SLEEP_BACKLOG_MAX - 1;
    }
    s_backlog[s_backlog_count++] = *s;
}

void power_mgr_init(void)
{
    backlog_init_if_cold();

    uint16_t v;
    if (nvs_store_get_u16(NVS_NS_DEVICE, NVS_KEY_MODE, &v) == ESP_OK) {
        s_mode = (v == POWER_MODE_SLEEP) ? POWER_MODE_SLEEP : POWER_MODE_ACTIVE;
    }
    if (nvs_store_get_u16(NVS_NS_DEVICE, NVS_KEY_INTERVAL, &v) == ESP_OK && v > 0) {
        s_interval_s = v;
    }
    ESP_LOGI(TAG, "Power mode: %s (sleep interval %us)",
             s_mode == POWER_MODE_SLEEP ? "SLEEP" : "ACTIVE", s_interval_s);
}

power_mode_t power_mgr_get_mode(void)        { return s_mode; }
uint16_t power_mgr_get_sleep_interval_s(void){ return s_interval_s; }

esp_err_t power_mgr_set_mode(power_mode_t mode)
{
    s_mode = mode;
    return nvs_store_set_u16(NVS_NS_DEVICE, NVS_KEY_MODE, (uint16_t)mode);
}

esp_err_t power_mgr_set_sleep_interval_s(uint16_t seconds)
{
    if (seconds == 0) return ESP_ERR_INVALID_ARG;
    s_interval_s = seconds;
    return nvs_store_set_u16(NVS_NS_DEVICE, NVS_KEY_INTERVAL, seconds);
}

size_t power_mgr_backlog_count(void) { return s_backlog_count; }

void power_mgr_handle_command(const char *json, int len)
{
    if (!json || len <= 0) return;

    // Bounded local copy so we can use plain string ops on the payload.
    char buf[160];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, json, n);
    buf[n] = '\0';
    ESP_LOGI(TAG, "Command: %s", buf);

    bool reboot = false;

    // Optional interval update (applies in either mode; takes effect next sleep).
    const char *iv = strstr(buf, "\"interval\"");
    if (iv) {
        const char *colon = strchr(iv, ':');
        if (colon) {
            long v = strtol(colon + 1, NULL, 10);
            if (v >= 60 && v <= 65535) {
                power_mgr_set_sleep_interval_s((uint16_t)v);
                ESP_LOGI(TAG, "Sleep interval set to %lds", v);
            }
        }
    }

    // Optional one-shot dashboard maintenance window. Only meaningful in SLEEP
    // mode, where it's consumed on the current/next wake; ignored in ACTIVE
    // (the UI is already up). {"ui":true} -> default window, {"ui":N} -> N secs.
    const char *ui = strstr(buf, "\"ui\"");
    if (ui) {
        const char *colon = strchr(ui, ':');
        if (colon) {
            if (strstr(colon, "true")) {
                s_maint_window_s = UI_MAINT_WINDOW_S;
            } else {
                long v = strtol(colon + 1, NULL, 10);
                if (v >= 60 && v <= 65535)      s_maint_window_s = (uint32_t)v;
                else if (v == 0 || strstr(colon, "false")) s_maint_window_s = 0;
            }
            ESP_LOGI(TAG, "UI maintenance window: %lus", (unsigned long)s_maint_window_s);
        }
    }

    const char *m = strstr(buf, "\"mode\"");
    if (m) {
        power_mode_t target = s_mode;
        if (strstr(m, "sleep"))       target = POWER_MODE_SLEEP;
        else if (strstr(m, "active")) target = POWER_MODE_ACTIVE;

        if (target != s_mode) {
            power_mgr_set_mode(target);
            ESP_LOGW(TAG, "Mode -> %s; rebooting to apply",
                     target == POWER_MODE_SLEEP ? "SLEEP" : "ACTIVE");
            reboot = true;
        }
    }

    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(500));  // let MQTT flush the ack / settle
        esp_restart();
    }
}

static uint32_t now_epoch(void)
{
    time_t now;
    time(&now);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    if ((tm_utc.tm_year + 1900) >= 2025) return (uint32_t)now;
    return 0;
}

// Poll the anemometer for SLEEP_BURST_SECONDS and collapse it to one summary.
static void sample_burst(wind_summary_t *out)
{
    float sum = 0.0f, peak = 0.0f, mn = 1e9f;
    int count = 0;

    for (int i = 0; i < SLEEP_BURST_SECONDS; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        anemometer_reading_t r;
        anemometer_get(&r);
        if (!r.valid) continue;
        float w = r.wind_mph;
        sum += w;
        if (w > peak) peak = w;
        if (w < mn)   mn = w;
        count++;
    }

    out->epoch    = now_epoch();
    out->avg_mph  = count ? (sum / count) : 0.0f;
    out->peak_mph = count ? peak : 0.0f;
    out->min_mph  = count ? mn : 0.0f;
}

void power_mgr_run_sleep_cycle(void)
{
    ESP_LOGI(TAG, "SLEEP cycle: wake, sample %ds, publish, deep-sleep %us",
             SLEEP_BURST_SECONDS, s_interval_s);

    // Sample first so we capture wind even if the network never comes up.
    wind_summary_t s;
    sample_burst(&s);
    backlog_push(&s);
    ESP_LOGI(TAG, "Burst: avg=%.1f peak=%.1f min=%.1f (backlog %u)",
             s.avg_mph, s.peak_mph, s.min_mph, (unsigned)s_backlog_count);

    // Give WiFi + MQTT a bounded chance to connect, then flush the backlog.
    // A mode-change command arriving on the retained cmd topic will reboot us
    // out of this function via the MQTT event task before we deep-sleep.
    int64_t deadline = (int64_t)SLEEP_WIFI_TIMEOUT_S * 1000;
    int64_t waited = 0;
    while (waited < deadline && !mqtt_bridge_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }

    // Settle window: let the broker deliver any retained command (mode/ui) and
    // let the MQTT event task run before we decide whether to flush or sleep.
    if (mqtt_bridge_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (mqtt_bridge_is_connected() && s_backlog_count > 0) {
        mqtt_bridge_publish_summaries(s_backlog, s_backlog_count);
        // Allow the QoS1 publishes to be handed to the transport before sleep.
        vTaskDelay(pdMS_TO_TICKS(1500));
        ESP_LOGI(TAG, "Flushed %u summaries", (unsigned)s_backlog_count);
        s_backlog_count = 0;
    } else {
        ESP_LOGW(TAG, "Broker unreachable; %u summaries held in RTC backlog",
                 (unsigned)s_backlog_count);
    }

    // If a {"ui":...} command was picked up (set by the MQTT event task while
    // we were connecting), bring the dashboard up and stay awake for the
    // window, then resume sleeping. Clear the retained command so this doesn't
    // repeat every wake.
    if (s_maint_window_s > 0) {
        uint32_t window = s_maint_window_s;
        s_maint_window_s = 0;
        mqtt_bridge_clear_retained_cmd();
        ESP_LOGW(TAG, "Maintenance window: dashboard up for %lus",
                 (unsigned long)window);
        http_server_start();
        for (uint32_t i = 0; i < window; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        http_server_stop();
        ESP_LOGI(TAG, "Maintenance window over; resuming sleep");
    }

    uint64_t us = (uint64_t)s_interval_s * 1000000ULL;
    ESP_LOGI(TAG, "Entering deep sleep for %us", s_interval_s);
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}
