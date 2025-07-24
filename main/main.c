#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <string.h>
#include "esp_http_server.h"
#include "esp_mac.h"

#define MAXIMUM_AP 20
wifi_ap_record_t scanned_aps[MAXIMUM_AP];
uint16_t scanned_ap_count = 0;
TaskHandle_t main_task_handle = NULL;
esp_err_t connect_post_handler(httpd_req_t *req);

// ==== WiFi Config ====
#define WIFI_SSID "ESP32"
#define WIFI_PASS "12345678"
#define WIFI_CHANNEL 1
#define MAX_STA_CONN 4

// declare function
void wifi_connect(const char *ssid, const char *password);
void wifi_scan_task(void *pvParameters);
esp_err_t restart_handle(httpd_req_t *req);
esp_err_t connect_post_handler(httpd_req_t *req);

static const char *TAG_SCAN = "wifi_scan";
static const char *TAG_AP = "wifi_softap_web";
static const char *TAG_INFO = "INFO";
static const char *TAG_RE = "Reconnect";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG_AP, "station " MACSTR " join, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG_AP, "station " MACSTR " leave, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
    }
}

esp_err_t home(httpd_req_t *req)
{
    wifi_config_t sta_config;
    esp_wifi_get_config(WIFI_IF_STA, &sta_config);

    char ssid[33] = {0};
    size_t ssid_len = sizeof(ssid);
    nvs_handle_t nvs_handle;

    if (nvs_open("wifi_creds", NVS_READONLY, &nvs_handle) == ESP_OK)
    {
        if (nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len) == ESP_OK)
        {
        }
        else
        {
            strcpy(ssid, "Not connected");
        }
        nvs_close(nvs_handle);
    }
    else
    {
        strcpy(ssid, "Not connected");
    }

    const char *ssid_display = ssid;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG_INFO, "MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char device_id[32];
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char html[1024];
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        int8_t rssi = ap_info.rssi;
        ESP_LOGI(TAG_INFO, "Signal strength: %d dBm", rssi);
    }
    snprintf(html, sizeof(html),
             "<!DOCTYPE html><html><head><title>Welcome to my esp32</title></head><body>"
             "<h1>Welcome to my esp32</h1>"
             "<p>Model: IoT6D</p>"
             "<p>Version: 1.01</p>"
             "<p>Wifi: %s</p>"
             "<p>MAC Address: %02X:%02X:%02X:%02X:%02X:%02X</p>"
             "<p>Device id: %02X%02X%02X%02X%02X%02X</p>"
             "<p>Signal wifi%d</p>"
             "<button onclick=\"location.href='/wifimanage'\">Wifi manage</button>"
             "<button onclick=\"location.href='/restart'\">Restart</button>"
             "</body></html>",
             ssid_display, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ap_info.rssi);

    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wifi_manage(httpd_req_t *req)
{
    esp_err_t err;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        wifi_config_t sta_config;
        esp_wifi_get_config(WIFI_IF_STA, &sta_config);

        const char *ssid_display = strlen((char *)sta_config.sta.ssid) > 0 ? (char *)sta_config.sta.ssid : "Not connected";

        char html[1024];
        snprintf(html, sizeof(html),
                 "<!DOCTYPE html><html><head><title>Welcome to my esp32</title></head><body>"
                 "<p>Wifi: %s</p>"
                 "<button onclick=\"location.href='/disconnect'\">Dis</button>"
                 "</body></html>",
                 ssid_display);

        return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    }
    else
    {

        const char *html_start =
            "<!DOCTYPE html><html><head><title>WiFi Connect</title></head><body>"
            "<h1>select your wifi</h1>"
            "<form method=\"POST\" action=\"/connect\">"
            "<label for=\"ssid\">SSID:</label>"
            "<select id=\"ssid\" name=\"ssid\">";
        err = httpd_resp_send_chunk(req, html_start, strlen(html_start));
        if (err != ESP_OK)
            return err;

        for (int i = 0; i < scanned_ap_count; i++)
        {
            char ssid[33];
            memcpy(ssid, scanned_aps[i].ssid, sizeof(scanned_aps[i].ssid));
            ssid[32] = '\0';

            char option[128];
            int option_len = snprintf(option, sizeof(option),
                                      "<option value=\"%s\">%s</option>", ssid, ssid);
            err = httpd_resp_send_chunk(req, option, option_len);
            if (err != ESP_OK)
                return err;
        }

        const char *html_end =
            "</select><br><br>"
            "<label for=\"pass\">Password:</label>"
            "<input type=\"password\" id=\"pass\" name=\"password\" /><br><br>"
            "<input type=\"submit\" value=\"Connect\" />"
            "</form></body></html>";
        err = httpd_resp_send_chunk(req, html_end, strlen(html_end));
        if (err != ESP_OK)
            return err;

        return httpd_resp_send_chunk(req, NULL, 0);
    }

    const char *html_start =
        "<!DOCTYPE html><html><head><title>WiFi Connect</title></head><body>"
        "<h1>select your wifi</h1>"
        "<form method=\"POST\" action=\"/connect\">"
        "<label for=\"ssid\">SSID:</label>"
        "<select id=\"ssid\" name=\"ssid\">";
    err = httpd_resp_send_chunk(req, html_start, strlen(html_start));
    if (err != ESP_OK)
        return err;

    for (int i = 0; i < scanned_ap_count; i++)
    {
        char ssid[33];
        memcpy(ssid, scanned_aps[i].ssid, sizeof(scanned_aps[i].ssid));
        ssid[32] = '\0';

        char option[128];
        int option_len = snprintf(option, sizeof(option),
                                  "<option value=\"%s\">%s</option>", ssid, ssid);
        err = httpd_resp_send_chunk(req, option, option_len);
        if (err != ESP_OK)
            return err;
    }

    const char *html_end =
        "</select><br><br>"
        "<label for=\"pass\">Password:</label>"
        "<input type=\"password\" id=\"pass\" name=\"password\" /><br><br>"
        "<input type=\"submit\" value=\"Connect\" />"
        "</form></body></html>";
    err = httpd_resp_send_chunk(req, html_end, strlen(html_end));
    if (err != ESP_OK)
        return err;

    return httpd_resp_send_chunk(req, NULL, 0);
}
void disconnect_wifi_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(100)); // รอให้ response ส่งเสร็จก่อน

    ESP_LOGI(TAG_AP, "Disconnect wifi task running");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_creds", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        nvs_erase_key(nvs_handle, "ssid");
        nvs_erase_key(nvs_handle, "password");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(1000));
    xTaskCreate(wifi_scan_task, "wifi_scan_task", 8192, NULL, 5, NULL);
    vTaskDelete(NULL);
}

esp_err_t disconnect_handle(httpd_req_t *req)
{
    ESP_LOGI(TAG_AP, "Disconnect");

    const char *resp = "<html><body><h1>Wi-Fi Disconnected</h1>"
                       "<a href='/'>Back to Home</a></body></html>";

    esp_err_t err = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG_AP, "httpd_resp_send failed: %s", esp_err_to_name(err));
    }

    xTaskCreate(disconnect_wifi_task, "disconnect_wifi_task", 8192, NULL, 5, NULL);

    return ESP_OK;
}
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t home_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = home,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &home_uri);

        httpd_uri_t uri_get = {
            .uri = "/wifimanage",
            .method = HTTP_GET,
            .handler = wifi_manage,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_post = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = connect_post_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &uri_post);
        httpd_uri_t disconnect_get = {
            .uri = "/disconnect",
            .method = HTTP_GET,
            .handler = disconnect_handle,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &disconnect_get);
        httpd_uri_t restart_get = {
            .uri = "/restart",
            .method = HTTP_GET,
            .handler = restart_handle,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &restart_get);
    }

    return server;
}
void wifi_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_sta_config = {0};

    strncpy((char *)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char *)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password));

    ESP_LOGI(TAG_AP, "Connecting to SSID: %s", ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_connect());
}
esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;
    char ssid[33] = {0};
    char password[65] = {0};

    int idx = 0;
    while (remaining > 0 && idx < sizeof(buf) - 1)
    {
        ret = httpd_req_recv(req, buf + idx, remaining > sizeof(buf) - idx - 1 ? sizeof(buf) - idx - 1 : remaining);
        if (ret <= 0)
        {
            return ESP_FAIL;
        }
        remaining -= ret;
        idx += ret;
    }
    buf[idx] = '\0';

    ESP_LOGI(TAG_AP, "Received POST data: %s", buf);

    char *ssid_start = strstr(buf, "ssid=");
    char *pass_start = strstr(buf, "password=");
    if (!ssid_start)
        return ESP_FAIL;

    ssid_start += strlen("ssid=");
    char *ssid_end = strchr(ssid_start, '&');
    if (ssid_end)
    {
        int len = ssid_end - ssid_start;
        if (len > 32)
            len = 32;
        strncpy(ssid, ssid_start, len);
        ssid[len] = '\0';
    }
    else
    {
        strncpy(ssid, ssid_start, 32);
        ssid[32] = '\0';
    }

    if (pass_start)
    {
        pass_start += strlen("password=");
        char *pass_end = strchr(pass_start, '&');
        int len = pass_end ? (pass_end - pass_start) : strlen(pass_start);
        if (len > 64)
            len = 64;
        strncpy(password, pass_start, len);
        password[len] = '\0';
    }

    ESP_LOGI(TAG_AP, "Parsed SSID: %s, Password: %s", ssid, password);

    wifi_connect(ssid, password);

    const char *resp = "<!DOCTYPE html><html><body><h1>connecting WiFi...</h1></body></html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_creds", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_set_str(nvs_handle, "ssid", ssid);
        nvs_set_str(nvs_handle, "password", password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    else
    {
        ESP_LOGI(TAG_AP, "Fail to save in NVS%s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static const char *auth_mode_type(wifi_auth_mode_t auth_mode)
{
    switch (auth_mode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2 PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA WPA2 PSK";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3 PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2 WPA3 PSK";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI PSK";
    default:
        return "UNKNOWN";
    }
}

void wifi_init_apsta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    if (strlen(WIFI_PASS) == 0)
    {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_AP, "SoftAP started. SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
}

void wifi_scan_task(void *pvParameters)
{
    while (1)
    {
        wifi_ap_record_t connected_info;
        esp_err_t connected = esp_wifi_sta_get_ap_info(&connected_info);
        if (connected == ESP_OK)
        {
            ESP_LOGI(TAG_SCAN, "Connected to SSID: %s. Stop scanning.", (char *)connected_info.ssid);
            vTaskDelete(NULL);
            return;
        }
        wifi_mode_t mode;
        esp_err_t err_mode = esp_wifi_get_mode(&mode);
        if (err_mode != ESP_OK || mode == WIFI_MODE_NULL)
        {
            ESP_LOGE(TAG_SCAN, "Wi-Fi not initialized. Abort scan.");
            vTaskDelete(NULL);
            return;
        }

        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true};

        ESP_LOGI(TAG_SCAN, "Starting scan...");
        esp_err_t err_scan = esp_wifi_scan_start(&scan_config, true);
        if (err_scan != ESP_OK)
        {
            ESP_LOGE(TAG_SCAN, "Scan start failed: %s", esp_err_to_name(err_scan));
            vTaskDelete(NULL);
            return;
        }

        uint16_t ap_num = MAXIMUM_AP;
        wifi_ap_record_t ap_records[MAXIMUM_AP];
        memset(ap_records, 0, sizeof(ap_records));

        esp_err_t err_get = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
        if (err_get != ESP_OK)
        {
            ESP_LOGE(TAG_SCAN, "Failed to get AP records: %s", esp_err_to_name(err_get));
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG_SCAN, "Found %d APs", ap_num);
        vTaskDelay(pdMS_TO_TICKS(10000)); // delay 10 seconds

        for (int i = 0; i < ap_num; i++)
        {
            char ssid[33] = {0};
            memcpy(ssid, ap_records[i].ssid, sizeof(ap_records[i].ssid));
            ssid[32] = '\0';
            if (strlen(ssid) == 0)
            {
                ESP_LOGI(TAG_SCAN, "AP %d: SSID: <hidden>, Channel: %d, RSSI: %d, Authmode: %s",
                         i, ap_records[i].primary, ap_records[i].rssi,
                         auth_mode_type(ap_records[i].authmode));
            }
            else
            {
                ESP_LOGI(TAG_SCAN, "AP %d: SSID: %s, Channel: %d, RSSI: %d, Authmode: %s",
                         i, ssid, ap_records[i].primary, ap_records[i].rssi,
                         auth_mode_type(ap_records[i].authmode));
            }
        }
        scanned_ap_count = ap_num;
        memcpy(scanned_aps, ap_records, sizeof(wifi_ap_record_t) * ap_num);
        if (main_task_handle != NULL)
        {
            xTaskNotifyGive(main_task_handle);
        }
    }

    vTaskDelete(NULL);
}
esp_err_t restart_handle(httpd_req_t *req)
{
    const char *resp = "<html><body><h1>Restarting...</h1></body></html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_restart();

    return ESP_OK;
}
void wifi_reconnect_task(void *pvParameter)
{
    while (1)
    {
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

        if (ret != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(60000));
            ESP_LOGW(TAG_RE, "WiFi disconnected, attempting to reconnect...");

            char ssid[33] = {0};
            char password[65] = {0};
            size_t ssid_len = sizeof(ssid);
            size_t pass_len = sizeof(password);

            nvs_handle_t nvs_handle;
            if (nvs_open("wifi_creds", NVS_READONLY, &nvs_handle) == ESP_OK)
            {
                if (nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len) == ESP_OK &&
                    nvs_get_str(nvs_handle, "password", password, &pass_len) == ESP_OK)
                {
                    wifi_connect(ssid, password);
                }
                nvs_close(nvs_handle);
            }
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_apsta();

    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);

    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        if (nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(nvs_handle, "password", password, &pass_len) == ESP_OK)
        {
            ESP_LOGI(TAG_AP, "Connecting to saved wifi: SSID=%s", ssid);
            wifi_connect(ssid, password);
        }
        else
        {
            ESP_LOGI(TAG_AP, "No wifi credentials in NVS");
        }
        nvs_close(nvs_handle);
    }
    else
    {
        ESP_LOGI(TAG_AP, "Cannot open NVS");
    }

    start_webserver();

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 8192, NULL, 5, NULL);
    xTaskCreate(wifi_reconnect_task, "wifi_reconnect_task", 8192, NULL, 5, NULL);
}