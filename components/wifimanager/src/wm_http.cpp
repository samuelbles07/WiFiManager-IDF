#include "wm_config.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "cJSON.h"
#include <cstring>

// HTTP server implementation for WiFiManager captive portal

static httpd_handle_t s_server = nullptr;

// External binary data (embedded assets)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t wifi_html_start[] asm("_binary_wifi_html_start"); 
extern const uint8_t wifi_html_end[] asm("_binary_wifi_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t wm_js_start[] asm("_binary_wm_js_start");
extern const uint8_t wm_js_end[] asm("_binary_wm_js_end");

/**
 * Root handler - serve main portal page
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    WM_LOGD("Serving root page");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    // Serve embedded HTML
    const size_t index_html_len = index_html_end - index_html_start;
    httpd_resp_send(req, (const char*)index_html_start, index_html_len);
    
    return ESP_OK;
}

/**
 * WiFi scan handler - return JSON list of available networks
 */
static esp_err_t scan_get_handler(httpd_req_t *req) {
    WM_LOGD("WiFi scan requested");
    
    // Start WiFi scan
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = nullptr;
    scan_config.bssid = nullptr;
    scan_config.channel = 0;
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true); // Blocking scan
    if (ret != ESP_OK) {
        WM_LOGE("WiFi scan failed: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    wifi_ap_record_t *ap_records = nullptr;
    if (ap_count > 0) {
        ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_records) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
        }
    }
    
    // Create JSON response
    cJSON* networks_json = cJSON_CreateArray();
    if (networks_json && ap_records) {
        for (int i = 0; i < ap_count; i++) {
            cJSON* network = cJSON_CreateObject();
            if (network) {
                cJSON_AddStringToObject(network, "ssid", (char*)ap_records[i].ssid);
                cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
                cJSON_AddNumberToObject(network, "channel", ap_records[i].primary);
                cJSON_AddNumberToObject(network, "encryption", ap_records[i].authmode);
                cJSON_AddBoolToObject(network, "hidden", false);
                
                cJSON_AddItemToArray(networks_json, network);
            }
        }
    }
    
    char* json_string = cJSON_Print(networks_json);
    if (json_string) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, json_string, strlen(json_string));
        free(json_string);
    } else {
        httpd_resp_send_500(req);
    }
    
    // Cleanup
    if (ap_records) {
        free(ap_records);
    }
    cJSON_Delete(networks_json);
    
    return ESP_OK;
}

/**
 * WiFi save handler - process form submission with SSID/password
 */
static esp_err_t wifisave_post_handler(httpd_req_t *req) {
    WM_LOGD("WiFi save requested");
    
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    WM_LOGD("Received data: %s", buf);
    
    // Parse form data
    char ssid[33] = {0};
    char password[65] = {0};
    
    // Simple URL decode and parameter parsing
    char* ssid_param = strstr(buf, "s=");
    char* pass_param = strstr(buf, "p=");
    
    if (ssid_param) {
        ssid_param += 2; // Skip "s="
        char* end = strchr(ssid_param, '&');
        int len = end ? (end - ssid_param) : strlen(ssid_param);
        len = (len < sizeof(ssid) - 1) ? len : sizeof(ssid) - 1;
        strncpy(ssid, ssid_param, len);
        
        // URL decode (basic)
        for (int i = 0, j = 0; i < len; i++, j++) {
            if (ssid[i] == '+') {
                ssid[j] = ' ';
            } else if (ssid[i] == '%' && i + 2 < len) {
                // Hex decode
                char hex[3] = {ssid[i+1], ssid[i+2], 0};
                ssid[j] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                ssid[j] = ssid[i];
            }
        }
    }
    
    if (pass_param) {
        pass_param += 2; // Skip "p="
        char* end = strchr(pass_param, '&');
        int len = end ? (end - pass_param) : strlen(pass_param);
        len = (len < sizeof(password) - 1) ? len : sizeof(password) - 1;
        strncpy(password, pass_param, len);
        
        // URL decode (basic)
        for (int i = 0, j = 0; i < len; i++, j++) {
            if (password[i] == '+') {
                password[j] = ' ';
            } else if (password[i] == '%' && i + 2 < len) {
                char hex[3] = {password[i+1], password[i+2], 0};
                password[j] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                password[j] = password[i];
            }
        }
    }
    
    WM_LOGI("Connecting to SSID: %s", ssid);
    
    if (strlen(ssid) == 0) {
        // Send error response
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<html><body><h1>Error: SSID required</h1><a href='/'>Back</a></body></html>", -1);
        return ESP_OK;
    }
    
    // Configure WiFi with new credentials
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (strlen(password) > 0) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        WM_LOGE("Failed to set WiFi config: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Attempt connection
    esp_wifi_disconnect();
    esp_wifi_connect();
    
    // Send success response
    httpd_resp_set_type(req, "text/html");
    const char* success_msg = 
        "<html><body>"
        "<h1>Connecting...</h1>"
        "<p>Device is attempting to connect to the network.</p>"
        "<p>Please wait and check your device's connection status.</p>"
        "<script>setTimeout(function(){window.location.href='/';}, 5000);</script>"
        "</body></html>";
    httpd_resp_send(req, success_msg, strlen(success_msg));
    
    return ESP_OK;
}

/**
 * Info handler - display device and network information
 */
static esp_err_t info_get_handler(httpd_req_t *req) {
    WM_LOGD("Info page requested");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    // Get device info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    char html[2048];
    snprintf(html, sizeof(html),
        "<html><head><title>Device Info</title></head><body>"
        "<h1>Device Information</h1>"
        "<table border='1'>"
        "<tr><td>Chip</td><td>%s</td></tr>"
        "<tr><td>Cores</td><td>%d</td></tr>"
        "<tr><td>Revision</td><td>%d.%d</td></tr>"
        "<tr><td>WiFi</td><td>%s</td></tr>"
        "<tr><td>Bluetooth</td><td>%s</td></tr>"
        "<tr><td>Free Heap</td><td>%lu bytes</td></tr>"
        "</table>"
        "<p><a href='/'>Back to WiFi Manager</a></p>"
        "</body></html>",
        CONFIG_IDF_TARGET,
        chip_info.cores,
        chip_info.revision / 100,
        chip_info.revision % 100,
        "Yes", // All ESP32 variants have WiFi
        (chip_info.features & CHIP_FEATURE_BT) ? "Yes" : "No",
        esp_get_free_heap_size()
    );
    
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

/**
 * Exit handler - close captive portal
 */
static esp_err_t exit_get_handler(httpd_req_t *req) {
    WM_LOGD("Exit requested");
    
    httpd_resp_set_type(req, "text/html");
    const char* exit_msg = 
        "<html><body>"
        "<h1>Exiting WiFi Manager</h1>"
        "<p>Configuration portal is closing.</p>"
        "</body></html>";
    httpd_resp_send(req, exit_msg, strlen(exit_msg));
    
    // TODO: Signal to close portal
    return ESP_OK;
}

/**
 * Captive portal helpers for different operating systems
 */
static esp_err_t generate_204_handler(httpd_req_t *req) {
    WM_LOGD("Android captive portal check");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t hotspot_detect_handler(httpd_req_t *req) {
    WM_LOGD("iOS/macOS captive portal check");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ncsi_handler(httpd_req_t *req) {
    WM_LOGD("Windows captive portal check");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Microsoft NCSI", -1);
    return ESP_OK;
}

/**
 * Start HTTP server
 */
esp_err_t wm_http_server_start() {
    if (s_server != nullptr) {
        WM_LOGW("HTTP server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WM_HTTP_PORT;
    config.max_open_sockets = 7;
    config.stack_size = CONFIG_WM_HTTP_STACK_SIZE;
    
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        WM_LOGE("Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &root_uri);
    
    httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_get_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &scan_uri);
    
    httpd_uri_t wifisave_uri = {
        .uri = "/wifisave",
        .method = HTTP_POST,
        .handler = wifisave_post_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &wifisave_uri);
    
    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &info_uri);
    
    httpd_uri_t exit_uri = {
        .uri = "/exit",
        .method = HTTP_GET,
        .handler = exit_get_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &exit_uri);
    
    // Captive portal detection handlers
    httpd_uri_t generate_204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = generate_204_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &generate_204_uri);
    
    httpd_uri_t hotspot_detect_uri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = hotspot_detect_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &hotspot_detect_uri);
    
    httpd_uri_t ncsi_uri = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = ncsi_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &ncsi_uri);
    
    httpd_uri_t fwlink_uri = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = hotspot_detect_handler, // Redirect to root
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_server, &fwlink_uri);
    
    WM_LOGI("HTTP server started on port %d", config.server_port);
    return ESP_OK;
}

/**
 * Stop HTTP server
 */
esp_err_t wm_http_server_stop() {
    if (s_server == nullptr) {
        return ESP_OK;
    }
    
    esp_err_t ret = httpd_stop(s_server);
    s_server = nullptr;
    
    WM_LOGI("HTTP server stopped");
    return ret;
}

// Legacy placeholder functions
void wm_http_init() {
    WM_LOGD("HTTP server init");
}

void wm_http_deinit() {
    WM_LOGD("HTTP server deinit");
} 