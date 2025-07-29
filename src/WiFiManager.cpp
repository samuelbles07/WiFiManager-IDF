#include "WiFiManager.h"
#include "wm_config.h"
#include "wm_internal.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <algorithm>

WiFiManager::WiFiManager() :
    _state(WM_STATE_INIT),
    _connectTimeout(WM_DEFAULT_CONNECT_TIMEOUT * 1000000ULL), // Convert to microseconds
    _configPortalTimeout(WM_DEFAULT_PORTAL_TIMEOUT * 1000000ULL),
    _configPortalBlocking(true),
    _breakAfterConfig(false),
    _apStaticIPSet(false),
    _staStaticIPSet(false),
    _minimumQuality(WM_MIN_QUALITY),
    _removeDuplicateAPs(CONFIG_WM_REMOVE_DUP_APS),
    _scanDispPerc(false),
    _lastScanTime(0),
    _scanInProgress(false),
    _captivePortalEnable(true),
    _captivePortalClientCheck(true),
    _webPortalClientCheck(true),
    _apNetif(nullptr),
    _staNetif(nullptr),
    _httpServer(nullptr),
    _wifiEventHandler(nullptr),
    _ipEventHandler(nullptr),
    _lastConxResult(WL_IDLE_STATUS),
    _portalAbortResult(false),
    _configPortalStart(0),
    _connectStart(0),
    _dnsTaskHandle(nullptr),
    _dnsSocket(-1),
    _dnsRunning(false),
    _initialized(false),
    _cleanupInProgress(false)
{
    WM_LOGI("WiFiManager constructor");
    // Don't call init() here - it will be called when needed
}

WiFiManager::~WiFiManager() {
    WM_LOGI("WiFiManager destructor");
    cleanup();
}

void WiFiManager::init() {
    if (_initialized) {
        WM_LOGD("WiFiManager already initialized");
        return;
    }
    
    WM_LOGI("Initializing WiFiManager...");
    
    // Initialize NVS if not already done
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the underlying TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Initialize the event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Generate default AP name if not set
    if (_apName.empty()) {
        _apName = generateDefaultAPName();
    }

    _initialized = true;
    WM_LOGI("WiFiManager initialized with AP name: %s", _apName.c_str());
}

void WiFiManager::cleanup() {
    stopHTTPServer();
    stopDNSServer();
    stopWiFi();
    
    // Unregister event handlers
    if (_wifiEventHandler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifiEventHandler);
        _wifiEventHandler = nullptr;
    }
    if (_ipEventHandler) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, _ipEventHandler);
        _ipEventHandler = nullptr;
    }
}

std::string WiFiManager::generateDefaultAPName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    char apName[32];
    snprintf(apName, sizeof(apName), "%s-%02X%02X%02X", 
             CONFIG_WM_DEFAULT_AP_SSID, mac[3], mac[4], mac[5]);
    
    return std::string(apName);
}

void WiFiManager::stopServers() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    WM_LOGI("🛑 Manually stopping servers...");
    
    if (!_cleanupInProgress) {
        _cleanupInProgress = true;
        
        stopHTTPServer();
        stopDNSServer();
        
        _cleanupInProgress = false;
        WM_LOGI("✅ Servers stopped successfully");
    } else {
        WM_LOGD("Server cleanup already in progress");
    }
}

bool WiFiManager::resetSettings() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    WM_LOGI("🔄 Resetting WiFi credentials...");
    
    // Disconnect from current WiFi
    esp_wifi_disconnect();
    
    // Clear WiFi credentials from ESP-IDF's NVS storage
    esp_err_t ret = esp_wifi_restore();
    if (ret != ESP_OK) {
        WM_LOGE("❌ Failed to clear WiFi credentials: %s", esp_err_to_name(ret));
        return false;
    }
    
    WM_LOGI("✅ WiFi credentials reset successfully - device will need reconfiguration");
    return true;
}

// Core connection methods
bool WiFiManager::autoConnect() {
    return autoConnect(_apName.c_str(), _apPassword.empty() ? nullptr : _apPassword.c_str());
}

bool WiFiManager::autoConnect(const char* apName) {
    return autoConnect(apName, nullptr);
}

bool WiFiManager::autoConnect(const char* apName, const char* apPassword) {
    std::lock_guard<std::mutex> lock(_mutex);
    
    WM_LOGI("AutoConnect called with AP: %s", apName ? apName : "null");
    
    // Initialize if not already done
    init();
    
    if (apName) {
        _apName = apName;
    }
    if (apPassword) {
        _apPassword = apPassword;
    }
    
    _state = WM_STATE_INIT;
    _portalAbortResult = false;
    _lastConxResult = WL_IDLE_STATUS;
    
    // Setup WiFi
    if (!setupWiFi()) {
        WM_LOGE("Failed to setup WiFi");
        return false;
    }
    
    // First, try to connect using saved credentials
    if (getWiFiIsSaved() && startSTA()) {
        _state = WM_STATE_TRY_STA;
        _connectStart = esp_timer_get_time();
        
        if (_configPortalBlocking) {
            // Wait for connection or timeout
            while (_state == WM_STATE_TRY_STA) {
                updateState();
                vTaskDelay(pdMS_TO_TICKS(100));
                
                if (esp_timer_get_time() - _connectStart > _connectTimeout) {
                    WM_LOGW("STA connection timeout");
                    break;
                }
            }
            
            if (_state == WM_STATE_RUN_STA) {
                WM_LOGI("AutoConnect successful");
                return true;
            }
        } else {
            // Non-blocking mode, return true to indicate process started
            return true;
        }
    }
    
    // STA failed or no saved credentials, start config portal
    WM_LOGI("Starting config portal");
    bool portalResult = startConfigPortalInternal(_apName.c_str(), _apPassword.empty() ? nullptr : _apPassword.c_str());
    
    // If successful connection via portal, switch to STA-only mode
    if (portalResult && _state == WM_STATE_RUN_STA) {
        WM_LOGI("🎉 WiFi connected successfully! Switching to STA-only mode...");
        
        // Switch to STA-only mode (servers remain running for manual control)
        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret == ESP_OK) {
            WM_LOGI("✅ Successfully switched to STA-only mode");
            WM_LOGI("💡 Servers still running - call stopServers() manually to stop them");
        } else {
            WM_LOGW("⚠️ Failed to switch to STA mode: %s", esp_err_to_name(ret));
        }
    }
    
    return portalResult;
}

bool WiFiManager::startConfigPortal() {
    std::lock_guard<std::mutex> lock(_mutex);
    return startConfigPortalInternal(_apName.c_str(), _apPassword.empty() ? nullptr : _apPassword.c_str());
}

bool WiFiManager::startConfigPortal(const char* apName) {
    std::lock_guard<std::mutex> lock(_mutex);
    return startConfigPortalInternal(apName, nullptr);
}

bool WiFiManager::startConfigPortal(const char* apName, const char* apPassword) {
    std::lock_guard<std::mutex> lock(_mutex);
    return startConfigPortalInternal(apName, apPassword);
}

bool WiFiManager::startConfigPortalInternal(const char* apName, const char* apPassword) {
    WM_LOGI("StartConfigPortal called with AP: %s", apName ? apName : "null");
    
    // Initialize if not already done
    init();
    
    if (apName) {
        _apName = apName;
    }
    if (apPassword) {
        _apPassword = apPassword;
    }
    
    _state = WM_STATE_START_PORTAL;
    _portalAbortResult = false;
    _configPortalStart = esp_timer_get_time();
    
    WM_LOGI("🔧 Setting up WiFi subsystem...");
    // Setup WiFi if not already done
    if (!setupWiFi()) {
        WM_LOGE("❌ Failed to setup WiFi");
        return false;
    }
    
    WM_LOGI("📡 Starting AP mode with SSID: %s", _apName.c_str());
    // Start AP mode
    if (!startAP(_apName.c_str(), _apPassword.empty() ? nullptr : _apPassword.c_str())) {
        WM_LOGE("❌ Failed to start AP");
        return false;
    }
    
    WM_LOGI("🌐 Starting HTTP server...");
    // Start HTTP server
    if (!startHTTPServer()) {
        WM_LOGE("❌ Failed to start HTTP server");
        return false;
    }
    
    WM_LOGI("🔍 Starting DNS server for captive portal...");
    // Start DNS server for captive portal
    if (_captivePortalEnable && !startDNSServer()) {
        WM_LOGW("⚠️  Failed to start DNS server");
    }
    
    _state = WM_STATE_RUN_PORTAL;
    
    // Trigger AP callback
    if (_apCallback) {
        _apCallback(this);
    }
    
    WM_LOGI("✅ Config portal started successfully!");
    WM_LOGI("📱 Connect to WiFi network: %s", _apName.c_str());
    WM_LOGI("🌐 Open browser to: http://192.168.4.1");
    
    if (_configPortalBlocking) {
        // Blocking mode - wait for completion
        while (_state == WM_STATE_RUN_PORTAL || _state == WM_STATE_TRY_STA) {
            updateState();
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Check for timeout
            if (_configPortalTimeout > 0 && 
                esp_timer_get_time() - _configPortalStart > _configPortalTimeout) {
                WM_LOGW("Config portal timeout");
                _state = WM_STATE_PORTAL_TIMEOUT;
                break;
            }
        }
        
        // Check final state but don't cleanup servers yet
        if (_state == WM_STATE_RUN_STA) {
            WM_LOGI("✅ Config portal completed successfully");
            return true;
        } else {
            WM_LOGI("⏰ Config portal completed with timeout/abort");
            WM_LOGI("💡 Servers still running - call stopServers() manually to stop them");
            return false;
        }
    }
    
    // Non-blocking mode
    return true;
}

void WiFiManager::startWebPortal() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    WM_LOGI("Starting web portal");
    
    if (!startHTTPServer()) {
        WM_LOGE("Failed to start web portal HTTP server");
        return;
    }
    
    if (_webServerModeCallback) {
        _webServerModeCallback();
    }
    
    WM_LOGI("Web portal started");
}

void WiFiManager::stopWebPortal() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    WM_LOGI("Stopping web portal");
    stopHTTPServer();
}

bool WiFiManager::process() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    updateState();
    
    // Return true if still processing, false if finished
    return (_state == WM_STATE_TRY_STA || _state == WM_STATE_RUN_PORTAL);
}

// Configuration methods
void WiFiManager::setConfigPortalTimeout(uint32_t seconds) {
    _configPortalTimeout = seconds * 1000000ULL; // Convert to microseconds
    WM_LOGD("Config portal timeout set to %u seconds", seconds);
}

void WiFiManager::setConnectTimeout(uint32_t seconds) {
    _connectTimeout = seconds * 1000000ULL; // Convert to microseconds
    WM_LOGD("Connect timeout set to %u seconds", seconds);
}

void WiFiManager::setConfigPortalBlocking(bool shouldBlock) {
    _configPortalBlocking = shouldBlock;
    WM_LOGD("Config portal blocking set to %s", shouldBlock ? "true" : "false");
}

void WiFiManager::setBreakAfterConfig(bool shouldBreak) {
    _breakAfterConfig = shouldBreak;
    WM_LOGD("Break after config set to %s", shouldBreak ? "true" : "false");
}

// Callback registration methods
void WiFiManager::setAPCallback(APCallback callback) {
    _apCallback = callback;
}

void WiFiManager::setSaveConfigCallback(SaveConfigCallback callback) {
    _saveConfigCallback = callback;
}

void WiFiManager::setConfigModeCallback(ConfigModeCallback callback) {
    _configModeCallback = callback;
}

void WiFiManager::setWebServerModeCallback(WebServerModeCallback callback) {
    _webServerModeCallback = callback;
}

// Status methods
bool WiFiManager::isConfigPortalActive() const {
    return _state == WM_STATE_RUN_PORTAL;
}

bool WiFiManager::isWebPortalActive() const {
    return _httpServer != nullptr;
}

wl_status_t WiFiManager::getLastConxResult() const {
    return _lastConxResult;
}

// WiFi management implementations
bool WiFiManager::setupWiFi() {
    WM_LOGD("Setting up WiFi subsystem");
    
    // Initialize NVS if not already done
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create default network interfaces if not already created
    if (!_apNetif) {
        _apNetif = esp_netif_create_default_wifi_ap();
    }
    if (!_staNetif) {
        _staNetif = esp_netif_create_default_wifi_sta();
    }
    
    if (!_apNetif || !_staNetif) {
        WM_LOGE("Failed to create network interfaces");
        return false;
    }

    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set WiFi storage to flash for persistent credentials
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    
    // Register event handlers
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                      &WiFiManager::wifiEventHandler, this, &_wifiEventHandler);
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                      &WiFiManager::ipEventHandler, this, &_ipEventHandler);
    
    // Configure default AP IP
    esp_netif_dhcps_stop(_apNetif);
    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(CONFIG_WM_AP_IP, &ip_info.ip);
    esp_netif_str_to_ip4(CONFIG_WM_AP_GW, &ip_info.gw); 
    esp_netif_str_to_ip4(CONFIG_WM_AP_NETMASK, &ip_info.netmask);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(_apNetif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(_apNetif));
    
    WM_LOGI("WiFi subsystem initialized");
    return true;
}

bool WiFiManager::startSTA() {
    WM_LOGD("Starting STA mode");
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Trigger connection attempt with saved credentials
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        WM_LOGW("WiFi connect failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool WiFiManager::startAP(const char* ssid, const char* password) {
    WM_LOGI("🚀 Starting AP mode: %s", ssid);
    
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
        WM_LOGI("🔒 AP configured with WPA2-PSK security");
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        WM_LOGI("🔓 AP configured as open network");
    }
    
    WM_LOGI("🔧 Setting WiFi mode to AP...");
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        WM_LOGE("❌ Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return false;
    }
    
    WM_LOGI("⚙️  Setting AP configuration...");
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        WM_LOGE("❌ Failed to set AP config: %s", esp_err_to_name(ret));
        return false;
    }
    
    WM_LOGI("🎯 Starting WiFi driver...");
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        WM_LOGE("❌ Failed to start WiFi: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Wait a bit for AP to fully start
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    WM_LOGI("✅ AP started successfully!");
    WM_LOGI("📡 SSID: %s", ssid);
    WM_LOGI("🔢 Channel: %d", WM_DEFAULT_AP_CHANNEL);
    WM_LOGI("🌐 IP: 192.168.4.1");
    
    return true;
}

void WiFiManager::stopWiFi() {
    WM_LOGD("Stopping WiFi");
    esp_wifi_stop();
}

bool WiFiManager::startHTTPServer() {
    WM_LOGD("Starting HTTP server");
    
    if (_httpServer != nullptr) {
        WM_LOGW("HTTP server already running");
        return true;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WM_HTTP_PORT;
    config.max_open_sockets = 7;
    config.stack_size = CONFIG_WM_HTTP_STACK_SIZE;
    config.max_uri_handlers = 12; // Increase handler limit
    config.global_user_ctx = this; // Store WiFiManager instance for handlers
    
    esp_err_t ret = httpd_start(&_httpServer, &config);
    if (ret != ESP_OK) {
        WM_LOGE("Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handleRoot,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &root_uri);
    
    httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = handleScan,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &scan_uri);
    
    httpd_uri_t wifisave_uri = {
        .uri = "/wifisave",
        .method = HTTP_POST,
        .handler = handleWifiSave,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &wifisave_uri);
    
    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = handleInfo,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &info_uri);
    
    httpd_uri_t exit_uri = {
        .uri = "/exit",
        .method = HTTP_GET,
        .handler = handleExit,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &exit_uri);
    
    // Captive portal detection handlers
    httpd_uri_t generate_204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = handleCaptivePortal,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &generate_204_uri);
    
    httpd_uri_t hotspot_detect_uri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = handleCaptivePortal,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &hotspot_detect_uri);
    
    httpd_uri_t ncsi_uri = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = handleCaptivePortal,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &ncsi_uri);
    
    httpd_uri_t fwlink_uri = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = handleCaptivePortal,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &fwlink_uri);
    
    // Add /wifi route (redirect to root)
    httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = handleRoot,
        .user_ctx = this
    };
    httpd_register_uri_handler(_httpServer, &wifi_uri);
    
    WM_LOGI("HTTP server started on port %d", config.server_port);
    return true;
}

void WiFiManager::stopHTTPServer() {
    if (_httpServer) {
        WM_LOGI("🛑 Stopping HTTP server...");
        
        esp_err_t ret = httpd_stop(_httpServer);
        if (ret == ESP_OK) {
            WM_LOGI("✅ HTTP server stopped successfully");
            _httpServer = nullptr;
        } else {
            WM_LOGW("⚠️ Error stopping HTTP server: %s", esp_err_to_name(ret));
            _httpServer = nullptr;  // Clear handle anyway to prevent issues
        }
    } else {
        WM_LOGD("HTTP server already stopped");
    }
}



bool WiFiManager::startDNSServer() {
    WM_LOGD("Starting DNS server");
    
    if (_dnsRunning) {
        WM_LOGW("DNS server already running");
        return true;
    }
    
    _dnsRunning = true;
    
    BaseType_t ret = xTaskCreate(dnsServerTask, "dns_server", 
                                CONFIG_WM_DNS_STACK_SIZE, this, 5, &_dnsTaskHandle);
    
    if (ret != pdPASS) {
        WM_LOGE("Failed to create DNS server task");
        _dnsRunning = false;
        return false;
    }
    
    WM_LOGI("DNS server started");
    return true;
}

void WiFiManager::stopDNSServer() {
    if (!_dnsRunning) {
        WM_LOGD("DNS server already stopped");
        return;
    }
    
    WM_LOGI("🛑 Stopping DNS server...");
    _dnsRunning = false;
    
    // Close socket to wake up task
    if (_dnsSocket >= 0) {
        close(_dnsSocket);
        _dnsSocket = -1;
    }
    
    // Wait for task to finish
    int timeout = 50; // 5 seconds
    while (_dnsTaskHandle && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (_dnsTaskHandle) {
        WM_LOGW("DNS task did not terminate gracefully, deleting");
        vTaskDelete(_dnsTaskHandle);
        _dnsTaskHandle = nullptr;
    }
    
    WM_LOGI("✅ DNS server stopped successfully");
}

void WiFiManager::updateState() {
    // State machine logic based on current state
    switch (_state) {
        case WM_STATE_TRY_STA:
            // Check connection timeout
            if (esp_timer_get_time() - _connectStart > _connectTimeout) {
                WM_LOGW("STA connection timeout");
                _lastConxResult = WL_CONNECT_FAILED;
                _state = WM_STATE_START_PORTAL;
            }
            break;
            
        case WM_STATE_RUN_PORTAL:
            // Check portal timeout
            if (_configPortalTimeout > 0 && 
                esp_timer_get_time() - _configPortalStart > _configPortalTimeout) {
                WM_LOGW("Config portal timeout");
                _state = WM_STATE_PORTAL_TIMEOUT;
            }
            break;
            
        default:
            break;
    }
}

bool WiFiManager::getWiFiIsSaved() const {
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

// Additional missing methods
void WiFiManager::setMinimumSignalQuality(int percent) {
    _minimumQuality = percent;
    WM_LOGD("Minimum signal quality set to %d%%", percent);
}

void WiFiManager::setRemoveDuplicateAPs(bool remove) {
    _removeDuplicateAPs = remove;
    WM_LOGD("Remove duplicate APs set to %s", remove ? "true" : "false");
}

void WiFiManager::addParameter(WiFiManagerParameter* parameter) {
    if (parameter && _params.size() < WM_MAX_CUSTOM_PARAMS) {
        _params.push_back(std::unique_ptr<WiFiManagerParameter>(parameter));
        WM_LOGD("Added custom parameter: %s", parameter->getID());
    }
}

std::string WiFiManager::getSSID() const {
    wifi_config_t wifi_config;
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        return std::string((char*)wifi_config.sta.ssid);
    }
    return std::string();
}

std::string WiFiManager::getPassword() const {
    // TODO: Get current password (should we return this?)
    WM_LOGD("getPassword placeholder");
    return std::string();
}

// Event handlers
void WiFiManager::wifiEventHandler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
    WiFiManager* manager = static_cast<WiFiManager*>(arg);
    if (!manager) return;
    
    WM_LOGD("WiFi event: %s, ID: %ld", event_base, event_id);
    
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            WM_LOGI("STA started");
            break;
            
        case WIFI_EVENT_STA_CONNECTED:
            WM_LOGI("STA connected to AP");
            break;
            
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* disconnected = 
                static_cast<wifi_event_sta_disconnected_t*>(event_data);
            WM_LOGW("STA disconnected, reason: %d", disconnected->reason);
            
            // Map disconnect reason to wl_status_t
            switch (disconnected->reason) {
                case WIFI_REASON_NO_AP_FOUND:
                case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
                case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                    manager->_lastConxResult = WL_NO_SSID_AVAIL;
                    break;
                case WIFI_REASON_AUTH_EXPIRE:
                case WIFI_REASON_AUTH_LEAVE:
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
                case WIFI_REASON_802_1X_AUTH_FAILED:
                    manager->_lastConxResult = WL_WRONG_PASSWORD;
                    break;
                default:
                    manager->_lastConxResult = WL_CONNECT_FAILED;
                    break;
            }
            
            // If we're in STA connection attempt, transition to portal
            if (manager->_state == WM_STATE_TRY_STA) {
                manager->_state = WM_STATE_START_PORTAL;
            }
            break;
        }
        
        case WIFI_EVENT_AP_START:
            WM_LOGI("AP started");
            break;
            
        case WIFI_EVENT_AP_STOP:
            WM_LOGI("AP stopped");
            break;
            
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event = 
                static_cast<wifi_event_ap_staconnected_t*>(event_data);
            WM_LOGI("Station connected to AP, MAC: " MACSTR, MAC2STR(event->mac));
            break;
        }
        
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event = 
                static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
            WM_LOGI("Station disconnected from AP, MAC: " MACSTR, MAC2STR(event->mac));
            break;
        }
        
        default:
            WM_LOGD("Unhandled WiFi event: %ld", event_id);
            break;
    }
}

void WiFiManager::ipEventHandler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    WiFiManager* manager = static_cast<WiFiManager*>(arg);
    if (!manager) return;
    
    WM_LOGD("IP event: %s, ID: %ld", event_base, event_id);
    
    switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
            WM_LOGI("STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            WM_LOGI("STA netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
            WM_LOGI("STA gateway: " IPSTR, IP2STR(&event->ip_info.gw));
            
            manager->_lastConxResult = WL_CONNECTED;
            manager->_state = WM_STATE_RUN_STA;
            
            // Trigger save config callback
            if (manager->_saveConfigCallback) {
                manager->_saveConfigCallback();
            }
            break;
        }
        
        case IP_EVENT_STA_LOST_IP:
            WM_LOGW("STA lost IP");
            manager->_lastConxResult = WL_CONNECTION_LOST;
            break;
            
        case IP_EVENT_AP_STAIPASSIGNED: {
            ip_event_ap_staipassigned_t* event = 
                static_cast<ip_event_ap_staipassigned_t*>(event_data);
            WM_LOGI("AP assigned IP to station: " IPSTR, IP2STR(&event->ip));
            break;
        }
        
        default:
            WM_LOGD("Unhandled IP event: %ld", event_id);
            break;
    }
}

// Additional utility methods (getSSID already defined above)

const char* WiFiManager::getWLStatusString(wl_status_t status) const {
    switch (status) {
        case WL_IDLE_STATUS: return "Idle";
        case WL_NO_SSID_AVAIL: return "No SSID Available"; 
        case WL_SCAN_COMPLETED: return "Scan Completed";
        case WL_CONNECTED: return "Connected";
        case WL_CONNECT_FAILED: return "Connect Failed";
        case WL_CONNECTION_LOST: return "Connection Lost";
        case WL_WRONG_PASSWORD: return "Wrong Password";
        case WL_DISCONNECTED: return "Disconnected";
        default: return "Unknown";
    }
}

const char* WiFiManager::getModeString(wifi_mode_t mode) const {
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default: return "UNKNOWN";
    }
}

// DNS Server Implementation

#define DNS_PORT 53
#define DNS_MAX_PACKET_SIZE 512

// DNS header structure
typedef struct {
    uint16_t id;        // Identification
    uint16_t flags;     // Flags
    uint16_t qdcount;   // Question count
    uint16_t ancount;   // Answer count
    uint16_t nscount;   // Authority count
    uint16_t arcount;   // Additional count
} __attribute__((packed)) dns_header_t;

// DNS flags
#define DNS_FLAG_RESPONSE   0x8000
#define DNS_FLAG_AA         0x0400  // Authoritative Answer

void WiFiManager::dnsServerTask(void* pvParameters) {
    WiFiManager* manager = static_cast<WiFiManager*>(pvParameters);
    
    struct sockaddr_in server_addr = {};
    struct sockaddr_in client_addr;
    uint8_t buffer[DNS_MAX_PACKET_SIZE];
    socklen_t client_len = sizeof(client_addr);
    int reuse = 1;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DNS_PORT);
    
    manager->_dnsSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (manager->_dnsSocket < 0) {
        WM_LOGE("Failed to create DNS socket");
        goto cleanup;
    }
    
    setsockopt(manager->_dnsSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    if (bind(manager->_dnsSocket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        WM_LOGE("Failed to bind DNS socket");
        goto cleanup;
    }
    
    WM_LOGI("DNS server started on port %d", DNS_PORT);
    
    while (manager->_dnsRunning) {
        int len = recvfrom(manager->_dnsSocket, buffer, sizeof(buffer) - 1, 0,
                          (struct sockaddr*)&client_addr, &client_len);
        
        if (len > 0) {
            WM_LOGD("DNS query from " IPSTR ":%d, length: %d", 
                   IP2STR((ip4_addr_t*)&client_addr.sin_addr), 
                   ntohs(client_addr.sin_port), len);
            
            // Create simple DNS response that redirects all queries to AP IP
            if (len >= sizeof(dns_header_t)) {
                dns_header_t* header = (dns_header_t*)buffer;
                
                // Modify header for response
                header->flags = htons(ntohs(header->flags) | DNS_FLAG_RESPONSE | DNS_FLAG_AA);
                header->ancount = header->qdcount; // Same number of answers as questions
                header->nscount = 0;
                header->arcount = 0;
                
                // For simplicity, we'll append a basic A record response
                // This is a simplified implementation that works for most cases
                uint8_t response[DNS_MAX_PACKET_SIZE];
                memcpy(response, buffer, len);
                int response_len = len;
                
                // Add a simple A record pointing to AP IP (192.168.4.1)
                if (response_len + 16 < sizeof(response)) {
                    // Name (compressed pointer to question)
                    response[response_len++] = 0xC0;
                    response[response_len++] = 0x0C; // Pointer to offset 12 (question)
                    
                    // Type (A record)
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x01;
                    
                    // Class (Internet)
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x01;
                    
                    // TTL (60 seconds)
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x3C;
                    
                    // Data length (4 bytes for IPv4)
                    response[response_len++] = 0x00;
                    response[response_len++] = 0x04;
                    
                    // IP address (AP IP: 192.168.4.1)
                    response[response_len++] = 192;
                    response[response_len++] = 168;
                    response[response_len++] = 4;
                    response[response_len++] = 1;
                }
                
                sendto(manager->_dnsSocket, response, response_len, 0,
                       (struct sockaddr*)&client_addr, client_len);
                WM_LOGV("DNS response sent, length: %d", response_len);
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (manager->_dnsRunning) {
                WM_LOGE("DNS recvfrom error: %d", errno);
            }
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent tight loop
    }
    
cleanup:
    if (manager->_dnsSocket >= 0) {
        close(manager->_dnsSocket);
        manager->_dnsSocket = -1;
    }
    
    WM_LOGI("DNS server task ended");
    manager->_dnsTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// Enhanced WiFi Scanning Implementation

void WiFiManager::performWiFiScan(bool async) {
    if (_scanInProgress) {
        WM_LOGD("Scan already in progress");
        return;
    }
    
    WM_LOGI("🔍 Starting WiFi scan (async: %s)", async ? "true" : "false");
    _scanInProgress = true;
    
    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    
    // If we're in pure AP mode, temporarily switch to AP+STA for scanning
    bool mode_changed = false;
    if (current_mode == WIFI_MODE_AP) {
        WM_LOGI("🔄 Switching to AP+STA mode for scanning...");
        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            WM_LOGE("❌ Failed to set APSTA mode for scanning: %s", esp_err_to_name(ret));
            _scanInProgress = false;
            return;
        }
        mode_changed = true;
        // Give it a moment to switch modes
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Configure scan parameters
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = nullptr;
    scan_config.bssid = nullptr;
    scan_config.channel = 0;
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, !async);
    if (ret != ESP_OK) {
        WM_LOGE("❌ WiFi scan failed: %s", esp_err_to_name(ret));
        _scanInProgress = false;
        
        // Restore original mode if we changed it
        if (mode_changed) {
            esp_wifi_set_mode(current_mode);
        }
        return;
    }
    
    if (!async) {
        // Blocking scan - get results immediately
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        
        if (ap_count > 0) {
            _rawScanResults.resize(ap_count);
            esp_wifi_scan_get_ap_records(&ap_count, _rawScanResults.data());
            WM_LOGI("✅ Found %d WiFi networks", ap_count);
            
            // Filter and sort results
            filterScanResults();
        } else {
            _rawScanResults.clear();
            WM_LOGW("⚠️  No WiFi networks found");
        }
        
        _lastScanTime = esp_timer_get_time();
        _scanInProgress = false;
        
        // Restore original mode if we changed it
        if (mode_changed) {
            WM_LOGI("🔄 Restoring AP mode after scan...");
            esp_wifi_set_mode(current_mode);
        }
    }
    // For async scans, results will be processed in the WiFi event handler
}

bool WiFiManager::isDuplicateSSID(const char* ssid, const std::vector<wifi_ap_record_t>& results) {
    for (const auto& ap : results) {
        if (strcmp((char*)ap.ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

int WiFiManager::calculateSignalQuality(int rssi) {
    // Convert RSSI to percentage (0-100%)
    // RSSI ranges typically from -100 (weak) to -30 (strong)
    int quality = 2 * (rssi + 100);
    if (quality > 100) quality = 100;
    if (quality < 0) quality = 0;
    return quality;
}

void WiFiManager::filterScanResults() {
    if (_rawScanResults.empty()) {
        return;
    }
    
    std::vector<wifi_ap_record_t> filtered;
    
    for (const auto& ap : _rawScanResults) {
        // Skip networks with empty SSID
        if (strlen((char*)ap.ssid) == 0) {
            continue;
        }
        
        // Apply minimum quality filter
        if (_minimumQuality > 0) {
            int quality = calculateSignalQuality(ap.rssi);
            if (quality < _minimumQuality) {
                WM_LOGV("Filtering out %s (quality: %d < %d)", 
                       (char*)ap.ssid, quality, _minimumQuality);
                continue;
            }
        }
        
        // Check for duplicates if removal is enabled
        if (_removeDuplicateAPs) {
            if (isDuplicateSSID((char*)ap.ssid, filtered)) {
                // Keep the one with stronger signal
                for (auto& existing : filtered) {
                    if (strcmp((char*)existing.ssid, (char*)ap.ssid) == 0) {
                        if (ap.rssi > existing.rssi) {
                            WM_LOGV("Replacing duplicate %s (RSSI: %d -> %d)", 
                                   (char*)ap.ssid, existing.rssi, ap.rssi);
                            existing = ap;
                        }
                        break;
                    }
                }
                continue;
            }
        }
        
        filtered.push_back(ap);
    }
    
    // Sort by signal strength (strongest first)
    std::sort(filtered.begin(), filtered.end(), 
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  return a.rssi > b.rssi;
              });
    
    // Update the raw scan results with filtered data
    _rawScanResults = std::move(filtered);
    
    WM_LOGI("Filtered scan results: %d networks", _rawScanResults.size());
    
    // Log the filtered results
    for (size_t i = 0; i < _rawScanResults.size() && i < 10; i++) {
        const auto& ap = _rawScanResults[i];
        int quality = calculateSignalQuality(ap.rssi);
        WM_LOGD("  %d: %s (RSSI: %d, Quality: %d%%, Ch: %d, Auth: %d)", 
               (int)i, (char*)ap.ssid, ap.rssi, quality, 
               ap.primary, ap.authmode);
        (void)quality; // Suppress unused variable warning when logging is disabled
    }
}

void WiFiManager::sortScanResultsBySignal() {
    std::sort(_rawScanResults.begin(), _rawScanResults.end(), 
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  return a.rssi > b.rssi;
              });
}

std::vector<wifi_ap_record_t> WiFiManager::getFilteredScanResults() {
    return _rawScanResults;
}

bool WiFiManager::scanWiFiNetworks() {
    performWiFiScan(false); // Blocking scan
    return !_rawScanResults.empty();
}



// HTTP Server Handler Implementations

// External binary data (embedded assets)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t wifi_html_start[] asm("_binary_wifi_html_start"); 
extern const uint8_t wifi_html_end[] asm("_binary_wifi_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t wm_js_start[] asm("_binary_wm_js_start");
extern const uint8_t wm_js_end[] asm("_binary_wm_js_end");

WiFiManager* WiFiManager::getManagerFromRequest(httpd_req_t *req) {
    return static_cast<WiFiManager*>(req->user_ctx);
}

esp_err_t WiFiManager::handleRoot(httpd_req_t *req) {
    WM_LOGD("Serving root page");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    // Serve embedded HTML
    const size_t index_html_len = index_html_end - index_html_start;
    httpd_resp_send(req, (const char*)index_html_start, index_html_len);
    
    return ESP_OK;
}

esp_err_t WiFiManager::handleScan(httpd_req_t *req) {
    WM_LOGD("WiFi scan requested");
    WiFiManager* manager = getManagerFromRequest(req);
    
    // Use enhanced scanning functionality
    manager->performWiFiScan(false); // Blocking scan with filtering
    
    // Get filtered scan results
    std::vector<wifi_ap_record_t> scan_results = manager->getFilteredScanResults();
    
    // Create JSON response
    cJSON* networks_json = cJSON_CreateArray();
    if (networks_json) {
        for (const auto& ap : scan_results) {
            cJSON* network = cJSON_CreateObject();
            if (network) {
                cJSON_AddStringToObject(network, "ssid", (char*)ap.ssid);
                cJSON_AddNumberToObject(network, "rssi", ap.rssi);
                cJSON_AddNumberToObject(network, "channel", ap.primary);
                cJSON_AddNumberToObject(network, "encryption", ap.authmode);
                cJSON_AddBoolToObject(network, "hidden", false);
                
                // Calculate signal quality percentage
                int quality = manager->calculateSignalQuality(ap.rssi);
                cJSON_AddNumberToObject(network, "quality", quality);
                
                // Add security type string for display
                const char* security = "Open";
                switch (ap.authmode) {
                    case WIFI_AUTH_WEP: security = "WEP"; break;
                    case WIFI_AUTH_WPA_PSK: security = "WPA"; break;
                    case WIFI_AUTH_WPA2_PSK: security = "WPA2"; break;
                    case WIFI_AUTH_WPA_WPA2_PSK: security = "WPA/WPA2"; break;
                    case WIFI_AUTH_WPA3_PSK: security = "WPA3"; break;
                    case WIFI_AUTH_WPA2_WPA3_PSK: security = "WPA2/WPA3"; break;
                    default: security = "Unknown"; break;
                }
                cJSON_AddStringToObject(network, "security", security);
                
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
        
        WM_LOGI("Sent scan results: %d networks", scan_results.size());
    } else {
        WM_LOGE("Failed to create JSON response");
        httpd_resp_send_500(req);
    }
    
    cJSON_Delete(networks_json);
    return ESP_OK;
}

esp_err_t WiFiManager::handleWifiSave(httpd_req_t *req) {
    WM_LOGD("WiFi save requested");
    WiFiManager* manager = getManagerFromRequest(req);
    
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
    
    // Parse custom parameters
    for (const auto& param : manager->_params) {
        if (!param || !param->getID()) continue;
        
        // Look for parameter in form data
        std::string param_key = std::string(param->getID()) + "=";
        char* param_start = strstr(buf, param_key.c_str());
        if (param_start) {
            param_start += param_key.length();
            char* param_end = strchr(param_start, '&');
            int len = param_end ? (param_end - param_start) : strlen(param_start);
            
            if (len > 0 && len < 256) {
                char param_value[256] = {0};
                strncpy(param_value, param_start, len);
                
                // URL decode parameter value
                for (int i = 0, j = 0; i <= len; i++, j++) {
                    if (param_value[i] == '+') {
                        param_value[j] = ' ';
                    } else if (param_value[i] == '%' && i + 2 <= len) {
                        char hex[3] = {param_value[i+1], param_value[i+2], 0};
                        param_value[j] = (char)strtol(hex, NULL, 16);
                        i += 2;
                    } else {
                        param_value[j] = param_value[i];
                    }
                }
                
                param->setValue(param_value);
                WM_LOGD("Updated parameter %s = %s", param->getID(), param_value);
            }
        }
    }
    
    if (ssid_param) {
        ssid_param += 2; // Skip "s="
        char* end = strchr(ssid_param, '&');
        int len = end ? (end - ssid_param) : strlen(ssid_param);
        len = (len < sizeof(ssid) - 1) ? len : sizeof(ssid) - 1;
        strncpy(ssid, ssid_param, len);
        ssid[len] = '\0';
        
        // URL decode (basic)
        for (int i = 0, j = 0; i <= len; i++, j++) {
            if (ssid[i] == '+') {
                ssid[j] = ' ';
            } else if (ssid[i] == '%' && i + 2 <= len) {
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
        password[len] = '\0';
        
        // URL decode (basic)
        for (int i = 0, j = 0; i <= len; i++, j++) {
            if (password[i] == '+') {
                password[j] = ' ';
            } else if (password[i] == '%' && i + 2 <= len) {
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
    
    // Switch to AP+STA mode to allow STA configuration
    WM_LOGI("🔄 Switching to AP+STA mode for connection...");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        WM_LOGE("❌ Failed to set AP+STA mode: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Give it a moment to switch modes
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Configure WiFi with new credentials
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (strlen(password) > 0) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    
    WM_LOGI("🔧 Setting STA configuration...");
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        WM_LOGE("❌ Failed to set WiFi config: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Attempt connection
    WM_LOGI("🌐 Attempting to connect to WiFi...");
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        WM_LOGE("❌ Failed to start WiFi connection: %s", esp_err_to_name(err));
        // Don't return error here, connection might still succeed
    }
    
    // Update manager state
    manager->_state = WM_STATE_TRY_STA;
    manager->_connectStart = esp_timer_get_time();
    
    // Send success response and ensure it's completely transmitted
    httpd_resp_set_type(req, "text/html");
    const char* success_msg = 
        "<html><body>"
        "<h1>Connecting...</h1>"
        "<p>Device is attempting to connect to the network.</p>"
        "<p>Please wait and check your device's connection status.</p>"
        "<script>setTimeout(function(){window.location.href='/';}, 5000);</script>"
        "</body></html>";
    
    WM_LOGI("📤 Sending HTTP response...");
    esp_err_t send_ret = httpd_resp_send(req, success_msg, strlen(success_msg));
    if (send_ret != ESP_OK) {
        WM_LOGE("❌ Failed to send HTTP response: %s", esp_err_to_name(send_ret));
        return send_ret;
    }
    
    // CRITICAL: Ensure the response is actually transmitted over the network
    WM_LOGI("⏳ Waiting for HTTP response transmission to complete...");
    
    // Give TCP stack time to actually send the data over WiFi
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second should be enough for small response
    
    WM_LOGI("✅ HTTP response transmission complete");
    return ESP_OK;
}

esp_err_t WiFiManager::handleInfo(httpd_req_t *req) {
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
        "<tr><td>WiFi</td><td>Yes</td></tr>"
        "<tr><td>Bluetooth</td><td>%s</td></tr>"
        "<tr><td>Free Heap</td><td>%lu bytes</td></tr>"
        "<tr><td>WiFiManager Version</td><td>%s</td></tr>"
        "</table>"
        "<p><a href='/'>Back to WiFi Manager</a></p>"
        "</body></html>",
        CONFIG_IDF_TARGET,
        chip_info.cores,
        chip_info.revision / 100,
        chip_info.revision % 100,
        (chip_info.features & CHIP_FEATURE_BT) ? "Yes" : "No",
        esp_get_free_heap_size(),
        WM_VERSION
    );
    
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

esp_err_t WiFiManager::handleExit(httpd_req_t *req) {
    WM_LOGD("Exit requested");
    WiFiManager* manager = getManagerFromRequest(req);
    
    httpd_resp_set_type(req, "text/html");
    const char* exit_msg = 
        "<html><body>"
        "<h1>Exiting WiFi Manager</h1>"
        "<p>Configuration portal is closing.</p>"
        "</body></html>";
    httpd_resp_send(req, exit_msg, strlen(exit_msg));
    
    // Signal to close portal
    manager->_state = WM_STATE_PORTAL_ABORT;
    manager->_portalAbortResult = true;
    
    return ESP_OK;
}

esp_err_t WiFiManager::handleCaptivePortal(httpd_req_t *req) {
    WM_LOGD("Captive portal detection request: %s", req->uri);
    
    // Different responses based on the requesting OS
    if (strstr(req->uri, "generate_204")) {
        // Android captive portal check
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    } else if (strstr(req->uri, "ncsi.txt")) {
        // Windows captive portal check
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Microsoft NCSI", -1);
    } else {
        // iOS/macOS and others - redirect to main page
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    }
    
    return ESP_OK;
} 