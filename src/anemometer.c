#include "anemometer.h"
#include "config.h"
#include "nvs_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "anemometer";

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t         s_cali = NULL;
static bool                      s_cali_ok = false;

static SemaphoreHandle_t s_mutex = NULL;
static anemometer_reading_t s_reading = {0};

static float s_mph_per_volt   = ANEMOMETER_DEFAULT_MPH_PER_VOLT;
static int   s_zero_offset_mv = ANEMOMETER_DEFAULT_ZERO_OFFSET_MV;

static float s_window_mph[ANEMOMETER_AVG_SAMPLES] = {0};
static int   s_window_idx = 0;
static int   s_window_filled = 0;

static uint32_t s_sample_count = 0;

static inline int raw_to_mv(int raw)
{
    int mv = 0;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    } else {
        // Fallback: linear approximation if calibration scheme isn't available.
        // 12-bit ADC, DB_12 attenuation, ~3100 mV full-scale.
        mv = (raw * 3100) / 4095;
    }
    return mv;
}

static void sampler_task(void *arg)
{
    const TickType_t sample_period  = pdMS_TO_TICKS(1000 / ANEMOMETER_OVERSAMPLE_HZ);
    const int        samples_per_s  = ANEMOMETER_OVERSAMPLE_HZ;
    int    in_window = 0;
    int    sum_mv = 0;
    int    peak_mv = 0;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, sample_period);

        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc, ANEMOMETER_ADC_CHANNEL, &raw);
        if (err != ESP_OK) continue;
        int mv = raw_to_mv(raw);
        s_sample_count++;

        sum_mv += mv;
        if (mv > peak_mv) peak_mv = mv;
        in_window++;

        if (in_window >= samples_per_s) {
            int avg_mv = sum_mv / in_window;
            int captured_peak = peak_mv;
            in_window = 0;
            sum_mv = 0;
            peak_mv = 0;

            float voltage = avg_mv / 1000.0f;
            float zero_v  = s_zero_offset_mv / 1000.0f;
            float mph     = (voltage - zero_v) * s_mph_per_volt;
            if (mph < 0.0f) mph = 0.0f;

            // Update rolling window.
            s_window_mph[s_window_idx] = mph;
            s_window_idx = (s_window_idx + 1) % ANEMOMETER_AVG_SAMPLES;
            if (s_window_filled < ANEMOMETER_AVG_SAMPLES) s_window_filled++;
            float sum = 0.0f;
            for (int i = 0; i < s_window_filled; i++) sum += s_window_mph[i];
            float mph_avg = sum / (float)s_window_filled;

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_reading.valid          = true;
            s_reading.voltage_v      = voltage;
            s_reading.raw_mv         = avg_mv;
            s_reading.peak_mv        = captured_peak;
            s_reading.saturated      = captured_peak >= ANEMOMETER_SATURATION_MV;
            s_reading.wind_mph       = mph;
            s_reading.wind_kmh       = mph * 1.60934f;
            s_reading.wind_mph_avg   = mph_avg;
            s_reading.wind_kmh_avg   = mph_avg * 1.60934f;
            if (mph > s_reading.gust_mph) s_reading.gust_mph = mph;
            s_reading.mph_per_volt   = s_mph_per_volt;
            s_reading.zero_offset_mv = s_zero_offset_mv;
            s_reading.sample_count   = s_sample_count;
            s_reading.last_sample_us = esp_timer_get_time();
            xSemaphoreGive(s_mutex);
        }
    }
}

esp_err_t anemometer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Load saved calibration.
    uint16_t stored;
    if (nvs_store_get_u16(NVS_NS_ANEMOMETER, "mph_per_v_x100", &stored) == ESP_OK) {
        s_mph_per_volt = (float)stored / 100.0f;
        ESP_LOGI(TAG, "Loaded slope: %.2f mph/V", s_mph_per_volt);
    } else {
        ESP_LOGI(TAG, "Using default slope: %.2f mph/V", s_mph_per_volt);
    }
    if (nvs_store_get_u16(NVS_NS_ANEMOMETER, "zero_off_mv", &stored) == ESP_OK) {
        s_zero_offset_mv = (int)stored;
        ESP_LOGI(TAG, "Loaded zero offset: %d mV", s_zero_offset_mv);
    }

    // ADC1 oneshot init.
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ANEMOMETER_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, ANEMOMETER_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // Curve-fitting calibration (supported on ESP32-C3).
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ANEMOMETER_ADC_UNIT,
        .chan     = ANEMOMETER_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
        ESP_LOGI(TAG, "ADC calibration: curve-fitting");
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, using linear approximation");
    }
#else
    ESP_LOGW(TAG, "ADC curve-fitting not compiled in, using linear approximation");
#endif

    xTaskCreate(sampler_task, "anemo_smp", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ADC sampler ready on GPIO%d (ADC1 ch%d) — %d Hz oversample → 1 Hz",
             ANEMOMETER_GPIO, (int)ANEMOMETER_ADC_CHANNEL, ANEMOMETER_OVERSAMPLE_HZ);
    return ESP_OK;
}

void anemometer_get(anemometer_reading_t *out)
{
    if (!out || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_reading, sizeof(anemometer_reading_t));
    xSemaphoreGive(s_mutex);
}

float anemometer_get_mph_per_volt(void)
{
    return s_mph_per_volt;
}

esp_err_t anemometer_set_mph_per_volt(float mph_per_volt)
{
    if (mph_per_volt <= 0.0f || mph_per_volt > 200.0f) return ESP_ERR_INVALID_ARG;
    s_mph_per_volt = mph_per_volt;
    uint16_t stored = (uint16_t)(mph_per_volt * 100.0f + 0.5f);
    esp_err_t err = nvs_store_set_u16(NVS_NS_ANEMOMETER, "mph_per_v_x100", stored);
    ESP_LOGI(TAG, "Slope set: %.2f mph/V (%s)",
             mph_per_volt, err == ESP_OK ? "saved" : "save failed");
    return err;
}

int anemometer_get_zero_offset_mv(void)
{
    return s_zero_offset_mv;
}

esp_err_t anemometer_set_zero_offset_mv(int offset_mv)
{
    if (offset_mv < 0 || offset_mv > 3000) return ESP_ERR_INVALID_ARG;
    s_zero_offset_mv = offset_mv;
    esp_err_t err = nvs_store_set_u16(NVS_NS_ANEMOMETER, "zero_off_mv", (uint16_t)offset_mv);
    ESP_LOGI(TAG, "Zero offset set: %d mV (%s)",
             offset_mv, err == ESP_OK ? "saved" : "save failed");
    return err;
}

esp_err_t anemometer_capture_zero(void)
{
    if (!s_mutex) return ESP_FAIL;
    int mv;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_reading.valid) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    mv = s_reading.raw_mv;
    xSemaphoreGive(s_mutex);
    return anemometer_set_zero_offset_mv(mv);
}

void anemometer_reset_gust(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_reading.gust_mph = 0.0f;
    xSemaphoreGive(s_mutex);
}
