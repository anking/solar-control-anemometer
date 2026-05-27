#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

// Single onboard LED (GPIO 8 on the C3 SuperMini, active LOW). The same LED
// indicates WiFi state — anemometer state is implicit (the sensor either
// produces pulses or it doesn't, and the dashboard shows that directly).
typedef enum {
    LED_STATUS_WIFI_CONNECTED,      // Solid ON
    LED_STATUS_WIFI_CONNECTING,     // Slow blink (1s)
    LED_STATUS_WIFI_RETRYING,       // Fast blink (200ms)
    LED_STATUS_WIFI_DISCONNECTED,   // OFF
    LED_STATUS_WIFI_OFF             // OFF
} led_wifi_status_t;

esp_err_t led_status_init(void);
void led_status_set_wifi(led_wifi_status_t status);
void led_status_set(bool on);

#endif // LED_STATUS_H
