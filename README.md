# IDF WiFiManager

A comprehensive ESP-IDF 5.x C++ component that provides WiFi configuration management with a captive portal, inspired by the Arduino WiFiManager library.

## Overview

This project demonstrates a complete WiFiManager component for ESP-IDF that includes:

- **Captive Portal**: Automatic WiFi configuration when no saved credentials exist
- **DNS Server**: Captive portal DNS server that redirects all queries to the configuration portal
- **HTTP Server**: Web interface for WiFi configuration
- **Credential Storage**: Persistent storage in NVS
- **WiFi Scanning**: Automatic network discovery
- **Static IP Configuration**: Support for both AP and STA static IP
- **Event-driven Architecture**: Non-blocking operation with callbacks
- **Custom Parameters**: Support for additional configuration parameters

## Project Structure

```
IDFWifiManager/
├── CMakeLists.txt                 # Project CMakeLists
├── main/
│   ├── CMakeLists.txt            # Main component CMakeLists
│   └── main.cpp                  # Example application
├── components/
│   └── wifimanager/              # WiFiManager component
│       ├── CMakeLists.txt        # Component CMakeLists
│       ├── include/              # Header files
│       │   ├── WiFiManager.h
│       │   ├── WiFiManagerParameter.h
│       │   └── wm_config.h
│       ├── src/                  # Source files
│       │   ├── WiFiManager.cpp
│       │   ├── WiFiManagerParameter.cpp
│       │   ├── wm_dns.cpp        # DNS server
│       │   ├── wm_http.cpp       # HTTP server
│       │   ├── wm_scan.cpp       # WiFi scanning
│       │   ├── wm_storage.cpp    # NVS storage
│       │   └── wm_state.cpp      # State machine
│       ├── assets/               # Web assets
│       │   ├── index.html
│       │   ├── wifi.html
│       │   ├── style.css
│       │   ├── wm.js
│       │   └── favicon.ico
│       └── README.md             # Component documentation
├── sdkconfig                     # ESP-IDF configuration
└── README.md                     # This file
```

## Quick Start

### 1. Build the Project

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build the project
idf.py build

# Flash to device
idf.py flash monitor
```

### 2. Basic Usage

The example application demonstrates basic WiFiManager usage:

```cpp
#include "WiFiManager.h"

WiFiManager wifiManager;

// Configure callbacks
wifiManager.setAPCallback([](WiFiManager* wm) {
    printf("AP started: %s\n", wm->getConfigPortalSSID());
});

wifiManager.setSaveConfigCallback([]() {
    printf("WiFi credentials saved\n");
});

// Start auto-connect
if (wifiManager.autoConnect()) {
    printf("WiFi connected! IP: %s\n", wifiManager.getLocalIPString().c_str());
} else {
    printf("Failed to connect\n");
}
```

### 3. Configuration Portal

When the device starts without saved WiFi credentials:

1. The device creates an AP named "ESP-XXXXXX" (or custom name)
2. Connect to this AP from your phone/computer
3. Navigate to `http://192.168.4.1` (or any URL - DNS redirects all queries)
4. Select your WiFi network and enter password
5. The device will connect to your network and save credentials

## Features

### Core Functionality

- **Auto-Connect**: Automatically connects to saved WiFi credentials
- **Captive Portal**: Web-based configuration when no credentials exist
- **Credential Persistence**: Stores WiFi credentials in NVS
- **DNS Server**: Redirects all DNS queries to the configuration portal
- **HTTP Server**: Serves configuration pages and handles form submissions

### Advanced Features

- **WiFi Scanning**: Discovers available networks
- **Signal Quality Filtering**: Filters networks by signal strength
- **Duplicate Removal**: Removes duplicate networks from scan results
- **Static IP Configuration**: Support for both AP and STA static IP
- **Custom Parameters**: Add custom configuration fields
- **Event Callbacks**: AP start and save configuration callbacks
- **Debug Output**: Configurable logging

### Web Interface

The configuration portal provides:

- **Main Page**: Overview and navigation menu
- **WiFi Configuration**: Form to select network and enter password
- **Network Scanning**: Automatic discovery of available networks
- **Device Information**: System information display
- **Reset Functionality**: Reset device settings

### Captive Portal Support

Includes support for various captive portal detection methods:

- Android: `/generate_204`
- Windows: `/fwlink`
- iOS: `/hotspot-detect.html`
- Network connectivity tests: `/ncsi.txt`, `/connecttest.txt`

## API Reference

### WiFiManager Class

#### Connection Methods

```cpp
bool autoConnect();
bool autoConnect(const char* apName);
bool autoConnect(const char* apName, const char* apPassword);
bool startConfigPortal();
bool startConfigPortal(const char* apName);
bool startConfigPortal(const char* apName, const char* apPassword);
```

#### Configuration Methods

```cpp
void setConfigPortalTimeout(uint32_t seconds);
void setConnectTimeout(uint32_t seconds);
void setBreakAfterConfig(bool breakAfterConfig);
void setEnableConfigPortal(bool enable);
void setDebugOutput(bool debug);
```

#### Callback Methods

```cpp
void setAPCallback(wm_ap_callback_t func);
void setSaveConfigCallback(wm_save_config_callback_t func);
```

#### Custom Parameters

```cpp
void addParameter(WiFiManagerParameter* p);
void setCustomHeadElement(const char* html);
void setClass(const char* className);
```

#### WiFi Configuration

```cpp
void setMinimumSignalQuality(int percent);
void setRemoveDuplicateAPs(bool removeDuplicates);
void setScanDispPerc(bool displayPercent);
```

#### IP Configuration

```cpp
void setAPStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn);
void setSTAStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn, IPAddress dns);
```

#### Getter Methods

```cpp
const char* getConfigPortalSSID();
const char* getConfigPortalPassword();
const char* getSSID();
const char* getPassword();
IPAddress getLocalIP();
String getLocalIPString();
```

### WiFiManagerParameter Class

```cpp
WiFiManagerParameter(const char* id, const char* placeholder, const char* defaultValue, int length, const char* customHTML = nullptr, int type = WFM_LABEL);

const char* getID();
const char* getValue();
const char* getPlaceholder();
void setValue(const char* value);
String getHTML();
```

## Examples

### Basic Auto-Connect

```cpp
WiFiManager wifiManager;
if (wifiManager.autoConnect("MyDevice")) {
    printf("Connected to WiFi\n");
}
```

### Custom Parameters

```cpp
WiFiManager wifiManager;

// Add custom parameters
WiFiManagerParameter mqtt_server("mqtt", "MQTT Server", "192.168.1.100", 16);
WiFiManagerParameter mqtt_port("mqtt_port", "MQTT Port", "1883", 6);
wifiManager.addParameter(&mqtt_server);
wifiManager.addParameter(&mqtt_port);

if (wifiManager.autoConnect()) {
    printf("MQTT Server: %s\n", mqtt_server.getValue());
    printf("MQTT Port: %s\n", mqtt_port.getValue());
}
```

### Static IP Configuration

```cpp
WiFiManager wifiManager;

// Configure AP static IP
wifiManager.setAPStaticIPConfig(
    IPAddress(10, 0, 1, 1),    // IP
    IPAddress(10, 0, 1, 1),    // Gateway
    IPAddress(255, 255, 255, 0) // Subnet
);

wifiManager.autoConnect();
```

### Non-blocking Operation

```cpp
WiFiManager wifiManager;

// Start in non-blocking mode
wifiManager.startConfigPortal();

// Main loop
while (true) {
    if (wifiManager.process()) {
        printf("WiFi connected!\n");
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

## Configuration

### Default Settings

- **AP SSID**: "ESP-XXXXXX"
- **AP Password**: None (open network)
- **AP IP**: 192.168.4.1
- **Portal Timeout**: 180 seconds
- **Connect Timeout**: 30 seconds
- **Max Scan Results**: 32
- **Remove Duplicates**: true
- **Minimum Signal Quality**: 0%

### NVS Storage

WiFiManager stores configuration in NVS with the following keys:

- `wm:ssid` - WiFi SSID
- `wm:pass` - WiFi password
- `wm:sta_ip` - STA static IP
- `wm:sta_gw` - STA gateway
- `wm:sta_sn` - STA subnet mask
- `wm:sta_dns` - STA DNS server
- `wm:ap_ip` - AP static IP
- `wm:ap_gw` - AP gateway
- `wm:ap_sn` - AP subnet mask

## Building and Flashing

### Prerequisites

- ESP-IDF 5.x
- ESP32 development board
- USB cable for flashing

### Build Commands

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Configure project (optional)
idf.py menuconfig

# Build project
idf.py build

# Flash to device
idf.py flash

# Monitor serial output
idf.py monitor

# Build, flash, and monitor in one command
idf.py build flash monitor
```

### Configuration Options

You can configure various options using `idf.py menuconfig`:

- **WiFi Configuration**: SSID, password, static IP settings
- **Portal Settings**: Timeout values, custom HTML
- **Debug Options**: Log levels, debug output
- **Network Settings**: DNS, DHCP options

## Troubleshooting

### Common Issues

1. **Portal not accessible**: Ensure device is in AP mode and connected to the ESP32 network
2. **Credentials not saved**: Check NVS initialization and storage permissions
3. **DNS not working**: Verify DNS server is running on port 53
4. **Scan not working**: Ensure WiFi is initialized and scanning is enabled

### Debug Output

Enable debug output to see detailed logs:

```cpp
wifiManager.setDebugOutput(true);
```

### Reset Settings

To reset all stored settings:

```cpp
wifiManager.resetSettings();
```

### Serial Monitor

Use the serial monitor to see debug output:

```bash
idf.py monitor
```

## Contributing

Contributions are welcome! Please ensure:

1. Code follows ESP-IDF coding standards
2. All new features include appropriate documentation
3. Tests are added for new functionality
4. Commit messages are clear and descriptive

## License

This project is provided as-is for educational and development purposes. Please refer to the ESP-IDF license for the underlying framework.

## Acknowledgments

This component is inspired by the Arduino WiFiManager library by tzapu, adapted for ESP-IDF 5.x with modern C++ features and ESP-IDF-specific optimizations.
