#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include "esp_err.h"
#include "anemometer.h"
#include "power_mgr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t mqtt_bridge_init(void);
bool mqtt_bridge_is_connected(void);

void mqtt_bridge_publish_reading(const anemometer_reading_t *reading);

// Publish a batch of 1 Hz readings as a JSON array to anemometers/{mac}/wind.
// Used by ACTIVE mode's report-by-exception flush.
void mqtt_bridge_publish_batch(const anemometer_reading_t *arr, size_t n);

// Publish a batch of coarse wind summaries (QoS 1) to anemometers/{mac}/summary.
// Used by SLEEP mode to flush the RTC backlog once the broker is reachable.
void mqtt_bridge_publish_summaries(const wind_summary_t *arr, size_t n);

// Clear the retained command on anemometers/{mac}/cmd (publishes an empty
// retained payload). Used after consuming a one-shot command so it doesn't
// re-trigger on the next wake.
void mqtt_bridge_clear_retained_cmd(void);

// Publishes anemometers/{mac}/info (model, firmware, sensor GPIO, calibration).
// Auto-called on MQTT_EVENT_CONNECTED; expose publicly so the API can re-send
// after a calibration change.
void mqtt_bridge_publish_info(void);

esp_err_t mqtt_bridge_configure(const char *host, int port, const char *user, const char *pass);
esp_err_t mqtt_bridge_disconnect(void);
esp_err_t mqtt_bridge_clear_config(void);

typedef struct {
    bool     configured;
    bool     connected;
    char     host[64];
    int      port;
    char     mac[18];
    char     error[64];
    uint32_t publish_count;
    uint32_t publish_fail_count;
} mqtt_status_t;

void mqtt_bridge_get_status(mqtt_status_t *out);

#endif // MQTT_BRIDGE_H
