#include "wifi_manager.h"
#include "config.h"
#include "state_machine.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "wifi_manager";

#define WIFI_MAX_RETRIES 5
#define NVS_NAMESPACE "voxie_wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"
#define NVS_KEY_URL "server_url"

static int s_retry_num = 0;
static bool s_is_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_http_server = NULL;

static void start_softap_provisioning(void);
static void start_sta_connection(const char* ssid, const char* password);

// HTML for Captive Portal
static const char* INDEX_HTML = 
"<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>Voxie Setup</title>"
"<style>"
"body{font-family:sans-serif;margin:0;padding:20px;background:#f4f4f9;}"
".container{max-width:400px;margin:auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 4px 6px rgba(0,0,0,0.1);}"
"h2{text-align:center;color:#333;}"
"label{display:block;margin-top:10px;font-weight:bold;color:#555;}"
"input,select{width:100%;padding:10px;margin-top:5px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}"
"button{width:100%;padding:12px;margin-top:20px;background:#007bff;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer;}"
"button:hover{background:#0056b3;}"
"</style>"
"</head><body>"
"<div class=\"container\">"
"<h2>Voxie Wi-Fi Setup</h2>"
"<label for=\"ssid\">Network Name (SSID)</label>"
"<select id=\"ssid\"><option value=\"\">Loading...</option></select>"
"<label for=\"password\">Password</label>"
"<input type=\"password\" id=\"password\" placeholder=\"Leave blank if open\">"
"<label for=\"server_url\">Cloud Server URL (Optional)</label>"
"<input type=\"text\" id=\"server_url\" placeholder=\"https://api.voxie.cloud\">"
"<button onclick=\"connect()\">Connect</button>"
"</div>"
"<script>"
"fetch('/scan').then(r=>r.json()).then(data=>{"
"let s=document.getElementById('ssid');s.innerHTML='';"
"data.forEach(n=>{"
"let o=document.createElement('option');o.value=n.ssid;o.text=n.ssid + ' ('+n.rssi+'dBm)';"
"s.appendChild(o);"
"});"
"}).catch(()=>{"
"document.getElementById('ssid').innerHTML='<option value=\"\">Error loading</option>';"
"});"
"function connect(){"
"let s=document.getElementById('ssid').value;"
"let p=document.getElementById('password').value;"
"let u=document.getElementById('server_url').value;"
"if(!s) {alert('Select a network');return;}"
"fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p,server_url:u})})"
".then(()=>{"
"alert('Saved! Voxie will now reboot.');"
"setTimeout(()=>location.reload(), 5000);"
"});"
"}"
"</script>"
"</body></html>";

static esp_err_t index_html_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = {};
    esp_wifi_scan_start(&scan_config, true);
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_info));
    
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char*)ap_info[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_info[i].rssi);
        cJSON_AddItemToArray(root, item);
    }
    
    const char *json_resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    
    free((void *)json_resp);
    cJSON_Delete(root);
    free(ap_info);
    
    return ESP_OK;
}

static esp_err_t connect_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    cJSON *server_url = cJSON_GetObjectItem(json, "server_url");

    if (cJSON_IsString(ssid) && (ssid->valuestring != NULL)) {
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid->valuestring);
            
            if (cJSON_IsString(password) && (password->valuestring != NULL)) {
                nvs_set_str(nvs_handle, NVS_KEY_PASS, password->valuestring);
            }
            
            if (cJSON_IsString(server_url) && (server_url->valuestring != NULL) && strlen(server_url->valuestring) > 0) {
                nvs_set_str(nvs_handle, NVS_KEY_URL, server_url->valuestring);
            }
            
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Credentials saved, rebooting...");
            
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            cJSON_Delete(json);
            
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

static const httpd_uri_t uri_get_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_html_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_get_scan = {
    .uri       = "/scan",
    .method    = HTTP_GET,
    .handler   = scan_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_post_connect = {
    .uri       = "/connect",
    .method    = HTTP_POST,
    .handler   = connect_handler,
    .user_ctx  = NULL
};

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    
    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_http_server, &uri_get_root);
        httpd_register_uri_handler(s_http_server, &uri_get_scan);
        httpd_register_uri_handler(s_http_server, &uri_post_connect);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_retry_num < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to the AP...");
        } else {
            ESP_LOGE(TAG, "Failed to connect to AP after retries");
            start_softap_provisioning();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        
        // Notify state machine
        xEventGroupSetBits(state_machine_get_events(), EVT_WIFI_CONNECTED);
        
        // Sync time
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, SNTP_SERVER);
        esp_sntp_init();
    }
}

static void start_softap_provisioning(void) {
    ESP_LOGI(TAG, "Starting SoftAP Provisioning Mode");
    state_machine_transition(DEVICE_STATE_UNPROVISIONED);
    
    // Stop STA if it was running
    esp_wifi_stop();
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    
    wifi_config_t wifi_config = {};
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    snprintf((char*)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s-%02X%02X", SOFTAP_SSID_PREFIX, mac[4], mac[5]);
    wifi_config.ap.ssid_len = strlen((char*)wifi_config.ap.ssid);
    
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "SoftAP started: SSID=%s", wifi_config.ap.ssid);
    
    start_webserver();
}

static void start_sta_connection(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    state_machine_transition(DEVICE_STATE_CONNECTING);
    
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_manager_init(void) {
    // esp_netif and the default event loop are initialized once in app_main()
    // before this is called; creating them again returns ESP_ERR_INVALID_STATE.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    
    nvs_handle_t nvs_handle;
    char ssid[32] = {0};
    char password[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);
    
    bool has_creds = false;
    
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        if (nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK) {
            has_creds = true;
            nvs_get_str(nvs_handle, NVS_KEY_PASS, password, &pass_len);
        }
        nvs_close(nvs_handle);
    }
    
    if (has_creds) {
        start_sta_connection(ssid, password);
    } else {
        start_softap_provisioning();
    }
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}

int wifi_manager_get_rssi(void) {
    if (!s_is_connected) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_manager_get_server_url(char *url_buf, size_t max_len) {
    if (url_buf == NULL || max_len == 0) return false;
    
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = max_len;
        esp_err_t err = nvs_get_str(nvs_handle, NVS_KEY_URL, url_buf, &len);
        nvs_close(nvs_handle);
        return (err == ESP_OK);
    }
    return false;
}
