#include "anemometer.h"
#include "config.h"
#include "nvs_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "anemometer";

static volatile uint32_t s_pulse_count = 0;
static volatile int64_t  s_last_edge_us = 0;
static volatile uint32_t s_total_pulses = 0;
static portMUX_TYPE      s_isr_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_mutex = NULL;
static anemometer_reading_t s_reading = {0};
static float s_mph_per_hz = ANEMOMETER_DEFAULT_MPH_PER_HZ;
static float s_window_hz[ANEMOMETER_AVG_SAMPLES] = {0};
static int   s_window_idx = 0;
static int   s_window_filled = 0;

static void IRAM_ATTR pulse_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_edge_us) < ANEMOMETER_DEBOUNCE_US) return;
    s_last_edge_us = now;
    s_pulse_count++;
    s_total_pulses++;
}

static void sampler_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(ANEMOMETER_SAMPLE_INTERVAL_MS);
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, period);

        // Snapshot + reset counter atomically wrt ISR.
        portENTER_CRITICAL(&s_isr_mux);
        uint32_t pulses = s_pulse_count;
        s_pulse_count = 0;
        portEXIT_CRITICAL(&s_isr_mux);

        float seconds = ANEMOMETER_SAMPLE_INTERVAL_MS / 1000.0f;
        float hz = (float)pulses / seconds;

        // Update rolling window.
        s_window_hz[s_window_idx] = hz;
        s_window_idx = (s_window_idx + 1) % ANEMOMETER_AVG_SAMPLES;
        if (s_window_filled < ANEMOMETER_AVG_SAMPLES) s_window_filled++;

        float sum = 0.0f;
        for (int i = 0; i < s_window_filled; i++) sum += s_window_hz[i];
        float hz_avg = sum / (float)s_window_filled;

        float mph     = hz     * s_mph_per_hz;
        float mph_avg = hz_avg * s_mph_per_hz;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_reading.valid          = true;
        s_reading.hz             = hz;
        s_reading.hz_avg         = hz_avg;
        s_reading.wind_mph       = mph;
        s_reading.wind_kmh       = mph * 1.60934f;
        s_reading.wind_mph_avg   = mph_avg;
        s_reading.wind_kmh_avg   = mph_avg * 1.60934f;
        if (mph > s_reading.gust_mph) s_reading.gust_mph = mph;
        s_reading.mph_per_hz     = s_mph_per_hz;
        s_reading.total_pulses   = s_total_pulses;
        s_reading.last_sample_us = esp_timer_get_time();
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t anemometer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Load saved calibration.
    uint16_t stored;
    if (nvs_store_get_u16(NVS_NS_ANEMOMETER, "mph_per_hz_x1k", &stored) == ESP_OK) {
        s_mph_per_hz = (float)stored / 1000.0f;
        ESP_LOGI(TAG, "Loaded calibration: %.3f mph per Hz", s_mph_per_hz);
    } else {
        ESP_LOGI(TAG, "Using default calibration: %.3f mph per Hz", s_mph_per_hz);
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ANEMOMETER_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(ANEMOMETER_GPIO, pulse_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISR add failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(sampler_task, "anemo_smp", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Anemometer ready on GPIO%d (pull-up, falling-edge, debounce %dus)",
             ANEMOMETER_GPIO, ANEMOMETER_DEBOUNCE_US);
    return ESP_OK;
}

void anemometer_get(anemometer_reading_t *out)
{
    if (!out || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_reading, sizeof(anemometer_reading_t));
    xSemaphoreGive(s_mutex);
}

float anemometer_get_mph_per_hz(void)
{
    return s_mph_per_hz;
}

esp_err_t anemometer_set_mph_per_hz(float mph_per_hz)
{
    if (mph_per_hz <= 0.0f || mph_per_hz > 50.0f) return ESP_ERR_INVALID_ARG;
    s_mph_per_hz = mph_per_hz;
    uint16_t stored = (uint16_t)(mph_per_hz * 1000.0f + 0.5f);
    esp_err_t err = nvs_store_set_u16(NVS_NS_ANEMOMETER, "mph_per_hz_x1k", stored);
    ESP_LOGI(TAG, "Calibration set: %.3f mph per Hz (%s)",
             mph_per_hz, err == ESP_OK ? "saved" : "save failed");
    return err;
}

void anemometer_reset_gust(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_reading.gust_mph = 0.0f;
    xSemaphoreGive(s_mutex);
}
