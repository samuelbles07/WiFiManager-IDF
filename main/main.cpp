/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "WiFiManager.h"
#include "WiFiManagerParameter.h"

static const char* TAG = "main";

// Global WiFiManager instance
WiFiManager wifiManager;

// Custom parameter example
WiFiManagerParameter customField("server", "api server", "api.example.com", 40);
WiFiManagerParameter customPort("port", "port", "80", 6);

// Callback functions
void configModeCallback() {
    ESP_LOGI(TAG, "Entered config mode");
}

void saveConfigCallback() {
    ESP_LOGI(TAG, "Should save config");
    ESP_LOGI(TAG, "Custom server: %s", customField.getValue());
    ESP_LOGI(TAG, "Custom port: %s", customPort.getValue());
}

void apCallback(WiFiManager* myWiFiManager) {
    ESP_LOGI(TAG, "Entered AP mode");
    ESP_LOGI(TAG, "AP IP address: 192.168.4.1");
    ESP_LOGI(TAG, "Go to http://192.168.4.1 in a web browser");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "🚀 WiFiManager ESP-IDF Example Starting");
    
    // Give the system a moment to fully initialize
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Print chip information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "📱 This is %s chip with %d CPU core(s), WiFi%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(TAG, "🔧 Silicon revision v%d.%d", major_rev, minor_rev);

    // Configure WiFiManager
    ESP_LOGI(TAG, "⚙️  Configuring WiFiManager...");
    
    // Set timeouts
    ESP_LOGI(TAG, "⏱️  Setting timeouts...");
    wifiManager.setConfigPortalTimeout(180); // 3 minutes
    wifiManager.setConnectTimeout(30);       // 30 seconds
    
    // Set callbacks
    ESP_LOGI(TAG, "📞 Setting callbacks...");
    wifiManager.setConfigModeCallback(configModeCallback);
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setAPCallback(apCallback);
    
    // Add custom parameters
    ESP_LOGI(TAG, "🔧 Adding custom parameters...");
    wifiManager.addParameter(&customField);
    wifiManager.addParameter(&customPort);
    
    // Set minimum signal quality
    ESP_LOGI(TAG, "📶 Setting signal quality...");
    wifiManager.setMinimumSignalQuality(8);
    
    // Remove duplicate APs (lower quality duplicates)
    ESP_LOGI(TAG, "🧹 Enabling duplicate AP removal...");
    wifiManager.setRemoveDuplicateAPs(true);
    
    ESP_LOGI(TAG, "🌐 Starting autoConnect...");
    
    // Try to connect to saved WiFi network or start captive portal
    if (wifiManager.autoConnect("ESP-WiFiManager")) {
        ESP_LOGI(TAG, "✅ Connected to WiFi!");
        
        // Connected, continue with normal operation
        ESP_LOGI(TAG, "📡 WiFi SSID: %s", wifiManager.getSSID().c_str());
        ESP_LOGI(TAG, "🌍 Local IP: [would show IP here]");
        
        // Print custom parameter values
        ESP_LOGI(TAG, "🔧 Custom parameters:");
        ESP_LOGI(TAG, "  📊 Server: %s", customField.getValue());
        ESP_LOGI(TAG, "  🔌 Port: %s", customPort.getValue());
        
        // NOTE: Custom parameters are NOT automatically saved to NVS
        // You need to implement your own storage if you want persistence
        // Example: Use NVS, SPIFFS, or other storage method to save customField.getValue(), etc.
        
        // Optional: Stop captive portal servers manually when no longer needed
        // Uncomment these lines if you want to stop the servers after successful connection:
        // ESP_LOGI(TAG, "🛑 Stopping captive portal servers...");
        // wifiManager.stopServers();
        
        // Main application loop
        ESP_LOGI(TAG, "🏃 Starting main application loop...");
        
        while (1) {
            ESP_LOGI(TAG, "💚 Running main application...");
            vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds
        }
        
    } else {
        ESP_LOGE(TAG, "❌ Failed to connect or user cancelled config portal");
        
        // Could restart or enter deep sleep here
        ESP_LOGI(TAG, "🔄 Restarting in 3 seconds...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
}

// Alternative non-blocking example (commented out)
/*
void non_blocking_example() {
    ESP_LOGI(TAG, "Non-blocking WiFiManager example");
    
    // Set non-blocking mode
    wifiManager.setConfigPortalBlocking(false);
    
    // Start the connection process
    wifiManager.autoConnect("ESP-WiFiManager");
    
    // Main loop with periodic processing
    while (true) {
        // Process WiFiManager state machine
        if (!wifiManager.process()) {
            // WiFiManager finished (connected or failed)
            if (wifiManager.getState() == WM_STATE_RUN_STA) {
                ESP_LOGI(TAG, "Connected to WiFi in non-blocking mode!");
                break;
            } else {
                ESP_LOGE(TAG, "WiFiManager failed in non-blocking mode");
                esp_restart();
            }
        }
        
        // Do other work here while WiFiManager is processing
        ESP_LOGI(TAG, "Doing other work... WiFiManager state: %d", wifiManager.getState());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Continue with main application
    while (1) {
        ESP_LOGI(TAG, "Main application running...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
*/
