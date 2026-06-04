#ifndef CONFIG_H
#define CONFIG_H

#include "hal/adc_types.h"

// =============================================================================
// HARDWARE: Teyleten Robot ESP32-C3 SuperMini
// =============================================================================
// GPIO 8 has the onboard LED, active-LOW on most SuperMini boards.

// Onboard LED
#define LED_GPIO              8
#define LED_ACTIVE_LOW        1

// Anemometer: passive DC generator (no excitation needed). Signal is connected
// to GPIO 4 with a 1µF cap to GND for ripple smoothing. GPIO 4 is ADC1_CH4.
// We use ADC1 only — ADC2 conflicts with WiFi on the C3.
#define ANEMOMETER_ADC_UNIT    ADC_UNIT_1
#define ANEMOMETER_ADC_CHANNEL ADC_CHANNEL_4
#define ANEMOMETER_GPIO        4

// =============================================================================
// ANEMOMETER CALIBRATION
// =============================================================================
// Linear model: wind_mph = max(0, (voltage_v - zero_offset_v) * mph_per_volt)
//
// Default slope of 36 mph/V is the empirical starting point for this build;
// field-calibrate from the dashboard against a hand anemometer for real
// accuracy. The sensor's nominal spec is closer to 18.83, but real units
// drift heavily — measure, don't assume.
#define ANEMOMETER_DEFAULT_MPH_PER_VOLT   36.0f
#define ANEMOMETER_DEFAULT_ZERO_OFFSET_MV 0

// Saturation threshold — readings above this can't be trusted (ADC pegged).
// With ADC_ATTEN_DB_12 the C3 ADC saturates near 3.1 V.
#define ANEMOMETER_SATURATION_MV          3000

// Sampling: oversample the ADC at this rate, average to produce a 1 Hz reading.
#define ANEMOMETER_OVERSAMPLE_HZ          100
#define ANEMOMETER_SAMPLE_INTERVAL_MS     1000

// ---- WMO-style wind reporting ----------------------------------------------
// One 1-second sample is appended to a ring buffer every second. From that
// buffer we derive:
//   - Sustained mean: arithmetic mean of the last 120 s   (ASOS-style 2-min)
//   - Gust:           peak 3-second running mean in the   (WMO 8 standard)
//                     last 600 s
// 600 floats = 2.4 KB; cost is trivial on the C3.
#define ANEMOMETER_HISTORY_SECONDS        600   // 10-min rolling window
#define ANEMOMETER_SUSTAINED_SECONDS      120   //  2-min sustained mean
#define ANEMOMETER_GUST_AVG_SECONDS       3     //  3-sec gust window

// =============================================================================
// POWER / REPORTING (battery operation)
// =============================================================================
// The device runs on battery. Instead of pushing a reading every second, we
// report by exception: only send when something meaningful changes, plus a
// periodic heartbeat so the backend knows we're still alive. Between sends the
// radio can idle in modem-sleep, which is where most of the battery savings
// come from.

// ACTIVE mode — adaptive batched reporting.
//   Heartbeat: force a flush at least this often even if nothing changed.
#define REPORT_HEARTBEAT_SECONDS   15
//   Deadband: sustained-wind change (mph) that counts as "something changed".
#define REPORT_MPH_DEADBAND        2.0f
//   Gust trigger: an instantaneous jump (mph) above the last sent value that
//   forces an immediate flush — we never want to sit on a gust.
#define REPORT_GUST_DELTA_MPH      3.0f
//   Cap on how many 1 Hz samples we batch before flushing regardless.
#define REPORT_BATCH_MAX           20

// SLEEP mode — deep-sleep duty cycle for when the panels are towed and live
// wind data no longer matters. We wake on an interval, take a short burst of
// samples, publish a summary (plus any backlog accumulated while offline),
// then deep-sleep again.
//   Default wake interval (seconds). Overridable via MQTT / NVS.
#define SLEEP_DEFAULT_INTERVAL_S   3600
//   How long to stay awake sampling each wake (seconds).
#define SLEEP_BURST_SECONDS        30
//   Max time to wait for WiFi+MQTT before giving up and re-sleeping (seconds).
#define SLEEP_WIFI_TIMEOUT_S       25
//   Backlog of hourly summaries kept in RTC memory if we can't reach the
//   broker (e.g. out of range). 192 ≈ 8 days at 1/hour.
#define SLEEP_BACKLOG_MAX          192
//   Maintenance window: when a {"ui":true} command is picked up on wake, the
//   device brings the dashboard up and stays awake this long (seconds) before
//   resuming the sleep cycle — enough to do an OTA update or inspect.
#define UI_MAINT_WINDOW_S          1800

// =============================================================================
// FIRMWARE VERSION (injected by version.py at build time)
// =============================================================================

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#endif // CONFIG_H
