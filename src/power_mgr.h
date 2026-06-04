#ifndef POWER_MGR_H
#define POWER_MGR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Two power profiles, selected at boot and switchable at runtime (which
// triggers a reboot so there is a single, simple boot path):
//
//   ACTIVE - device stays awake. Reports by exception: batches 1 Hz samples
//            and only pushes to MQTT when the wind changes meaningfully or a
//            heartbeat is due. WiFi runs in modem-sleep between sends.
//
//   SLEEP  - deep-sleep duty cycle for when the panels are towed and live
//            wind no longer matters. Wakes on an interval, samples a short
//            burst, publishes a summary (plus any backlog accumulated while
//            out of broker range), then deep-sleeps again.
typedef enum {
    POWER_MODE_ACTIVE = 0,
    POWER_MODE_SLEEP  = 1,
} power_mode_t;

// One coarse wind summary covering a single wake burst. Kept tiny because a
// backlog of these lives in RTC memory across deep-sleep cycles.
typedef struct {
    uint32_t epoch;     // unix time of the burst, or 0 if clock wasn't synced
    float    avg_mph;   // mean sustained wind over the burst
    float    peak_mph;  // highest 1-second reading in the burst
    float    min_mph;   // lowest 1-second reading in the burst
} wind_summary_t;

// Loads persisted mode + interval from NVS. Call once, early in app_main,
// before branching on the mode.
void power_mgr_init(void);

power_mode_t power_mgr_get_mode(void);
uint16_t     power_mgr_get_sleep_interval_s(void);

// Persist a new mode / interval to NVS. Neither reboots on its own - the
// caller decides when to apply (typically esp_restart()).
esp_err_t power_mgr_set_mode(power_mode_t mode);
esp_err_t power_mgr_set_sleep_interval_s(uint16_t seconds);

// Parse and apply an MQTT command payload, e.g. {"mode":"sleep"} or
// {"mode":"active"} or {"interval":3600}. Persists any change and, if the
// mode actually changed, reboots into the new profile (does not return in
// that case). Safe to call from the MQTT event task.
void power_mgr_handle_command(const char *json, int len);

// Runs the full deep-sleep duty cycle: wait for WiFi/MQTT, sample a burst,
// publish summary + backlog, then enter deep sleep. Never returns. Only valid
// when the mode is POWER_MODE_SLEEP.
void power_mgr_run_sleep_cycle(void) __attribute__((noreturn));

// Number of summaries currently buffered in RTC memory (undelivered backlog).
size_t power_mgr_backlog_count(void);

#endif // POWER_MGR_H
