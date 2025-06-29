#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

// Define the GPIO pins for the laser modules
#define LASER_PIN_1 2
#define LASER_PIN_2 3

// Define ADC input for photodiodes 1 & 2 : (GPIO 26 = ADC0 / GPIO 27 = ADC 1)
#define PHOTODIODE_1_ADC_INPUT 0
#define PHOTODIODE_1_GPIO 26
#define PHOTODIODE_2_ADC_INPUT 1
#define PHOTODIODE_2_GPIO 27

// WiFi credentials - replace with your network details
#define WIFI_SSID "INALAN_2.4G_EShG4j"
#define WIFI_PASSWORD "eR9EyzA4"

// Server details - replace with your PHP server details
#define SERVER_IP "192.168.1.9"  // Your server IP
#define SERVER_PORT 8000
#define SERVER_PATH "/receive_data.php"  // path to your PHP script

// Structure to hold sensor data
typedef struct {
    uint16_t sensor1_raw;
    float sensor1_voltage;
    uint16_t sensor2_raw;
    float sensor2_voltage;
    uint32_t timestamp;
} sensor_data_t;

// TCP connection state
typedef struct {
    struct tcp_pcb *tcp_pcb;
    bool connected;
    bool data_sent;
    bool response_received;
    char *request_data;
    int request_len;
    char response_buffer[1024];
    int response_len;
} tcp_client_t;

static tcp_client_t tcp_client;

// TCP connection callback
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    if (err != ERR_OK) {
        printf("TCP connection failed: %d\n", err);
        return err;
    }
    
    printf("TCP connected to server\n");
    client->connected = true;
    
    // Send HTTP POST request
    err_t write_err = tcp_write(tpcb, client->request_data, client->request_len, TCP_WRITE_FLAG_COPY);
    if (write_err == ERR_OK) {
        tcp_output(tpcb);
        client->data_sent = true;
        printf("HTTP request sent\n");
    } else {
        printf("Failed to send HTTP request: %d\n", write_err);
    }
    
    return ERR_OK;
}

// TCP receive callback
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    
    if (p == NULL) {
        // Connection closed by server - this is normal for HTTP
        printf("Server closed connection (normal HTTP behavior)\n");
        client->response_received = true;
        return ERR_OK;
    }
    
    if (err == ERR_OK) {
        // Copy response data to buffer
        if (client->response_len + p->len < sizeof(client->response_buffer) - 1) {
            memcpy(client->response_buffer + client->response_len, p->payload, p->len);
            client->response_len += p->len;
            client->response_buffer[client->response_len] = '\0';
        }
        
        // Print server response (just the important part)
        printf("Server response received (%d bytes)\n", p->len);
        
        // Look for the JSON response in the HTTP response
        char *json_start = strstr(client->response_buffer, "{");
        if (json_start) {
            printf("JSON Response: %s\n", json_start);
        }
        
        // Acknowledge received data
        tcp_recved(tpcb, p->len);
    }
    
    pbuf_free(p);
    return ERR_OK;
}

// TCP error callback
static void tcp_client_err(void *arg, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    // Error -13 is ERR_CONN_ABORTED - normal when server closes connection
    if (err == -13) {
        printf("Connection closed by server (normal)\n");
        client->response_received = true;
    } else {
        printf("TCP error: %d\n", err);
    }
    client->connected = false;
}

// Function to send HTTP POST request
bool send_http_post(const char* json_data) {
    // Reset client state
    memset(&tcp_client, 0, sizeof(tcp_client));
    
    // Create HTTP POST request
    static char http_request[1024];
    int content_length = strlen(json_data);
    
    snprintf(http_request, sizeof(http_request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "User-Agent: PicoW-LaserSensor/1.0\r\n"
        "Accept: application/json\r\n"
        "\r\n"
        "%s",
        SERVER_PATH, SERVER_IP, SERVER_PORT, content_length, json_data);
    
    tcp_client.request_data = http_request;
    tcp_client.request_len = strlen(http_request);
    
    printf("Sending HTTP request:\n%s\n", http_request);
    
    // Create TCP PCB
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        printf("Failed to create TCP PCB\n");
        return false;
    }
    
    tcp_client.tcp_pcb = pcb;
    
    // Set up callbacks
    tcp_arg(pcb, &tcp_client);
    tcp_err(pcb, tcp_client_err);
    tcp_recv(pcb, tcp_client_recv);
    
    // Parse server IP
    ip_addr_t server_addr;
    if (!ip4addr_aton(SERVER_IP, &server_addr)) {
        printf("Invalid server IP address: %s\n", SERVER_IP);
        tcp_close(pcb);
        return false;
    }
    
    printf("Connecting to %s:%d\n", SERVER_IP, SERVER_PORT);
    
    // Connect to server
    err_t err = tcp_connect(pcb, &server_addr, SERVER_PORT, tcp_client_connected);
    if (err != ERR_OK) {
        printf("Failed to initiate TCP connection: %d\n", err);
        tcp_close(pcb);
        return false;
    }
    
    // Wait for response with timeout
    int timeout = 0;
    while (!tcp_client.response_received && timeout < 100) {  // 10 second timeout
        sleep_ms(100);
        cyw43_arch_poll(); // Process network events
        timeout++;
    }
    
    // Close the connection if still open
    if (tcp_client.tcp_pcb) {
        tcp_close(tcp_client.tcp_pcb);
    }
    
    return tcp_client.response_received;
}

// Function to send sensor data to PHP server
bool send_data_to_server(sensor_data_t *data) {
    // Create JSON payload
    char json_payload[512];
    snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"sensor1_raw\":%d,"
        "\"sensor1_voltage\":%.3f,"
        "\"sensor2_raw\":%d,"
        "\"sensor2_voltage\":%.3f,"
        "\"timestamp\":%lu"
        "}",
        data->sensor1_raw, data->sensor1_voltage,
        data->sensor2_raw, data->sensor2_voltage,
        data->timestamp
    );

    printf("Sending data: %s\n", json_payload);
    return send_http_post(json_payload);
}

// Function to connect to WiFi
bool connect_to_wifi() {
    cyw43_arch_enable_sta_mode();
    
    printf("Connecting to WiFi: %s\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, 
                                           CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Failed to connect to WiFi\n");
        return false;
    }
    
    printf("Connected to WiFi successfully!\n");
    printf("IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    printf("Server: %s:%d%s\n", SERVER_IP, SERVER_PORT, SERVER_PATH);
    return true;
}

// Function to read sensor data
void read_sensors(sensor_data_t *data) {
    const float conversion_factor = 3.3f / (1 << 12);
    
    // Read from first ADC (GPIO 26)
    adc_select_input(0);
    data->sensor1_raw = adc_read();
    data->sensor1_voltage = data->sensor1_raw * conversion_factor;
    
    // Read from second ADC (GPIO 27)
    adc_select_input(1);
    data->sensor2_raw = adc_read();
    data->sensor2_voltage = data->sensor2_raw * conversion_factor;
    
    // Add timestamp (milliseconds since boot)
    data->timestamp = to_ms_since_boot(get_absolute_time());
    
    printf("Sensor 1 - Raw: %4d, Voltage: %.3f V  |||||  ", 
           data->sensor1_raw, data->sensor1_voltage);
    printf("Sensor 2 - Raw: %4d, Voltage: %.3f V\n", 
           data->sensor2_raw, data->sensor2_voltage);
}

int main() {
    // Initialize all of pico
    stdio_init_all();
    
    // Initialize the Wi-Fi chip first
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return -1;
    }
    
    // Turn on the Pico W LED to show we're alive
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    
    // Initialize ADC
    adc_init();
    adc_gpio_init(PHOTODIODE_1_GPIO);
    adc_gpio_init(PHOTODIODE_2_GPIO);
    
    // Initialize the laser pins as outputs
    gpio_init(LASER_PIN_1);
    gpio_set_dir(LASER_PIN_1, GPIO_OUT);
    gpio_init(LASER_PIN_2);
    gpio_set_dir(LASER_PIN_2, GPIO_OUT);
    
    // Start with both lasers off
    gpio_put(LASER_PIN_1, 0);
    gpio_put(LASER_PIN_2, 0);
    
    // Give it some time
    sleep_ms(5000);
    
    // Print startup message
    printf("Raspberry Pi Pico 2W Dual Laser Control with WiFi\n");
    printf("Target Server: %s:%d%s\n", SERVER_IP, SERVER_PORT, SERVER_PATH);
    sleep_ms(3000);
    
    // Connect to WiFi
    if (!connect_to_wifi()) {
        printf("Cannot continue without WiFi connection\n");
        return -1;
    }
    
    printf("Initializing Lasers...\n");
    sleep_ms(2000);
    
    printf("First Laser will turn on in\n3...\n");
    sleep_ms(1000);
    printf("2...\n");
    sleep_ms(1000);
    printf("1...\n");
    sleep_ms(1000);
    
    // Turn on the first Laser
    gpio_put(LASER_PIN_1, 1);
    printf("First Laser is ON\n");
    sleep_ms(2000);
    
    printf("Second Laser will turn on in\n3...\n");
    sleep_ms(1000);
    printf("2...\n");
    sleep_ms(1000);
    printf("1...\n");
    sleep_ms(1000);
    
    // Turn on the second laser
    gpio_put(LASER_PIN_2, 1);
    printf("Second Laser is ON\n");
    sleep_ms(2000);
    printf("Both Lasers Are Active\n");
    
    sleep_ms(5000);
    
    // Keep the program running and monitor photodiodes
    printf("Starting Photodiode Monitoring with Data Transmission...\n");
    
    sensor_data_t sensor_data;
    int transmission_counter = 0;
    
    while (1) {
        // Read sensor data
        read_sensors(&sensor_data);
        
        // Send data to server every 10 readings (every 5 seconds)
        transmission_counter++;
        if (transmission_counter >= 10) {
            printf("\n=== Transmitting data to server... ===\n");
            if (send_data_to_server(&sensor_data)) {
                printf("=== Data transmitted successfully! ===\n\n");
            } else {
                printf("=== Failed to transmit data! ===\n\n");
            }
            transmission_counter = 0;
        }
        
        sleep_ms(500);
        cyw43_arch_poll(); // Keep WiFi alive
    }
    
    // Cleanup (this will never be reached in the current loop)
    cyw43_arch_deinit();
    return 0;
}