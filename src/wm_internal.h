#pragma once

#include "wm_config.h"
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi state management functions
esp_err_t wm_wifi_init();
esp_err_t wm_wifi_deinit();
esp_err_t wm_wifi_start_sta();
esp_err_t wm_wifi_start_ap(const char* ssid, const char* password);
esp_err_t wm_wifi_set_ap_ip(const char* ip, const char* gateway, const char* netmask);
esp_err_t wm_wifi_set_sta_ip(const char* ip, const char* gateway, const char* netmask, const char* dns);
esp_err_t wm_wifi_stop();
bool wm_wifi_is_configured();
esp_err_t wm_wifi_get_ssid(char* ssid, size_t len);
wl_status_t wm_map_disconnect_reason(wifi_err_reason_t reason);
const char* wm_wifi_mode_string(wifi_mode_t mode);

// DNS server functions
esp_err_t wm_dns_server_start(const char* ap_ip);
esp_err_t wm_dns_server_stop();

// HTTP server functions  
esp_err_t wm_http_server_start();
esp_err_t wm_http_server_stop();

// WiFi scanning functions
esp_err_t wm_wifi_scan_start();
esp_err_t wm_wifi_scan_get_results(wifi_ap_record_t* ap_records, uint16_t* ap_count);

// NVS storage functions
esp_err_t wm_nvs_save_wifi_config(const char* ssid, const char* password);
esp_err_t wm_nvs_load_custom_params();
esp_err_t wm_nvs_save_custom_params();
esp_err_t wm_nvs_erase_all();

// Legacy placeholder functions
void wm_state_init();
void wm_state_deinit();

#ifdef __cplusplus
}
#endif 