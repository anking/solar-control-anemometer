#ifndef ANEMOMETER_H
#define ANEMOMETER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool      valid;            // true once we have at least one full sample window
    float     voltage_v;        // 1-second average voltage at ADC pin
    int       raw_mv;           // most recent single ADC sample (mV), for diagnostics
    int       peak_mv;          // peak instantaneous mV in the last window
    bool      saturated;        // peak_mv >= ANEMOMETER_SATURATION_MV
    float     wind_mph;         // (voltage_v - zero) * mph_per_volt, clamped ≥ 0
    float     wind_kmh;
    float     wind_mph_avg;     // rolling avg over N seconds
    float     wind_kmh_avg;
    float     gust_mph;         // peak instantaneous reading since reset
    float     mph_per_volt;     // active slope
    int       zero_offset_mv;   // active zero-offset
    uint32_t  sample_count;     // total ADC samples taken since boot
    int64_t   last_sample_us;
} anemometer_reading_t;

esp_err_t anemometer_init(void);
void anemometer_get(anemometer_reading_t *out);

// Calibration — values are persisted in NVS.
float anemometer_get_mph_per_volt(void);
esp_err_t anemometer_set_mph_per_volt(float mph_per_volt);

int anemometer_get_zero_offset_mv(void);
esp_err_t anemometer_set_zero_offset_mv(int offset_mv);

// Snapshot the current 1-second average voltage and store it as the new
// zero offset. Run this when you're confident the air is still.
esp_err_t anemometer_capture_zero(void);

void anemometer_reset_gust(void);

#endif // ANEMOMETER_H
