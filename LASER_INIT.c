#include <stdio.h>
#include <string.h>
#include <math.h>
#include "hardware/gpio.h"
#include "hardware/adc.h"
// Software AES implementation (no hardware dependency)
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

// Define the GPIO pins for the laser modules
#define LASER_PIN_1 2
#define LASER_PIN_2 3

// Define ADC input for photodiodes 1 & 2
#define PHOTODIODE_1_ADC_INPUT 0
#define PHOTODIODE_1_GPIO 26
#define PHOTODIODE_2_ADC_INPUT 1
#define PHOTODIODE_2_GPIO 27

// Calibration button (optional)
#define CALIBRATION_BUTTON_PIN 14

// WiFi credentials
#define WIFI_SSID "HOST"
#define WIFI_PASSWORD "abcd1111"

// Server details
#define SERVER_IP "192.168.76.164"
#define SERVER_PORT 8000
#define SERVER_PATH "/receive_data.php"

// Encryption settings
#define XTEA_KEY_SIZE 16
#define XTEA_BLOCK_SIZE 8

// Particle detection parameters
#define CALIBRATION_SAMPLES 200         // Number of samples for stable baseline
#define DETECTION_THRESHOLD_PERCENT 8   // 8% drop triggers particle detection
#define MIN_PARTICLE_DURATION_MS 3      // Minimum event duration (filter noise)
#define MAX_PARTICLE_DURATION_MS 100    // Maximum event duration (filter air bubbles)
#define SAMPLING_RATE_MS 2              // Sample every 2ms for good resolution
#define COUNTING_PERIOD_SEC 60          // Count particles for 60 seconds
#define TRANSMISSION_INTERVAL_SEC 30    // Send results every 30 seconds

// Simple XTEA encryption (lightweight and very secure)
#define XTEA_KEY_SIZE 16
#define XTEA_BLOCK_SIZE 8
#define XTEA_ROUNDS 32

typedef struct {
    uint32_t key[4];
    bool initialized;
} xtea_context_t;

static xtea_context_t crypto_ctx = {0};

// Structure to hold calibration data
typedef struct {
    float sensor1_baseline;
    float sensor2_baseline;
    bool calibrated;
    uint32_t calibration_timestamp;
    float sensor1_noise_level;    // Standard deviation during calibration
    float sensor2_noise_level;
} calibration_data_t;

// Structure to hold particle counting results
typedef struct {
    uint32_t sensor1_particle_count;
    uint32_t sensor2_particle_count;
    uint32_t counting_duration_sec;
    uint32_t start_timestamp;
    uint32_t end_timestamp;
    float sensor1_concentration_per_min;
    float sensor2_concentration_per_min;
    float sensor1_baseline;
    float sensor2_baseline;
    uint32_t sensor1_false_positives;  // Events too short/long
    uint32_t sensor2_false_positives;
    float avg_sensor1_voltage;         // Average during counting period
    float avg_sensor2_voltage;
    bool counting_active;
} particle_count_data_t;

// Event detection state for each sensor
typedef struct {
    bool in_event;
    uint32_t event_start_time;
    float event_min_voltage;
    float event_baseline;
    uint32_t valid_events;
    uint32_t false_positives;
    float voltage_sum;                 // For calculating average
    uint32_t voltage_samples;
} detection_state_t;

// Global variables
static calibration_data_t calibration = {0};
static particle_count_data_t count_data = {0};
static detection_state_t sensor1_state = {0};
static detection_state_t sensor2_state = {0};

// TCP connection state (keeping existing networking code)
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

// Debug function to show device ID multiple ways
void debug_device_id() {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    printf("\n");
    printf("================================\n");
    printf("DEVICE ID DEBUG INFO\n");
    printf("================================\n");
    printf("Raw bytes: ");
    for (int i = 0; i < 8; i++) {
        printf("0x%02X ", board_id.id[i]);
    }
    printf("\n");
    
    printf("Hex string: ");
    for (int i = 0; i < 8; i++) {
        printf("%02x", board_id.id[i]);
    }
    printf("\n");
    
    printf("For PHP config: $DEVICE_ID = \"");
    for (int i = 0; i < 8; i++) {
        printf("%02x", board_id.id[i]);
    }
    printf("\";\n");
    printf("================================\n");
    printf("\n");
}

// XTEA encryption and decryption functions
void xtea_encrypt_block(uint32_t* data, const uint32_t* key) {
    uint32_t v0 = data[0], v1 = data[1];
    uint32_t sum = 0;
    const uint32_t delta = 0x9E3779B9;
    
    for (int i = 0; i < XTEA_ROUNDS; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    
    data[0] = v0;
    data[1] = v1;
}

// Initialize XTEA encryption with device-specific key
void init_xtea_encryption() {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    // Generate 4 x 32-bit key from 8-byte board ID
    crypto_ctx.key[0] = (board_id.id[0] << 24) | (board_id.id[1] << 16) | (board_id.id[2] << 8) | board_id.id[3];
    crypto_ctx.key[1] = (board_id.id[4] << 24) | (board_id.id[5] << 16) | (board_id.id[6] << 8) | board_id.id[7];
    crypto_ctx.key[2] = crypto_ctx.key[0] ^ 0xAAAAAAAA; // Add some variety
    crypto_ctx.key[3] = crypto_ctx.key[1] ^ 0x55555555; // Add some variety
    
    crypto_ctx.initialized = true;
    
    printf("XTEA encryption initialized\n");
    debug_device_id(); // Show device ID in multiple formats
}

// Simple padding for XTEA (8-byte blocks)
void pad_data(uint8_t* data, size_t* len) {
    size_t padding = XTEA_BLOCK_SIZE - (*len % XTEA_BLOCK_SIZE);
    if (padding == XTEA_BLOCK_SIZE) padding = 0;
    
    for (size_t i = 0; i < padding; i++) {
        data[*len + i] = (uint8_t)padding;
    }
    *len += padding;
}

// Remove padding from decrypted data
size_t unpad_data(uint8_t* data, size_t len) {
    if (len == 0) return 0;
    uint8_t padding = data[len - 1];
    if (padding > XTEA_BLOCK_SIZE || padding == 0) return len;
    
    // Verify padding
    for (size_t i = len - padding; i < len; i++) {
        if (data[i] != padding) return len;
    }
    
    return len - padding;
}

// Encrypt JSON data using XTEA
bool encrypt_json_data(const char* json_data, uint8_t* encrypted_buffer, size_t* encrypted_len) {
    if (!crypto_ctx.initialized) {
        printf("Error: XTEA not initialized\n");
        return false;
    }
    
    size_t json_len = strlen(json_data);
    if (json_len > 2000) { // Reasonable limit
        printf("Error: JSON data too large for encryption\n");
        return false;
    }
    
    // Copy data to temporary buffer for padding
    uint8_t padded_data[2048];
    memcpy(padded_data, json_data, json_len);
    size_t padded_len = json_len;
    
    // Add padding to make data block-aligned (8 bytes for XTEA)
    pad_data(padded_data, &padded_len);
    
    // Encrypt using XTEA in simple ECB mode
    for (size_t i = 0; i < padded_len; i += XTEA_BLOCK_SIZE) {
        uint32_t block[2];
        
        // Convert bytes to 32-bit words (little endian)
        block[0] = (padded_data[i + 3] << 24) | (padded_data[i + 2] << 16) | 
                   (padded_data[i + 1] << 8) | padded_data[i];
        block[1] = (padded_data[i + 7] << 24) | (padded_data[i + 6] << 16) | 
                   (padded_data[i + 5] << 8) | padded_data[i + 4];
        
        // Encrypt block
        xtea_encrypt_block(block, crypto_ctx.key);
        
        // Convert back to bytes
        encrypted_buffer[i] = block[0] & 0xFF;
        encrypted_buffer[i + 1] = (block[0] >> 8) & 0xFF;
        encrypted_buffer[i + 2] = (block[0] >> 16) & 0xFF;
        encrypted_buffer[i + 3] = (block[0] >> 24) & 0xFF;
        encrypted_buffer[i + 4] = block[1] & 0xFF;
        encrypted_buffer[i + 5] = (block[1] >> 8) & 0xFF;
        encrypted_buffer[i + 6] = (block[1] >> 16) & 0xFF;
        encrypted_buffer[i + 7] = (block[1] >> 24) & 0xFF;
    }
    
    *encrypted_len = padded_len;
    printf("Encrypted %zu bytes of JSON data using XTEA\n", *encrypted_len);
    return true;
}

// Base64 encoding for HTTP transmission
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const uint8_t* data, size_t len, char* output) {
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3, j += 4) {
        uint32_t triple = (data[i] << 16) | 
                         ((i + 1 < len) ? (data[i + 1] << 8) : 0) | 
                         ((i + 2 < len) ? data[i + 2] : 0);
        
        output[j] = base64_chars[(triple >> 18) & 0x3F];
        output[j + 1] = base64_chars[(triple >> 12) & 0x3F];
        output[j + 2] = (i + 1 < len) ? base64_chars[(triple >> 6) & 0x3F] : '=';
        output[j + 3] = (i + 2 < len) ? base64_chars[triple & 0x3F] : '=';
    }
    output[j] = '\0';
}

// Calibration function with noise analysis
bool calibrate_sensors() {
    printf("\n=== PARTICLE COUNTER CALIBRATION ===\n");
    printf("Ensure vessel is clean with clear liquid (no particles)\n");
    printf("Keep system stable - avoid vibrations\n");
    printf("Calibrating in 5 seconds...\n");
    
    sleep_ms(5000);
    
    float sensor1_readings[CALIBRATION_SAMPLES];
    float sensor2_readings[CALIBRATION_SAMPLES];
    float sensor1_sum = 0, sensor2_sum = 0;
    const float conversion_factor = 3.3f / (1 << 12);
    
    printf("Taking %d calibration samples", CALIBRATION_SAMPLES);
    
    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        // Read sensor 1
        adc_select_input(0);
        uint16_t raw1 = adc_read();
        sensor1_readings[i] = raw1 * conversion_factor;
        sensor1_sum += sensor1_readings[i];
        
        // Read sensor 2
        adc_select_input(1);
        uint16_t raw2 = adc_read();
        sensor2_readings[i] = raw2 * conversion_factor;
        sensor2_sum += sensor2_readings[i];
        
        if (i % 20 == 0) {
            printf(".");
            fflush(stdout);
        }
        
        sleep_ms(25); // 25ms between samples for stable reading
    }
    
    // Calculate baseline averages
    calibration.sensor1_baseline = sensor1_sum / CALIBRATION_SAMPLES;
    calibration.sensor2_baseline = sensor2_sum / CALIBRATION_SAMPLES;
    
    // Calculate noise levels (standard deviation)
    float sensor1_variance = 0, sensor2_variance = 0;
    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        float diff1 = sensor1_readings[i] - calibration.sensor1_baseline;
        float diff2 = sensor2_readings[i] - calibration.sensor2_baseline;
        sensor1_variance += diff1 * diff1;
        sensor2_variance += diff2 * diff2;
    }
    
    calibration.sensor1_noise_level = sqrtf(sensor1_variance / CALIBRATION_SAMPLES);
    calibration.sensor2_noise_level = sqrtf(sensor2_variance / CALIBRATION_SAMPLES);
    
    calibration.calibrated = true;
    calibration.calibration_timestamp = to_ms_since_boot(get_absolute_time());
    
    printf("\n=== CALIBRATION COMPLETE ===\n");
    printf("Sensor 1: Baseline=%.4fV, Noise=%.4fV\n", 
           calibration.sensor1_baseline, calibration.sensor1_noise_level);
    printf("Sensor 2: Baseline=%.4fV, Noise=%.4fV\n", 
           calibration.sensor2_baseline, calibration.sensor2_noise_level);
    
    float threshold1 = calibration.sensor1_baseline * (DETECTION_THRESHOLD_PERCENT / 100.0f);
    float threshold2 = calibration.sensor2_baseline * (DETECTION_THRESHOLD_PERCENT / 100.0f);
    printf("Detection thresholds: S1=%.4fV, S2=%.4fV (%d%% drop)\n", 
           threshold1, threshold2, DETECTION_THRESHOLD_PERCENT);
    
    // Check if system is stable enough for particle detection
    if (calibration.sensor1_noise_level > 0.02f || calibration.sensor2_noise_level > 0.02f) {
        printf("⚠️  WARNING: High noise levels detected!\n");
        printf("   Consider improving electrical stability\n");
    } else {
        printf("✅ System stable - ready for particle detection\n");
    }
    
    printf("=== READY FOR PARTICLE COUNTING ===\n\n");
    return true;
}

// Enhanced particle detection with duration analysis
bool detect_particle_event(uint8_t sensor_id, float current_voltage, detection_state_t *state) {
    if (!calibration.calibrated) return false;
    
    float baseline = (sensor_id == 1) ? calibration.sensor1_baseline : calibration.sensor2_baseline;
    float threshold_voltage = baseline * (1.0f - DETECTION_THRESHOLD_PERCENT / 100.0f);
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    // Update running averages
    state->voltage_sum += current_voltage;
    state->voltage_samples++;
    
    if (!state->in_event && current_voltage < threshold_voltage) {
        // Event started
        state->in_event = true;
        state->event_start_time = current_time;
        state->event_min_voltage = current_voltage;
        state->event_baseline = baseline;
        return false; // Don't count yet
    }
    
    if (state->in_event) {
        // Update minimum voltage during event
        if (current_voltage < state->event_min_voltage) {
            state->event_min_voltage = current_voltage;
        }
        
        if (current_voltage >= threshold_voltage) {
            // Event ended - analyze duration
            uint32_t duration = current_time - state->event_start_time;
            float signal_drop = state->event_baseline - state->event_min_voltage;
            float drop_percent = (signal_drop / state->event_baseline) * 100.0f;
            
            state->in_event = false;
            
            // Validate event duration and amplitude
            if (duration >= MIN_PARTICLE_DURATION_MS && duration <= MAX_PARTICLE_DURATION_MS) {
                state->valid_events++;
                printf("PARTICLE S%d: %lums, %.1f%% drop, %.4fV amplitude\n",
                       sensor_id, duration, drop_percent, signal_drop);
                return true; // Valid particle detected
            } else {
                state->false_positives++;
                printf("False S%d: %lums (out of range)\n", sensor_id, duration);
                return false;
            }
        }
    }
    
    return false;
}

// Initialize counting period
void start_counting_period() {
    printf("\n=== STARTING PARTICLE COUNTING ===\n");
    printf("Counting period: %d seconds\n", COUNTING_PERIOD_SEC);
    printf("Add your sample to the vessel now...\n\n");
    
    // Reset counting data
    memset(&count_data, 0, sizeof(count_data));
    memset(&sensor1_state, 0, sizeof(sensor1_state));
    memset(&sensor2_state, 0, sizeof(sensor2_state));
    
    count_data.counting_active = true;
    count_data.start_timestamp = to_ms_since_boot(get_absolute_time());
    count_data.counting_duration_sec = COUNTING_PERIOD_SEC;
    count_data.sensor1_baseline = calibration.sensor1_baseline;
    count_data.sensor2_baseline = calibration.sensor2_baseline;
}

// Finalize counting and calculate results
void finalize_counting_period() {
    count_data.counting_active = false;
    count_data.end_timestamp = to_ms_since_boot(get_absolute_time());
    
    // Get final counts
    count_data.sensor1_particle_count = sensor1_state.valid_events;
    count_data.sensor2_particle_count = sensor2_state.valid_events;
    count_data.sensor1_false_positives = sensor1_state.false_positives;
    count_data.sensor2_false_positives = sensor2_state.false_positives;
    
    // Calculate average voltages during counting
    if (sensor1_state.voltage_samples > 0) {
        count_data.avg_sensor1_voltage = sensor1_state.voltage_sum / sensor1_state.voltage_samples;
    }
    if (sensor2_state.voltage_samples > 0) {
        count_data.avg_sensor2_voltage = sensor2_state.voltage_sum / sensor2_state.voltage_samples;
    }
    
    // Calculate concentration (particles per minute)
    float actual_duration_min = count_data.counting_duration_sec / 60.0f;
    count_data.sensor1_concentration_per_min = count_data.sensor1_particle_count / actual_duration_min;
    count_data.sensor2_concentration_per_min = count_data.sensor2_particle_count / actual_duration_min;
    
    printf("\n=== COUNTING COMPLETE ===\n");
    printf("Duration: %lu seconds\n", count_data.counting_duration_sec);
    printf("Sensor 1: %lu particles (%.1f/min)\n", 
           count_data.sensor1_particle_count, count_data.sensor1_concentration_per_min);
    printf("Sensor 2: %lu particles (%.1f/min)\n", 
           count_data.sensor2_particle_count, count_data.sensor2_concentration_per_min);
    printf("False positives: S1=%lu, S2=%lu\n", 
           count_data.sensor1_false_positives, count_data.sensor2_false_positives);
    printf("Average voltages: S1=%.3fV, S2=%.3fV\n",
           count_data.avg_sensor1_voltage, count_data.avg_sensor2_voltage);
    printf("========================\n\n");
}

// TCP callback functions (keeping existing networking code)
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    if (err != ERR_OK) return err;
    
    client->connected = true;
    err_t write_err = tcp_write(tpcb, client->request_data, client->request_len, TCP_WRITE_FLAG_COPY);
    if (write_err == ERR_OK) {
        tcp_output(tpcb);
        client->data_sent = true;
    }
    return ERR_OK;
}

static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    
    if (p == NULL) {
        client->response_received = true;
        return ERR_OK;
    }
    
    if (err == ERR_OK) {
        if (client->response_len + p->len < sizeof(client->response_buffer) - 1) {
            memcpy(client->response_buffer + client->response_len, p->payload, p->len);
            client->response_len += p->len;
            client->response_buffer[client->response_len] = '\0';
        }
        tcp_recved(tpcb, p->len);
    }
    
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_client_err(void *arg, err_t err) {
    tcp_client_t *client = (tcp_client_t*)arg;
    if (err == -13) {
        client->response_received = true;
    }
    client->connected = false;
}

// Send encrypted HTTP POST request
bool send_encrypted_http_post(const uint8_t* encrypted_data, size_t encrypted_len) {
    memset(&tcp_client, 0, sizeof(tcp_client));
    
    // Base64 encode the encrypted data
    char base64_data[3000]; // Generous buffer for base64
    base64_encode(encrypted_data, encrypted_len, base64_data);
    
    static char http_request[4096];
    int content_length = strlen(base64_data);
    
    snprintf(http_request, sizeof(http_request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/x-encrypted-data\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "User-Agent: PicoW-ParticleCounter/1.0\r\n"
        "X-Encryption: XTEA-64\r\n"
        "\r\n"
        "%s",
        SERVER_PATH, SERVER_IP, SERVER_PORT, content_length, base64_data);
    
    tcp_client.request_data = http_request;
    tcp_client.request_len = strlen(http_request);
    
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) return false;
    
    tcp_client.tcp_pcb = pcb;
    tcp_arg(pcb, &tcp_client);
    tcp_err(pcb, tcp_client_err);
    tcp_recv(pcb, tcp_client_recv);
    
    ip_addr_t server_addr;
    if (!ip4addr_aton(SERVER_IP, &server_addr)) {
        tcp_close(pcb);
        return false;
    }
    
    err_t err = tcp_connect(pcb, &server_addr, SERVER_PORT, tcp_client_connected);
    if (err != ERR_OK) {
        tcp_close(pcb);
        return false;
    }
    
    int timeout = 0;
    while (!tcp_client.response_received && timeout < 100) {
        sleep_ms(100);
        cyw43_arch_poll();
        timeout++;
    }
    
    if (tcp_client.tcp_pcb) {
        tcp_close(tcp_client.tcp_pcb);
    }
    
    return tcp_client.response_received;
}

// Send particle counting results to server (now encrypted)
bool send_particle_count_data() {
    char json_payload[1024];
    
    snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"type\":\"particle_count\","
        "\"timestamp\":%lu,"
        "\"counting_duration_sec\":%lu,"
        "\"sensor1_particles\":%lu,"
        "\"sensor2_particles\":%lu,"
        "\"sensor1_concentration_per_min\":%.2f,"
        "\"sensor2_concentration_per_min\":%.2f,"
        "\"sensor1_baseline\":%.4f,"
        "\"sensor2_baseline\":%.4f,"
        "\"avg_sensor1_voltage\":%.4f,"
        "\"avg_sensor2_voltage\":%.4f,"
        "\"sensor1_false_positives\":%lu,"
        "\"sensor2_false_positives\":%lu,"
        "\"detection_threshold_percent\":%d,"
        "\"calibrated\":%s,"
        "\"measurement_quality\":\"%.1f%%\""
        "}",
        count_data.end_timestamp,
        count_data.counting_duration_sec,
        count_data.sensor1_particle_count,
        count_data.sensor2_particle_count,
        count_data.sensor1_concentration_per_min,
        count_data.sensor2_concentration_per_min,
        count_data.sensor1_baseline,
        count_data.sensor2_baseline,
        count_data.avg_sensor1_voltage,
        count_data.avg_sensor2_voltage,
        count_data.sensor1_false_positives,
        count_data.sensor2_false_positives,
        DETECTION_THRESHOLD_PERCENT,
        calibration.calibrated ? "true" : "false",
        // Simple quality metric: ratio of valid to total events
        (count_data.sensor1_particle_count + count_data.sensor2_particle_count) > 0 ?
        (float)(count_data.sensor1_particle_count + count_data.sensor2_particle_count) * 100.0f /
        (count_data.sensor1_particle_count + count_data.sensor2_particle_count + 
         count_data.sensor1_false_positives + count_data.sensor2_false_positives) : 100.0f
    );

    printf("Encrypting and transmitting particle count data...\n");
    
    // Encrypt the JSON data
    uint8_t encrypted_data[2048];
    size_t encrypted_len;
    
    if (!encrypt_json_data(json_payload, encrypted_data, &encrypted_len)) {
        printf("Failed to encrypt particle count data\n");
        return false;
    }
    
    // Send encrypted data
    return send_encrypted_http_post(encrypted_data, encrypted_len);
}

// Connect to WiFi
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
    return true;
}

int main() {
    stdio_init_all();
    
    // Give extra time for USB serial connection to initialize
    sleep_ms(3000);
    
    printf("\n\n");
    printf("======================================\n");
    printf("STARTING PICO PARTICLE COUNTER\n");
    printf("======================================\n");
    
    // IMMEDIATE DEVICE ID TEST
    printf("Testing device ID function...\n");
    pico_unique_board_id_t test_board_id;
    pico_get_unique_board_id(&test_board_id);
    
    printf("SUCCESS: Got device ID!\n");
    printf("DEVICE ID: ");
    for (int i = 0; i < 8; i++) {
        printf("%02x", test_board_id.id[i]);
    }
    printf("\n");
    printf("Copy this to PHP: $DEVICE_ID = \"");
    for (int i = 0; i < 8; i++) {
        printf("%02x", test_board_id.id[i]);
    }
    printf("\";\n");
    printf("======================================\n");
    
    printf("Waiting for systems to initialize...\n");
    sleep_ms(2000);
    
    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return -1;
    }
    
    printf("Wi-Fi module initialized OK\n");
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    
    // Initialize XTEA encryption (this will show device ID)
    printf("Initializing encryption...\n");
    init_xtea_encryption();
    
    // Initialize ADC
    printf("Initializing ADC...\n");
    adc_init();
    adc_gpio_init(PHOTODIODE_1_GPIO);
    adc_gpio_init(PHOTODIODE_2_GPIO);
    
    // Initialize laser pins
    printf("Initializing GPIO pins...\n");
    gpio_init(LASER_PIN_1);
    gpio_set_dir(LASER_PIN_1, GPIO_OUT);
    gpio_init(LASER_PIN_2);
    gpio_set_dir(LASER_PIN_2, GPIO_OUT);
    
    // Initialize calibration button (optional)
    gpio_init(CALIBRATION_BUTTON_PIN);
    gpio_set_dir(CALIBRATION_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(CALIBRATION_BUTTON_PIN);
    
    printf("\n");
    printf("Raspberry Pi Pico 2W Particle Counter System\n");
    printf("==========================================\n");
    printf("🔒 XTEA Encryption Enabled\n");
    
    sleep_ms(2000);
    
    // Connect to WiFi
    if (!connect_to_wifi()) {
        printf("Cannot continue without WiFi connection\n");
        return -1;
    }
    
    printf("Turning on lasers...\n");
    gpio_put(LASER_PIN_1, 1);
    gpio_put(LASER_PIN_2, 1);
    printf("Both lasers are ON\n");
    
    sleep_ms(3000);
    
    // Initial calibration
    calibrate_sensors();
    
    printf("System ready for particle counting\n");
    printf("Press calibration button (GPIO 14) to recalibrate\n");
    printf("Starting first counting period in 10 seconds...\n\n");
    
    sleep_ms(10000);
    
    const float conversion_factor = 3.3f / (1 << 12);
    
    while (1) {
        // Check for manual calibration
        if (!gpio_get(CALIBRATION_BUTTON_PIN)) {
            printf("Manual calibration requested!\n");
            calibrate_sensors();
            sleep_ms(1000); // Debounce
        }
        
        // Start counting period
        start_counting_period();
        
        uint32_t period_start = to_ms_since_boot(get_absolute_time());
        uint32_t last_transmission = period_start;
        
        // Counting loop
        while (count_data.counting_active) {
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            
            // Check if counting period is complete
            if ((current_time - period_start) >= (COUNTING_PERIOD_SEC * 1000)) {
                finalize_counting_period();
                break;
            }
            
            // Read sensors at high frequency
            adc_select_input(0);
            uint16_t raw1 = adc_read();
            float voltage1 = raw1 * conversion_factor;
            
            adc_select_input(1);
            uint16_t raw2 = adc_read();
            float voltage2 = raw2 * conversion_factor;
            
            // Detect particle events
            detect_particle_event(1, voltage1, &sensor1_state);
            detect_particle_event(2, voltage2, &sensor2_state);
            
            // Send intermediate updates every 30 seconds
            if ((current_time - last_transmission) >= (TRANSMISSION_INTERVAL_SEC * 1000)) {
                printf("Intermediate update: S1=%lu particles, S2=%lu particles\n",
                       sensor1_state.valid_events, sensor2_state.valid_events);
                last_transmission = current_time;
            }
            
            sleep_ms(SAMPLING_RATE_MS);
            cyw43_arch_poll();
        }
        
        // Send final results (now encrypted)
        if (send_particle_count_data()) {
            printf("Encrypted particle count data transmitted successfully!\n");
        } else {
            printf("Failed to transmit encrypted particle count data!\n");
        }
        
        // Wait before next counting period
        printf("Next counting period starts in 30 seconds...\n");
        sleep_ms(30000);
    }
    
    cyw43_arch_deinit();
    return 0;
}