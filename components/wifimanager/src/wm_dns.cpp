#include "wm_config.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

// DNS server implementation for captive portal

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
#define DNS_FLAG_OPCODE     0x7800
#define DNS_FLAG_AA         0x0400  // Authoritative Answer
#define DNS_FLAG_TC         0x0200  // Truncated
#define DNS_FLAG_RD         0x0100  // Recursion Desired
#define DNS_FLAG_RA         0x0080  // Recursion Available
#define DNS_FLAG_RCODE      0x000F  // Response Code

// DNS record types
#define DNS_TYPE_A      1   // A record (IPv4 address)
#define DNS_TYPE_AAAA   28  // AAAA record (IPv6 address)

// DNS classes
#define DNS_CLASS_IN    1   // Internet class

static TaskHandle_t s_dns_task_handle = nullptr;
static int s_dns_socket = -1;
static ip4_addr_t s_ap_ip;
static bool s_dns_running = false;

/**
 * Parse DNS name from packet
 */
static int parse_dns_name(const uint8_t* packet, int packet_len, int offset, char* name, int name_len) {
    int pos = offset;
    int name_pos = 0;
    bool jumped = false;
    int jump_count = 0;
    
    while (pos < packet_len && jump_count < 10) {
        uint8_t len = packet[pos];
        
        // Check for compression (pointer)
        if ((len & 0xC0) == 0xC0) {
            if (!jumped) {
                offset = pos + 2; // Return position after pointer
            }
            pos = ((len & 0x3F) << 8) | packet[pos + 1];
            jumped = true;
            jump_count++;
            continue;
        }
        
        // End of name
        if (len == 0) {
            if (name_pos > 0 && name_pos < name_len - 1) {
                name[name_pos - 1] = '\0'; // Replace last dot with null terminator
            } else {
                name[0] = '\0';
            }
            return jumped ? offset : pos + 1;
        }
        
        pos++;
        
        // Copy label
        for (int i = 0; i < len && pos < packet_len && name_pos < name_len - 2; i++, pos++) {
            name[name_pos++] = packet[pos];
        }
        
        // Add dot separator
        if (name_pos < name_len - 1) {
            name[name_pos++] = '.';
        }
    }
    
    name[0] = '\0';
    return -1;
}

/**
 * Create DNS response packet
 */
static int create_dns_response(const uint8_t* query, int query_len, uint8_t* response, int response_max_len) {
    if (query_len < sizeof(dns_header_t)) {
        return -1;
    }
    
    const dns_header_t* query_header = (const dns_header_t*)query;
    dns_header_t* response_header = (dns_header_t*)response;
    
    // Copy query to response
    if (query_len > response_max_len) {
        return -1;
    }
    memcpy(response, query, query_len);
    
    // Modify header for response
    response_header->flags = htons(ntohs(query_header->flags) | DNS_FLAG_RESPONSE | DNS_FLAG_AA);
    response_header->ancount = query_header->qdcount; // Same number of answers as questions
    response_header->nscount = 0;
    response_header->arcount = 0;
    
    int pos = query_len;
    int questions = ntohs(query_header->qdcount);
    
    // Process each question and add answers
    int question_pos = sizeof(dns_header_t);
    for (int i = 0; i < questions && pos < response_max_len - 16; i++) {
        // Skip question name
        char name[256];
        int name_end = parse_dns_name(query, query_len, question_pos, name, sizeof(name));
        if (name_end < 0) {
            continue;
        }
        
        // Check question type and class
        if (name_end + 4 > query_len) {
            continue;
        }
        
        uint16_t qtype = ntohs(*(uint16_t*)(query + name_end));
        uint16_t qclass = ntohs(*(uint16_t*)(query + name_end + 2));
        
        WM_LOGD("DNS query: %s, type: %d, class: %d", name, qtype, qclass);
        
        // Only respond to A records in Internet class
        if (qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN) {
            // Add answer
            // Name (compressed pointer to question)
            response[pos++] = 0xC0;
            response[pos++] = sizeof(dns_header_t);
            
            // Type (A record)
            *(uint16_t*)(response + pos) = htons(DNS_TYPE_A);
            pos += 2;
            
            // Class (Internet)
            *(uint16_t*)(response + pos) = htons(DNS_CLASS_IN);
            pos += 2;
            
            // TTL (60 seconds)
            *(uint32_t*)(response + pos) = htonl(60);
            pos += 4;
            
            // Data length (4 bytes for IPv4)
            *(uint16_t*)(response + pos) = htons(4);
            pos += 2;
            
            // IP address (AP IP)
            *(uint32_t*)(response + pos) = s_ap_ip.addr;
            pos += 4;
            
            WM_LOGD("DNS response: %s -> " IPSTR, name, IP2STR(&s_ap_ip));
        }
        
        question_pos = name_end + 4;
    }
    
    return pos;
}

/**
 * DNS server task
 */
static void dns_server_task(void* pvParameters) {
    struct sockaddr_in server_addr = {};
    struct sockaddr_in client_addr;
    uint8_t buffer[DNS_MAX_PACKET_SIZE];
    socklen_t client_len = sizeof(client_addr);
    int reuse = 1;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DNS_PORT);
    
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        WM_LOGE("Failed to create DNS socket");
        goto cleanup;
    }
    
    setsockopt(s_dns_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    if (bind(s_dns_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        WM_LOGE("Failed to bind DNS socket");
        goto cleanup;
    }
    
    WM_LOGI("DNS server started on port %d", DNS_PORT);
    
    while (s_dns_running) {
        int len = recvfrom(s_dns_socket, buffer, sizeof(buffer) - 1, 0,
                          (struct sockaddr*)&client_addr, &client_len);
        
        if (len > 0) {
            WM_LOGD("DNS query from " IPSTR ":%d, length: %d", 
                   IP2STR((ip4_addr_t*)&client_addr.sin_addr), 
                   ntohs(client_addr.sin_port), len);
            
            // Create response
            uint8_t response[DNS_MAX_PACKET_SIZE];
            int response_len = create_dns_response(buffer, len, response, sizeof(response));
            
            if (response_len > 0) {
                sendto(s_dns_socket, response, response_len, 0,
                       (struct sockaddr*)&client_addr, client_len);
                WM_LOGD("DNS response sent, length: %d", response_len);
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            WM_LOGE("DNS recvfrom error: %d", errno);
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent tight loop
    }
    
cleanup:
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    
    WM_LOGI("DNS server task ended");
    s_dns_task_handle = nullptr;
    vTaskDelete(nullptr);
}

/**
 * Start DNS server
 */
esp_err_t wm_dns_server_start(const char* ap_ip) {
    if (s_dns_running) {
        WM_LOGW("DNS server already running");
        return ESP_OK;
    }
    
    if (!ap_ip || !ip4addr_aton(ap_ip, &s_ap_ip)) {
        WM_LOGE("Invalid AP IP address: %s", ap_ip ? ap_ip : "null");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_dns_running = true;
    
    BaseType_t ret = xTaskCreate(dns_server_task, "dns_server", 
                                CONFIG_WM_DNS_STACK_SIZE, nullptr, 5, &s_dns_task_handle);
    
    if (ret != pdPASS) {
        WM_LOGE("Failed to create DNS server task");
        s_dns_running = false;
        return ESP_FAIL;
    }
    
    WM_LOGI("DNS server starting with AP IP: %s", ap_ip);
    return ESP_OK;
}

/**
 * Stop DNS server
 */
esp_err_t wm_dns_server_stop() {
    if (!s_dns_running) {
        return ESP_OK;
    }
    
    WM_LOGI("Stopping DNS server");
    s_dns_running = false;
    
    // Close socket to wake up task
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    
    // Wait for task to finish
    int timeout = 50; // 5 seconds
    while (s_dns_task_handle && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (s_dns_task_handle) {
        WM_LOGW("DNS task did not terminate gracefully, deleting");
        vTaskDelete(s_dns_task_handle);
        s_dns_task_handle = nullptr;
    }
    
    WM_LOGI("DNS server stopped");
    return ESP_OK;
}

// Legacy placeholder functions
void wm_dns_init() {
    WM_LOGD("DNS server initialized");
}

void wm_dns_deinit() {
    WM_LOGD("DNS server deinitialized");
} 