#include "http_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "wifi_manager.h"
#include "wifi_config.h"
#include "anemometer.h"
#include "mqtt_bridge.h"
#include "led_status.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "http_server";
static httpd_handle_t server_handle = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ---- WebSocket client tracking ----------------------------------------------
#define STATUS_WS_MAX 4
static int s_status_ws_fds[STATUS_WS_MAX];
static size_t s_status_ws_count = 0;
static SemaphoreHandle_t s_status_ws_mutex = NULL;

static void ws_mutex_init(void)
{
    if (!s_status_ws_mutex) s_status_ws_mutex = xSemaphoreCreateMutex();
}

static void ws_add_client(int fd)
{
    if (!s_status_ws_mutex) return;
    if (xSemaphoreTake(s_status_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (s_status_ws_count < STATUS_WS_MAX) {
        for (size_t i = 0; i < s_status_ws_count; i++) {
            if (s_status_ws_fds[i] == fd) { xSemaphoreGive(s_status_ws_mutex); return; }
        }
        s_status_ws_fds[s_status_ws_count++] = fd;
    }
    xSemaphoreGive(s_status_ws_mutex);
}

static void ws_remove_client(int fd)
{
    if (!s_status_ws_mutex) return;
    if (xSemaphoreTake(s_status_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (size_t i = 0; i < s_status_ws_count; i++) {
        if (s_status_ws_fds[i] == fd) {
            s_status_ws_fds[i] = s_status_ws_fds[s_status_ws_count - 1];
            s_status_ws_count--;
            break;
        }
    }
    xSemaphoreGive(s_status_ws_mutex);
}

void http_server_ws_broadcast_status(const char *json, size_t len)
{
    if (!s_status_ws_mutex || !server_handle || !json || len == 0) return;
    int local_fds[STATUS_WS_MAX];
    size_t n = 0;

    if (xSemaphoreTake(s_status_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    n = s_status_ws_count;
    for (size_t i = 0; i < n; i++) local_fds[i] = s_status_ws_fds[i];
    xSemaphoreGive(s_status_ws_mutex);

    httpd_ws_frame_t frame = {
        .payload = (uint8_t *)json,
        .len = len,
        .type = HTTPD_WS_TYPE_TEXT,
    };
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = httpd_ws_send_frame_async(server_handle, local_fds[i], &frame);
        if (err != ESP_OK) ws_remove_client(local_fds[i]);
    }
}

// ---- JSON helpers -----------------------------------------------------------

static bool json_extract_str(const char *json, const char *key, char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p = strchr(p + strlen(search), '"');
    if (!p) return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = end - p;
    if (len >= out_len) len = out_len - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return true;
}

static int json_extract_int(const char *json, const char *key, int default_val)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return default_val;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static float json_extract_float(const char *json, const char *key, float default_val)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return default_val;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return strtof(p, NULL);
}

// ---- Page + health ----------------------------------------------------------

static esp_err_t index_handler(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ---- WiFi -------------------------------------------------------------------

static const char *auth_mode_str(uint8_t mode)
{
    switch (mode) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA_PSK";
        case 3: return "WPA2_PSK";
        case 4: return "WPA_WPA2_PSK";
        case 5: return "WPA2_ENTERPRISE";
        case 6: return "WPA3_PSK";
        case 7: return "WPA2_WPA3_PSK";
        default: return "UNKNOWN";
    }
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_result_t results[20];
    int count = wifi_manager_scan(results, 20);

    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int pos = 0;
    pos += snprintf(buf + pos, 4096 - pos, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(buf + pos, 4096 - pos, ",");
        pos += snprintf(buf + pos, 4096 - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\",\"open\":%s}",
            results[i].ssid, results[i].rssi, results[i].channel,
            auth_mode_str(results[i].authmode),
            results[i].authmode == 0 ? "true" : "false");
    }
    pos += snprintf(buf + pos, 4096 - pos, "]");

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, buf);
    free(buf);
    return ret;
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    json_extract_str(buf, "ssid", ssid, sizeof(ssid));
    json_extract_str(buf, "password", password, sizeof(password));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_config_set(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    err = wifi_manager_set_sta_config(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply config");
        return ESP_FAIL;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"ssid\":\"%s\"}", ssid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t wifi_forget_handler(httpd_req_t *req)
{
    wifi_config_clear();
    wifi_manager_disconnect_sta();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"rssi\":%d,\"channel\":%d,"
        "\"ap_active\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
        "\"ap_clients\":%d,\"hostname\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
        status.connected ? "true" : "false",
        status.ssid, status.ip, status.rssi, status.channel,
        status.ap_active ? "true" : "false",
        status.ap_ssid, status.ap_ip, status.ap_clients, status.hostname,
        status.mac[0], status.mac[1], status.mac[2],
        status.mac[3], status.mac[4], status.mac[5]);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// ---- System info ------------------------------------------------------------

static esp_err_t system_info_handler(httpd_req_t *req)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"firmware_version\":\"%s\",\"git_hash\":\"%s\",\"idf_version\":\"%s\","
        "\"free_heap\":%lu,\"min_free_heap\":%lu,"
        "\"board\":\"esp32-c3-supermini\",\"sensor_gpio\":%d}",
        FIRMWARE_VERSION, GIT_HASH, esp_get_idf_version(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        ANEMOMETER_GPIO);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// ---- Anemometer status + calibration ---------------------------------------

static esp_err_t api_status_handler(httpd_req_t *req)
{
    anemometer_reading_t r;
    anemometer_get(&r);

    char buf[448];
    snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"voltage_v\":%.3f,\"raw_mv\":%d,\"peak_mv\":%d,"
        "\"saturated\":%s,"
        "\"mph\":%.2f,\"kmh\":%.2f,"
        "\"mph_avg\":%.2f,\"kmh_avg\":%.2f,"
        "\"gust_mph\":%.2f,"
        "\"mph_per_volt\":%.2f,\"zero_offset_mv\":%d,"
        "\"samples\":%lu}",
        r.valid ? "true" : "false",
        r.voltage_v, r.raw_mv, r.peak_mv,
        r.saturated ? "true" : "false",
        r.wind_mph, r.wind_kmh, r.wind_mph_avg, r.wind_kmh_avg, r.gust_mph,
        r.mph_per_volt, r.zero_offset_mv,
        (unsigned long)r.sample_count);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t api_calibrate_handler(httpd_req_t *req)
{
    char body[160] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }

    bool changed = false;

    // Optional field: mph_per_volt
    float mph_per_volt = json_extract_float(body, "mph_per_volt", -1.0f);
    if (mph_per_volt > 0.0f) {
        if (anemometer_set_mph_per_volt(mph_per_volt) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mph_per_volt");
            return ESP_FAIL;
        }
        changed = true;
    }

    // Optional field: zero_offset_mv
    int zero_mv = json_extract_int(body, "zero_offset_mv", -1);
    if (zero_mv >= 0) {
        if (anemometer_set_zero_offset_mv(zero_mv) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid zero_offset_mv");
            return ESP_FAIL;
        }
        changed = true;
    }

    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Provide mph_per_volt and/or zero_offset_mv");
        return ESP_FAIL;
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"mph_per_volt\":%.2f,\"zero_offset_mv\":%d}",
             anemometer_get_mph_per_volt(), anemometer_get_zero_offset_mv());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_capture_zero_handler(httpd_req_t *req)
{
    esp_err_t err = anemometer_capture_zero();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No reading yet");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
        return ESP_FAIL;
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"zero_offset_mv\":%d}",
             anemometer_get_zero_offset_mv());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_reset_gust_handler(httpd_req_t *req)
{
    anemometer_reset_gust();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// ---- MQTT -------------------------------------------------------------------

static esp_err_t api_mqtt_get_handler(httpd_req_t *req)
{
    mqtt_status_t st;
    mqtt_bridge_get_status(&st);

    char safe_error[64] = {0};
    for (int i = 0, j = 0; st.error[i] && j < (int)sizeof(safe_error) - 1; i++) {
        char c = st.error[i];
        if (c >= ' ' && c != '"' && c != '\\') safe_error[j++] = c;
    }

    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"configured\":%s,\"connected\":%s,\"host\":\"%s\",\"port\":%d,\"mac\":\"%s\","
        "\"error\":\"%s\",\"pub_ok\":%lu,\"pub_fail\":%lu}",
        st.configured ? "true" : "false",
        st.connected ? "true" : "false",
        st.host, st.port, st.mac, safe_error,
        (unsigned long)st.publish_count, (unsigned long)st.publish_fail_count);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t api_mqtt_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }

    char host[64] = {0}, user[32] = {0}, pass[64] = {0};
    json_extract_str(body, "host", host, sizeof(host));
    json_extract_str(body, "username", user, sizeof(user));
    json_extract_str(body, "password", pass, sizeof(pass));
    int port = json_extract_int(body, "port", 1883);

    if (host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "host required");
        return ESP_FAIL;
    }

    mqtt_bridge_configure(host, port, user, pass);

    mqtt_status_t st;
    mqtt_bridge_get_status(&st);
    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"configured\":%s,\"host\":\"%s\",\"port\":%d}",
        st.configured ? "true" : "false", st.host, st.port);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_mqtt_delete_handler(httpd_req_t *req)
{
    mqtt_bridge_clear_config();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"configured\":false,\"connected\":false}");
}

// ---- LED mode ---------------------------------------------------------------

static esp_err_t api_led_get_handler(httpd_req_t *req)
{
    led_mode_t m = led_status_get_mode();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"mode\":\"%s\"}", led_status_mode_name(m));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t api_led_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }

    char mode_str[16] = {0};
    json_extract_str(body, "mode", mode_str, sizeof(mode_str));

    led_mode_t mode;
    if      (strcmp(mode_str, "errors") == 0) mode = LED_MODE_ERRORS_ONLY;
    else if (strcmp(mode_str, "off")    == 0) mode = LED_MODE_OFF;
    else if (strcmp(mode_str, "debug")  == 0) mode = LED_MODE_DEBUG;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be errors|off|debug");
        return ESP_FAIL;
    }

    if (led_status_set_mode(mode) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"mode\":\"%s\"}", led_status_mode_name(mode));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// ---- Restart ----------------------------------------------------------------

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Restart requested via API");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Restarting...\"}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---- WebSocket --------------------------------------------------------------

static esp_err_t ws_status_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS /ws client connected (fd=%d, count=%d)", fd, (int)s_status_ws_count);
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    uint8_t buf[128];
    frame.payload = buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        ws_remove_client(fd);
        ESP_LOGI(TAG, "WS /ws client disconnected (fd=%d)", fd);
    }
    return ret;
}

// ---- Server start -----------------------------------------------------------

esp_err_t http_server_start(void)
{
    if (server_handle != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_open_sockets = 5;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",                     .method = HTTP_GET,    .handler = index_handler },
        { .uri = "/health",               .method = HTTP_GET,    .handler = health_handler },
        { .uri = "/api/wifi/scan",        .method = HTTP_GET,    .handler = wifi_scan_handler },
        { .uri = "/api/wifi/connect",     .method = HTTP_POST,   .handler = wifi_connect_handler },
        { .uri = "/api/wifi/forget",      .method = HTTP_POST,   .handler = wifi_forget_handler },
        { .uri = "/api/wifi/status",      .method = HTTP_GET,    .handler = wifi_status_handler },
        { .uri = "/api/system",           .method = HTTP_GET,    .handler = system_info_handler },
        { .uri = "/api/status",           .method = HTTP_GET,    .handler = api_status_handler },
        { .uri = "/api/calibrate",        .method = HTTP_POST,   .handler = api_calibrate_handler },
        { .uri = "/api/capture-zero",     .method = HTTP_POST,   .handler = api_capture_zero_handler },
        { .uri = "/api/reset-gust",       .method = HTTP_POST,   .handler = api_reset_gust_handler },
        { .uri = "/api/mqtt",             .method = HTTP_GET,    .handler = api_mqtt_get_handler },
        { .uri = "/api/mqtt",             .method = HTTP_POST,   .handler = api_mqtt_post_handler },
        { .uri = "/api/mqtt",             .method = HTTP_DELETE, .handler = api_mqtt_delete_handler },
        { .uri = "/api/led",              .method = HTTP_GET,    .handler = api_led_get_handler },
        { .uri = "/api/led",              .method = HTTP_POST,   .handler = api_led_post_handler },
        { .uri = "/api/restart",          .method = HTTP_POST,   .handler = api_restart_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server_handle, &routes[i]);
    }

    ws_mutex_init();
    httpd_uri_t ws_status_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_status_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server_handle, &ws_status_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}

void http_server_stop(void)
{
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

httpd_handle_t http_server_get_handle(void)
{
    return server_handle;
}
