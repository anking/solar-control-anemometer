#ifndef ANEMOMETER_H
#define ANEMOMETER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool      valid;            // true once we have at least one full sample window
    float     hz;               // instantaneous pulse rate (last window)
    float     hz_avg;           // rolling average across N windows
    float     wind_mph;         // hz * mph_per_hz
    float     wind_kmh;         // wind_mph * 1.60934
    float     wind_mph_avg;     // hz_avg * mph_per_hz
    float     wind_kmh_avg;     // wind_mph_avg * 1.60934
    float     gust_mph;         // peak instantaneous reading since boot/reset
    float     mph_per_hz;       // active calibration constant
    uint32_t  total_pulses;     // since boot
    int64_t   last_sample_us;   // esp_timer_get_time() of last successful sample
} anemometer_reading_t;

esp_err_t anemometer_init(void);

// Snapshot of the latest reading. Safe to call from any task.
void anemometer_get(anemometer_reading_t *out);

// Calibration: pulses-per-second → wind speed (mph). Persisted in NVS.
float anemometer_get_mph_per_hz(void);
esp_err_t anemometer_set_mph_per_hz(float mph_per_hz);

// Reset gust tracker and pulse counter.
void anemometer_reset_gust(void);

#endif // ANEMOMETER_H
