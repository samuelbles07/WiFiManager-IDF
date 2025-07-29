#include "wm_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/ip4_addr.h"
#include <cstring>

// WiFi state management implementation

static esp_netif_t* s_ap_netif = nullptr;
static esp_netif_t* s_sta_netif = nullptr;
static bool s_wifi_initialized = false;

/**
 * Initialize WiFi subsystem
 */
esp_err_t wm_wifi_init() {
    if (s_wifi_initialized) {
        WM_LOGD("WiFi already initialized");
        return ESP_OK;
    }

    // Create default network interfaces
    ESP_ERROR_CHECK(esp_netif_init());
    
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    
    if (!s_ap_netif || !s_sta_netif) {
        WM_LOGE("Failed to create network interfaces");
        return ESP_FAIL;
    }

    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set WiFi storage to flash for persistent credentials
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    
    s_wifi_initialized = true;
    WM_LOGI("WiFi subsystem initialized");
    
    return ESP_OK;
}

/**
 * Deinitialize WiFi subsystem  
 */
esp_err_t wm_wifi_deinit() {
    if (!s_wifi_initialized) {
        return ESP_OK;
    }
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = nullptr;
    }
    
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = nullptr;
    }
    
    s_wifi_initialized = false;
    WM_LOGI("WiFi subsystem deinitialized");
    
    return ESP_OK;
}

/**
 * Start WiFi in STA mode and attempt connection
 */
esp_err_t wm_wifi_start_sta() {
    if (!s_wifi_initialized) {
        WM_LOGE("WiFi not initialized");
        return ESP_FAIL;
    }
    
    WM_LOGI("Starting WiFi in STA mode");
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Trigger connection attempt with saved credentials
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        WM_LOGW("WiFi connect failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

/**
 * Start WiFi in AP mode for captive portal
 */
esp_err_t wm_wifi_start_ap(const char* ssid, const char* password) {
    if (!s_wifi_initialized) {
        WM_LOGE("WiFi not initialized");
        return ESP_FAIL;
    }
    
    WM_LOGI("Starting WiFi in AP mode: %s", ssid);
    
    // Configure AP
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = WM_DEFAULT_AP_CHANNEL;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100;
    
    if (password && strlen(password) > 0) {
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char*)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
        WM_LOGI("AP configured with WPA2-PSK security");
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        WM_LOGI("AP configured as open network");
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    WM_LOGI("AP started successfully");
    return ESP_OK;
}

/**
 * Configure AP static IP
 */
esp_err_t wm_wifi_set_ap_ip(const char* ip, const char* gateway, const char* netmask) {
    if (!s_ap_netif) {
        WM_LOGE("AP network interface not available");
        return ESP_FAIL;
    }
    
    // Stop DHCP server first
    esp_netif_dhcps_stop(s_ap_netif);
    
    // Set static IP configuration
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(ip, &ip_info.ip);
    esp_netif_str_to_ip4(gateway, &ip_info.gw); 
    esp_netif_str_to_ip4(netmask, &ip_info.netmask);
    
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    
    // Restart DHCP server
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    
    WM_LOGI("AP IP configured: %s, GW: %s, Netmask: %s", ip, gateway, netmask);
    return ESP_OK;
}

/**
 * Configure STA static IP
 */
esp_err_t wm_wifi_set_sta_ip(const char* ip, const char* gateway, const char* netmask, const char* dns) {
    if (!s_sta_netif) {
        WM_LOGE("STA network interface not available");
        return ESP_FAIL;
    }
    
    // Stop DHCP client
    esp_netif_dhcpc_stop(s_sta_netif);
    
        // Set static IP configuration
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(ip, &ip_info.ip);
    esp_netif_str_to_ip4(gateway, &ip_info.gw);
    esp_netif_str_to_ip4(netmask, &ip_info.netmask);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ip_info));
    
    // Set DNS server if provided
    if (dns && strlen(dns) > 0) {
        esp_netif_dns_info_t dns_info;
        esp_netif_str_to_ip4(dns, &dns_info.ip.u_addr.ip4);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        WM_LOGI("STA DNS configured: %s", dns);
    }
    
    WM_LOGI("STA IP configured: %s, GW: %s, Netmask: %s", ip, gateway, netmask);
    return ESP_OK;
}

/**
 * Stop WiFi
 */
esp_err_t wm_wifi_stop() {
    if (!s_wifi_initialized) {
        return ESP_OK;
    }
    
    WM_LOGI("Stopping WiFi");
    esp_wifi_stop();
    return ESP_OK;
}

/**
 * Check if WiFi credentials are saved
 */
bool wm_wifi_is_configured() {
    wifi_config_t wifi_config;
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    
    if (ret != ESP_OK) {
        WM_LOGD("Failed to get WiFi config: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Check if SSID is set
    bool configured = (strlen((char*)wifi_config.sta.ssid) > 0);
    WM_LOGD("WiFi configured: %s (SSID: %s)", configured ? "yes" : "no", 
            configured ? (char*)wifi_config.sta.ssid : "none");
    
    return configured;
}

/**
 * Get current WiFi SSID
 */
esp_err_t wm_wifi_get_ssid(char* ssid, size_t len) {
    if (!ssid || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    wifi_config_t wifi_config;
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    
    if (ret == ESP_OK) {
        strncpy(ssid, (char*)wifi_config.sta.ssid, len - 1);
        ssid[len - 1] = '\0';
    }
    
    return ret;
}

/**
 * Map ESP-IDF disconnect reason to WiFiManager status
 */
wl_status_t wm_map_disconnect_reason(wifi_err_reason_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return WL_NO_SSID_AVAIL;
            
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_ASSOC_EXPIRE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_NOT_AUTHED:
        case WIFI_REASON_NOT_ASSOCED:
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        case WIFI_REASON_IE_INVALID:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        case WIFI_REASON_INVALID_RSN_IE_CAP:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_INVALID_PMKID:
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_AP_TSF_RESET:
        case WIFI_REASON_ROAMING:
            return WL_WRONG_PASSWORD;
            
        case WIFI_REASON_UNSPECIFIED:
        default:
            return WL_CONNECT_FAILED;
    }
}

/**
 * Get WiFi mode string for debugging
 */
const char* wm_wifi_mode_string(wifi_mode_t mode) {
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default: return "UNKNOWN";
    }
}

// Placeholder functions from original file
void wm_state_init() {
    WM_LOGD("State machine initialized");
}

void wm_state_deinit() {
    WM_LOGD("State machine deinitialized");
} 