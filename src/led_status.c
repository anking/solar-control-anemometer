#include "led_status.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led_status";

static led_wifi_status_t g_wifi_status = LED_STATUS_WIFI_OFF;
static TaskHandle_t g_led_task_handle = NULL;

static inline void led_drive(bool on) {
#if LED_ACTIVE_LOW
    gpio_set_level(LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(LED_GPIO, on ? 1 : 0);
#endif
}

static void led_task(void *pvParameters) {
    while (1) {
        switch (g_wifi_status) {
            case LED_STATUS_WIFI_CONNECTED:
                led_drive(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_STATUS_WIFI_CONNECTING:
                led_drive(true);
                vTaskDelay(pdMS_TO_TICKS(1000));
                led_drive(false);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_STATUS_WIFI_RETRYING:
                led_drive(true);
                vTaskDelay(pdMS_TO_TICKS(200));
                led_drive(false);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;
            case LED_STATUS_WIFI_DISCONNECTED:
            case LED_STATUS_WIFI_OFF:
            default:
                led_drive(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

esp_err_t led_status_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO");
        return ret;
    }

    led_drive(false);

    xTaskCreate(led_task, "led_status", 2048, NULL, 5, &g_led_task_handle);

    ESP_LOGI(TAG, "LED status initialized (GPIO%d, active %s)",
             LED_GPIO, LED_ACTIVE_LOW ? "LOW" : "HIGH");
    return ESP_OK;
}

void led_status_set_wifi(led_wifi_status_t status) {
    g_wifi_status = status;
}

void led_status_set(bool on) {
    led_drive(on);
}
