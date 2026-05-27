#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "led_status.h"
#include <time.h>

static const char *TAG = "wifi_manager";

static wifi_status_t g_wifi_status = {0};
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int MAX_RETRY = 5;
static int s_retry_num = 0;
static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;
static bool s_sntp_started = false;
static bool s_mdns_started = false;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static volatile bool s_ap_active       = false;
static volatile int  s_ap_client_count = 0;
static TaskHandle_t  s_ap_task_handle  = NULL;

#define AP_INITIAL_KEEP_MS       (5 * 60 * 1000)
#define AP_POST_CONNECT_KEEP_MS  (5 * 60 * 1000)
#define AP_CHECK_INTERVAL_MS     5000
#define AP_MAX_CONNECTIONS       4
#define AP_CHANNEL               1

#define MDNS_HOSTNAME_AP   "anemometer"

static char s_hostname_sta[32] = {0};

static void update_mdns_hostname(bool sta_connected)
{
    const char *name = sta_connected ? s_hostname_sta : MDNS_HOSTNAME_AP;

    if (!s_mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
            return;
        }
        s_mdns_started = true;
        (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    (void)mdns_hostname_set(name);
    (void)mdns_instance_name_set(name);

    strncpy(g_wifi_status.hostname, name, sizeof(g_wifi_status.hostname) - 1);
    g_wifi_status.hostname[sizeof(g_wifi_status.hostname) - 1] = '\0';

    ESP_LOGI(TAG, "mDNS hostname: http://%s.local/", name);
}

static void start_sntp_if_needed(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

static void wifi_manager_stop_ap(void)
{
    if (!s_ap_active) return;

    ESP_LOGI(TAG, "Stopping soft AP...");
    s_ap_active = false;
    g_wifi_status.ap_active  = false;
    g_wifi_status.ap_clients = 0;
    led_status_set_ap_active(false);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to STA mode: %s", esp_err_to_name(err));
    }

    s_ap_client_count = 0;
    ESP_LOGI(TAG, "Soft AP stopped");
}

static void ap_management_task(void *arg)
{
    int64_t boot_ms       = esp_timer_get_time() / 1000;
    int64_t ap_keep_until = boot_ms + AP_INITIAL_KEEP_MS;
    bool    sta_prev      = g_wifi_status.connected;

    ESP_LOGI(TAG, "AP management: hotspot will stay active for at least 5 minutes");

    while (s_ap_active) {
        vTaskDelay(pdMS_TO_TICKS(AP_CHECK_INTERVAL_MS));
        if (!s_ap_active) break;

        int64_t now_ms  = esp_timer_get_time() / 1000;
        bool sta_now    = g_wifi_status.connected;
        int  clients    = s_ap_client_count;

        if (sta_now && !sta_prev) {
            int64_t ext = now_ms + AP_POST_CONNECT_KEEP_MS;
            if (ext > ap_keep_until) {
                ap_keep_until = ext;
                ESP_LOGI(TAG, "AP management: STA connected, AP extended 5 more minutes");
            }
        }
        sta_prev = sta_now;

        if (now_ms < ap_keep_until) continue;

        if (!sta_now) {
            ap_keep_until = now_ms + AP_POST_CONNECT_KEEP_MS;
            ESP_LOGI(TAG, "AP management: STA not connected, extending AP 5 more minutes");
            continue;
        }

        if (clients > 0) continue;

        ESP_LOGI(TAG, "AP management: STA connected, no AP clients — shutting down AP");
        wifi_manager_stop_ap();
        break;
    }

    s_ap_task_handle = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {

        case WIFI_EVENT_STA_START:
            led_status_set_wifi(LED_STATUS_WIFI_CONNECTING);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;
            g_wifi_status.last_disconnect_reason = disc->reason;
            g_wifi_status.connected = false;

            ESP_LOGW(TAG, "Wi-Fi disconnected, reason: %d", disc->reason);

            if (s_retry_num < MAX_RETRY) {
                led_status_set_wifi(LED_STATUS_WIFI_RETRYING);
                esp_wifi_connect();
                s_retry_num++;
                g_wifi_status.reconnect_attempts++;
                ESP_LOGI(TAG, "Retry to connect to AP (attempt %d/%d)",
                         s_retry_num, MAX_RETRY);
            } else {
                led_status_set_wifi(LED_STATUS_WIFI_RETRYING);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGE(TAG, "Connection failed after %d attempts", MAX_RETRY);
                vTaskDelay(pdMS_TO_TICKS(10000));
                s_retry_num = 0;
                esp_wifi_connect();
            }
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

            update_mdns_hostname(false);
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev =
                (wifi_event_ap_staconnected_t *)event_data;
            s_ap_client_count++;
            g_wifi_status.ap_clients = (uint8_t)s_ap_client_count;
            ESP_LOGI(TAG, "AP: station joined (MAC: " MACSTR ", AID: %d, total: %d)",
                     MAC2STR(ev->mac), ev->aid, s_ap_client_count);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev =
                (wifi_event_ap_stadisconnected_t *)event_data;
            if (s_ap_client_count > 0) s_ap_client_count--;
            g_wifi_status.ap_clients = (uint8_t)s_ap_client_count;
            ESP_LOGI(TAG, "AP: station left  (MAC: " MACSTR ", AID: %d, total: %d)",
                     MAC2STR(ev->mac), ev->aid, s_ap_client_count);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));

        snprintf(g_wifi_status.ip, sizeof(g_wifi_status.ip),
                 IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(g_wifi_status.gateway, sizeof(g_wifi_status.gateway),
                 IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(g_wifi_status.netmask, sizeof(g_wifi_status.netmask),
                 IPSTR, IP2STR(&event->ip_info.netmask));

        s_retry_num = 0;
        g_wifi_status.connected = true;
        led_status_set_wifi(LED_STATUS_WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        start_sntp_if_needed();
        update_mdns_hostname(true);
    }
}

esp_err_t wifi_manager_init(void)
{
    memset(&g_wifi_status, 0, sizeof(wifi_status_t));

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err == ESP_OK) {
        snprintf(s_hostname_sta, sizeof(s_hostname_sta),
                 "anemometer-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        strncpy(s_hostname_sta, "anemometer", sizeof(s_hostname_sta) - 1);
    }

    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, s_hostname_sta));

    strncpy(g_wifi_status.hostname, MDNS_HOSTNAME_AP,
            sizeof(g_wifi_status.hostname) - 1);

    if (mac_err == ESP_OK) {
        snprintf(g_wifi_status.ap_ssid, sizeof(g_wifi_status.ap_ssid),
                 "Anemometer-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        strncpy(g_wifi_status.ap_ssid, "Anemometer",
                sizeof(g_wifi_status.ap_ssid) - 1);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &s_instance_got_ip));

    return ESP_OK;
}

esp_err_t wifi_manager_start(const char *ssid, const char *password)
{
    bool has_sta = (ssid != NULL && ssid[0] != '\0');

    if (has_sta) {
        strncpy(g_wifi_status.ssid, ssid, sizeof(g_wifi_status.ssid) - 1);
        g_wifi_status.ssid[sizeof(g_wifi_status.ssid) - 1] = '\0';
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONNECTIONS,
            .authmode       = WIFI_AUTH_OPEN,
            .pmf_cfg        = { .required = false },
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, g_wifi_status.ap_ssid,
            sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(g_wifi_status.ap_ssid);

    if (has_sta) {
        wifi_config_t sta_cfg = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        strncpy((char *)sta_cfg.sta.ssid, ssid,
                sizeof(sta_cfg.sta.ssid) - 1);
        if (password) {
            strncpy((char *)sta_cfg.sta.password, password,
                    sizeof(sta_cfg.sta.password) - 1);
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    } else {
        wifi_config_t empty_sta = {0};
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &empty_sta));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_active              = true;
    g_wifi_status.ap_active  = true;
    g_wifi_status.ap_clients = 0;
    led_status_set_ap_active(true);

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(g_wifi_status.ap_ip, sizeof(g_wifi_status.ap_ip),
                 IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(g_wifi_status.ap_ip, "192.168.4.1",
                sizeof(g_wifi_status.ap_ip) - 1);
    }

    update_mdns_hostname(false);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Hotspot active for at least 5 minutes");
    ESP_LOGI(TAG, "  SSID:     %s", g_wifi_status.ap_ssid);
    ESP_LOGI(TAG, "  Password: (none - open network)");
    ESP_LOGI(TAG, "  Web UI:   http://%s/ or http://%s.local/",
             g_wifi_status.ap_ip, MDNS_HOSTNAME_AP);
    ESP_LOGI(TAG, "========================================");

    xTaskCreate(ap_management_task, "ap_mgmt", 3072, NULL, 5, &s_ap_task_handle);

    esp_wifi_get_mac(WIFI_IF_STA, g_wifi_status.mac);

    if (!has_sta) {
        ESP_LOGI(TAG, "AP-only mode - no STA credentials configured");
        led_status_set_wifi(LED_STATUS_WIFI_DISCONNECTED);
        return ESP_OK;
    }

    led_status_set_wifi(LED_STATUS_WIFI_CONNECTING);
    ESP_LOGI(TAG, "Wi-Fi APSTA mode started. Connecting to %s...", ssid);

    const TickType_t timeout_ticks = pdMS_TO_TICKS(30000);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           timeout_ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            g_wifi_status.rssi    = ap_info.rssi;
            g_wifi_status.channel = ap_info.primary;
        }
        strncpy(g_wifi_status.dns, g_wifi_status.gateway,
                sizeof(g_wifi_status.dns) - 1);

        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s (AP still active for fallback)",
                 ssid);
        led_status_set_wifi(LED_STATUS_WIFI_DISCONNECTED);
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection timeout (still trying in background, AP active)");
        led_status_set_wifi(LED_STATUS_WIFI_RETRYING);
        return ESP_OK;
    }
}

void wifi_manager_get_status(wifi_status_t *status)
{
    if (status == NULL) return;

    if (g_wifi_status.connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            g_wifi_status.rssi    = ap_info.rssi;
            g_wifi_status.channel = ap_info.primary;
        }
    }

    g_wifi_status.ap_active  = s_ap_active;
    g_wifi_status.ap_clients = (uint8_t)s_ap_client_count;

    memcpy(status, &g_wifi_status, sizeof(wifi_status_t));
}

bool wifi_manager_is_connected(void)
{
    return g_wifi_status.connected;
}

bool wifi_manager_ap_is_active(void)
{
    return s_ap_active;
}

int wifi_manager_scan(wifi_scan_result_t *results, int max_results)
{
    if (results == NULL || max_results <= 0) return 0;

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    uint16_t fetch = (ap_count < (uint16_t)max_results) ? ap_count : (uint16_t)max_results;
    wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
    if (records == NULL) return 0;

    esp_wifi_scan_get_ap_records(&fetch, records);

    int written = 0;
    for (uint16_t i = 0; i < fetch; i++) {
        bool dup = false;
        for (int j = 0; j < written; j++) {
            if (strcmp(results[j].ssid, (const char *)records[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        if (records[i].ssid[0] == '\0') continue;

        strncpy(results[written].ssid, (const char *)records[i].ssid, 32);
        results[written].ssid[32] = '\0';
        results[written].rssi     = records[i].rssi;
        results[written].channel  = records[i].primary;
        results[written].authmode = (uint8_t)records[i].authmode;
        written++;
        if (written >= max_results) break;
    }

    free(records);
    ESP_LOGI(TAG, "WiFi scan: %d unique networks found", written);
    return written;
}

esp_err_t wifi_manager_set_sta_config(const char *ssid, const char *password)
{
    if (ssid == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Applying new STA config: SSID=\"%s\"", ssid);

    strncpy(g_wifi_status.ssid, ssid, sizeof(g_wifi_status.ssid) - 1);
    g_wifi_status.ssid[sizeof(g_wifi_status.ssid) - 1] = '\0';

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    }

    if (!password || password[0] == '\0') {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(err));
        return err;
    }

    s_retry_num = 0;
    g_wifi_status.connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_wifi_connect();
    return ESP_OK;
}

void wifi_manager_disconnect_sta(void)
{
    ESP_LOGI(TAG, "Disconnecting STA and clearing config");
    esp_wifi_disconnect();

    wifi_config_t empty = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty);

    g_wifi_status.connected = false;
    g_wifi_status.ssid[0] = '\0';
    g_wifi_status.ip[0] = '\0';
    g_wifi_status.gateway[0] = '\0';
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    update_mdns_hostname(false);
}
