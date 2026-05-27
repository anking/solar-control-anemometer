#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include "esp_err.h"
#include "anemometer.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t mqtt_bridge_init(void);
bool mqtt_bridge_is_connected(void);

void mqtt_bridge_publish_reading(const anemometer_reading_t *reading);

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
