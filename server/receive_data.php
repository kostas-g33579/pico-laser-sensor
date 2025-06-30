<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST, GET, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type, X-Encryption');

$timestamp = date('Y-m-d H:i:s');
$debug_log = "debug.log";

// XTEA Encryption Configuration
// IMPORTANT: Update this with your Pico's device ID (8 hex bytes)
// Get this from your Pico's console output during initialization
$DEVICE_ID = "3bc1a5a19108b57c"; // Replace with actual device ID from Pico
$XTEA_KEY_SIZE = 16; // 4 x 32-bit words
$XTEA_BLOCK_SIZE = 8; // 8 bytes per block
$XTEA_ROUNDS = 32;

// Generate XTEA key from device ID (matching Pico's method)
function generate_xtea_key($device_id_hex) {
    global $XTEA_KEY_SIZE;
    
    // Convert hex string to bytes
    $device_bytes = hex2bin($device_id_hex);
    if (strlen($device_bytes) !== 8) {
        throw new Exception("Device ID must be exactly 8 bytes (16 hex chars)");
    }
    
    // Generate 4 x 32-bit key words (matching Pico's logic)
    $key = array();
    $key[0] = (ord($device_bytes[0]) << 24) | (ord($device_bytes[1]) << 16) | 
              (ord($device_bytes[2]) << 8) | ord($device_bytes[3]);
    $key[1] = (ord($device_bytes[4]) << 24) | (ord($device_bytes[5]) << 16) | 
              (ord($device_bytes[6]) << 8) | ord($device_bytes[7]);
    $key[2] = $key[0] ^ 0xAAAAAAAA; // Add variety (matching Pico)
    $key[3] = $key[1] ^ 0x55555555; // Add variety (matching Pico)
    
    return $key;
}

// XTEA decryption function
function xtea_decrypt_block($data, $key) {
    global $XTEA_ROUNDS;
    
    $v0 = $data[0];
    $v1 = $data[1];
    $sum = 0xC6EF3720; // delta * rounds
    $delta = 0x9E3779B9;
    
    for ($i = 0; $i < $XTEA_ROUNDS; $i++) {
        $v1 = ($v1 - ((($v0 << 4) ^ ($v0 >> 5)) + $v0) ^ ($sum + $key[($sum >> 11) & 3])) & 0xFFFFFFFF;
        $sum = ($sum - $delta) & 0xFFFFFFFF;
        $v0 = ($v0 - ((($v1 << 4) ^ ($v1 >> 5)) + $v1) ^ ($sum + $key[$sum & 3])) & 0xFFFFFFFF;
    }
    
    return array($v0, $v1);
}

// Decrypt XTEA-encrypted data
function decrypt_xtea_data($encrypted_data, $key) {
    global $XTEA_BLOCK_SIZE;
    
    if (strlen($encrypted_data) % $XTEA_BLOCK_SIZE !== 0) {
        throw new Exception("Encrypted data length must be multiple of " . $XTEA_BLOCK_SIZE);
    }
    
    $decrypted = '';
    
    // Decrypt each 8-byte block
    for ($i = 0; $i < strlen($encrypted_data); $i += $XTEA_BLOCK_SIZE) {
        $block_bytes = substr($encrypted_data, $i, $XTEA_BLOCK_SIZE);
        
        // Convert bytes to 32-bit words (little endian)
        $data = array();
        $data[0] = (ord($block_bytes[3]) << 24) | (ord($block_bytes[2]) << 16) | 
                   (ord($block_bytes[1]) << 8) | ord($block_bytes[0]);
        $data[1] = (ord($block_bytes[7]) << 24) | (ord($block_bytes[6]) << 16) | 
                   (ord($block_bytes[5]) << 8) | ord($block_bytes[4]);
        
        // Decrypt block
        $decrypted_block = xtea_decrypt_block($data, $key);
        
        // Convert back to bytes (little endian)
        $decrypted .= chr($decrypted_block[0] & 0xFF);
        $decrypted .= chr(($decrypted_block[0] >> 8) & 0xFF);
        $decrypted .= chr(($decrypted_block[0] >> 16) & 0xFF);
        $decrypted .= chr(($decrypted_block[0] >> 24) & 0xFF);
        $decrypted .= chr($decrypted_block[1] & 0xFF);
        $decrypted .= chr(($decrypted_block[1] >> 8) & 0xFF);
        $decrypted .= chr(($decrypted_block[1] >> 16) & 0xFF);
        $decrypted .= chr(($decrypted_block[1] >> 24) & 0xFF);
    }
    
    // Remove padding
    $padding = ord($decrypted[strlen($decrypted) - 1]);
    if ($padding > 0 && $padding <= $XTEA_BLOCK_SIZE) {
        // Verify padding
        $valid_padding = true;
        for ($i = strlen($decrypted) - $padding; $i < strlen($decrypted); $i++) {
            if (ord($decrypted[$i]) !== $padding) {
                $valid_padding = false;
                break;
            }
        }
        
        if ($valid_padding) {
            $decrypted = substr($decrypted, 0, strlen($decrypted) - $padding);
        }
    }
    
    return $decrypted;
}

// Check if request is encrypted
function is_encrypted_request() {
    $content_type = $_SERVER['CONTENT_TYPE'] ?? '';
    $encryption_header = $_SERVER['HTTP_X_ENCRYPTION'] ?? '';
    
    return $content_type === 'application/x-encrypted-data' || 
           strpos($encryption_header, 'XTEA') !== false ||
           strpos($encryption_header, 'AES') !== false; // Backward compatibility
}

// Log incoming request
file_put_contents($debug_log, "[$timestamp] " . $_SERVER['REQUEST_METHOD'] . " from " . ($_SERVER['REMOTE_ADDR'] ?? 'unknown') . "\n", FILE_APPEND);

// Handle OPTIONS request
if ($_SERVER['REQUEST_METHOD'] == 'OPTIONS') {
    http_response_code(200);
    exit();
}

// Only accept POST
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    echo json_encode(['error' => 'Method not allowed']);
    exit();
}

// Get POST data
$input = file_get_contents('php://input');
file_put_contents($debug_log, "[$timestamp] Raw input length: " . strlen($input) . "\n", FILE_APPEND);

if (empty($input)) {
    echo json_encode(['error' => 'No POST data received']);
    exit();
}

$data = null;

try {
    // Check if data is encrypted
    if (is_encrypted_request()) {
        file_put_contents($debug_log, "[$timestamp] Processing encrypted request\n", FILE_APPEND);
        
        // Decode base64
        $encrypted_binary = base64_decode($input);
        if ($encrypted_binary === false) {
            throw new Exception("Failed to decode base64 data");
        }
        
        file_put_contents($debug_log, "[$timestamp] Decoded " . strlen($encrypted_binary) . " bytes of encrypted data\n", FILE_APPEND);
        
        // Generate decryption key
        $xtea_key = generate_xtea_key($DEVICE_ID);
        
        // Decrypt the data
        $decrypted_json = decrypt_xtea_data($encrypted_binary, $xtea_key);
        file_put_contents($debug_log, "[$timestamp] Decrypted JSON: " . substr($decrypted_json, 0, 200) . "...\n", FILE_APPEND);
        
        // Parse decrypted JSON
        $data = json_decode($decrypted_json, true);
        if ($data === null) {
            throw new Exception("Failed to parse decrypted JSON: " . json_last_error_msg());
        }
        
        file_put_contents($debug_log, "[$timestamp] Successfully decrypted and parsed data\n", FILE_APPEND);
        
    } else {
        // Handle unencrypted data (for backward compatibility)
        file_put_contents($debug_log, "[$timestamp] Processing unencrypted request\n", FILE_APPEND);
        $data = json_decode($input, true);
        if ($data === null) {
            throw new Exception("Invalid JSON: " . json_last_error_msg());
        }
    }
    
} catch (Exception $e) {
    http_response_code(400);
    $error_response = [
        'status' => 'error',
        'message' => 'Decryption/parsing error: ' . $e->getMessage(),
        'encrypted' => is_encrypted_request()
    ];
    file_put_contents($debug_log, "[$timestamp] DECRYPTION ERROR: " . $e->getMessage() . "\n", FILE_APPEND);
    echo json_encode($error_response);
    exit();
}

// Add server timestamp
$data['server_timestamp'] = $timestamp;
$data['server_unix_timestamp'] = time();
$data['was_encrypted'] = is_encrypted_request();

// Handle different data types
$data_type = $data['type'] ?? 'voltage_data';

try {
    if ($data_type === 'particle_count') {
        $result = handle_particle_count_data($data, $timestamp);
        $message = 'Particle count data processed successfully';
    } else {
        // Legacy voltage data
        $result = handle_voltage_data($data, $timestamp);
        $message = 'Voltage data processed successfully';
    }
    
    $response = [
        'status' => 'success',
        'message' => $message,
        'data_type' => $data_type,
        'records_created' => $result['records_created'] ?? 1,
        'server_time' => $timestamp,
        'encrypted' => is_encrypted_request()
    ];
    
    if (isset($result['total_particles'])) {
        $response['total_particles'] = $result['total_particles'];
    }
    
} catch (Exception $e) {
    http_response_code(500);
    $response = [
        'status' => 'error',
        'message' => 'Processing error: ' . $e->getMessage(),
        'data_type' => $data_type,
        'encrypted' => is_encrypted_request()
    ];
    file_put_contents($debug_log, "[$timestamp] ERROR: " . $e->getMessage() . "\n", FILE_APPEND);
}

$log_message = is_encrypted_request() ? "ENCRYPTED $data_type processed" : "$data_type processed";
file_put_contents($debug_log, "[$timestamp] SUCCESS: $log_message\n", FILE_APPEND);
echo json_encode($response, JSON_PRETTY_PRINT);

function handle_particle_count_data($data, $timestamp) {
    // Store particle counting results in CSV
    $particle_csv = 'particle_counts.csv';
    
    // Create header if file doesn't exist
    if (!file_exists($particle_csv)) {
        $header = "server_timestamp,device_timestamp,counting_duration_sec,sensor1_particles,sensor2_particles,sensor1_concentration_per_min,sensor2_concentration_per_min,sensor1_baseline,sensor2_baseline,avg_sensor1_voltage,avg_sensor2_voltage,sensor1_false_positives,sensor2_false_positives,detection_threshold_percent,measurement_quality,calibrated,was_encrypted\n";
        file_put_contents($particle_csv, $header);
    }
    
    // Prepare CSV data (including encryption status)
    $csv_line = implode(',', [
        '"' . $timestamp . '"',
        $data['timestamp'] ?? 0,
        $data['counting_duration_sec'] ?? 0,
        $data['sensor1_particles'] ?? 0,
        $data['sensor2_particles'] ?? 0,
        $data['sensor1_concentration_per_min'] ?? 0,
        $data['sensor2_concentration_per_min'] ?? 0,
        $data['sensor1_baseline'] ?? 0,
        $data['sensor2_baseline'] ?? 0,
        $data['avg_sensor1_voltage'] ?? 0,
        $data['avg_sensor2_voltage'] ?? 0,
        $data['sensor1_false_positives'] ?? 0,
        $data['sensor2_false_positives'] ?? 0,
        $data['detection_threshold_percent'] ?? 0,
        '"' . ($data['measurement_quality'] ?? 'unknown') . '"',
        '"' . ($data['calibrated'] ?? 'false') . '"',
        '"' . ($data['was_encrypted'] ? 'true' : 'false') . '"'
    ]) . "\n";
    
    file_put_contents($particle_csv, $csv_line, FILE_APPEND | LOCK_EX);
    
    // Create detailed human-readable log entry
    $log_file = 'particle_analysis.log';
    $log_entry = "[$timestamp] PARTICLE COUNT ANALYSIS" . ($data['was_encrypted'] ? " (ENCRYPTED)" : " (UNENCRYPTED)") . "\n";
    $log_entry .= "==========================================\n";
    $log_entry .= "Duration: " . ($data['counting_duration_sec'] ?? 0) . " seconds\n";
    $log_entry .= "Sensor 1: " . ($data['sensor1_particles'] ?? 0) . " particles (" . 
                  number_format($data['sensor1_concentration_per_min'] ?? 0, 1) . "/min)\n";
    $log_entry .= "Sensor 2: " . ($data['sensor2_particles'] ?? 0) . " particles (" . 
                  number_format($data['sensor2_concentration_per_min'] ?? 0, 1) . "/min)\n";
    
    $total_particles = ($data['sensor1_particles'] ?? 0) + ($data['sensor2_particles'] ?? 0);
    $log_entry .= "Total Particles: " . $total_particles . "\n";
    
    $log_entry .= "False Positives: S1=" . ($data['sensor1_false_positives'] ?? 0) . 
                  ", S2=" . ($data['sensor2_false_positives'] ?? 0) . "\n";
    $log_entry .= "Detection Quality: " . ($data['measurement_quality'] ?? 'unknown') . "\n";
    $log_entry .= "Baseline Voltages: S1=" . number_format($data['sensor1_baseline'] ?? 0, 4) . 
                  "V, S2=" . number_format($data['sensor2_baseline'] ?? 0, 4) . "V\n";
    $log_entry .= "Average Voltages: S1=" . number_format($data['avg_sensor1_voltage'] ?? 0, 4) . 
                  "V, S2=" . number_format($data['avg_sensor2_voltage'] ?? 0, 4) . "V\n";
    $log_entry .= "Data Security: " . ($data['was_encrypted'] ? "🔒 Encrypted" : "⚠️ Unencrypted") . "\n";
    
    // Calculate sample interpretation
    $avg_concentration = (($data['sensor1_concentration_per_min'] ?? 0) + 
                         ($data['sensor2_concentration_per_min'] ?? 0)) / 2;
    
    if ($total_particles == 0) {
        $interpretation = "CLEAN SAMPLE - No particles detected";
        $classification = "CLEAN";
    } elseif ($avg_concentration < 10) {
        $interpretation = "LOW PARTICLE DENSITY - Very clean sample";
        $classification = "LOW_DENSITY";
    } elseif ($avg_concentration < 100) {
        $interpretation = "MODERATE PARTICLE DENSITY - Some contamination";
        $classification = "MODERATE_DENSITY";
    } else {
        $interpretation = "HIGH PARTICLE DENSITY - Significant contamination";
        $classification = "HIGH_DENSITY";
    }
    
    $log_entry .= "Average Concentration: " . number_format($avg_concentration, 1) . " particles/min\n";
    $log_entry .= "INTERPRETATION: " . $interpretation . "\n";
    $log_entry .= "CLASSIFICATION: " . $classification . "\n";
    
    // Add quality assessment
    $quality_num = floatval(str_replace('%', '', $data['measurement_quality'] ?? '0'));
    if ($quality_num >= 90) {
        $log_entry .= "QUALITY ASSESSMENT: Excellent measurement reliability\n";
    } elseif ($quality_num >= 75) {
        $log_entry .= "QUALITY ASSESSMENT: Good measurement reliability\n";
    } elseif ($quality_num >= 60) {
        $log_entry .= "QUALITY ASSESSMENT: Fair measurement reliability\n";
    } else {
        $log_entry .= "QUALITY ASSESSMENT: Poor measurement reliability - consider recalibration\n";
    }
    
    // Add security assessment
    if ($data['was_encrypted']) {
        $log_entry .= "SECURITY: Data transmitted securely with AES encryption ✅\n";
    } else {
        $log_entry .= "SECURITY: Data transmitted without encryption ⚠️\n";
    }
    
    // Add recommendations
    if ($total_particles > 0) {
        $log_entry .= "RECOMMENDATIONS:\n";
        if ($avg_concentration > 100) {
            $log_entry .= "- Sample requires filtration or cleaning\n";
            $log_entry .= "- Check contamination sources\n";
        } elseif ($avg_concentration > 10) {
            $log_entry .= "- Consider filtration depending on application\n";
            $log_entry .= "- Monitor contamination trends\n";
        } else {
            $log_entry .= "- Sample quality acceptable for most applications\n";
        }
        
        if ($quality_num < 75) {
            $log_entry .= "- Check system stability and recalibrate if needed\n";
        }
        
        if (!$data['was_encrypted']) {
            $log_entry .= "- Consider enabling encryption for data security\n";
        }
    }
    
    $log_entry .= "==========================================\n\n";
    
    file_put_contents($log_file, $log_entry, FILE_APPEND | LOCK_EX);
    
    // Store summary data for quick access
    $summary_file = 'latest_analysis.json';
    $summary_data = [
        'timestamp' => $timestamp,
        'total_particles' => $total_particles,
        'avg_concentration' => $avg_concentration,
        'classification' => $classification,
        'interpretation' => $interpretation,
        'quality' => $data['measurement_quality'] ?? 'unknown',
        'duration_sec' => $data['counting_duration_sec'] ?? 0,
        'calibrated' => $data['calibrated'] ?? 'false',
        'encrypted' => $data['was_encrypted'] ?? false
    ];
    file_put_contents($summary_file, json_encode($summary_data, JSON_PRETTY_PRINT));
    
    return [
        'records_created' => 1, 
        'total_particles' => $total_particles,
        'classification' => $classification
    ];
}

function handle_voltage_data($data, $timestamp) {
    // Handle legacy voltage data (for backward compatibility)
    $voltage_csv = 'voltage_data.csv';
    
    if (!file_exists($voltage_csv)) {
        $header = "server_timestamp,sensor1_raw,sensor1_voltage,sensor2_raw,sensor2_voltage,device_timestamp,was_encrypted\n";
        file_put_contents($voltage_csv, $header);
    }
    
    $csv_line = implode(',', [
        '"' . $timestamp . '"',
        $data['sensor1_raw'] ?? 0,
        $data['sensor1_voltage'] ?? 0,
        $data['sensor2_raw'] ?? 0,
        $data['sensor2_voltage'] ?? 0,
        $data['timestamp'] ?? 0,
        '"' . ($data['was_encrypted'] ? 'true' : 'false') . '"'
    ]) . "\n";
    
    file_put_contents($voltage_csv, $csv_line, FILE_APPEND | LOCK_EX);
    
    // Log voltage data
    $voltage_log = 'voltage_readings.log';
    $encryption_status = $data['was_encrypted'] ? " (ENCRYPTED)" : " (UNENCRYPTED)";
    $log_entry = "[$timestamp] Voltage Reading$encryption_status: S1=" . 
                 number_format($data['sensor1_voltage'] ?? 0, 3) . "V, S2=" .
                 number_format($data['sensor2_voltage'] ?? 0, 3) . "V\n";
    file_put_contents($voltage_log, $log_entry, FILE_APPEND | LOCK_EX);
    
    return ['records_created' => 1];
}

// Optional: Add a simple status endpoint
if (isset($_GET['status'])) {
    $status = [
        'server_time' => $timestamp,
        'particle_data_available' => file_exists('particle_counts.csv'),
        'voltage_data_available' => file_exists('voltage_data.csv'),
        'encryption_enabled' => true,
        'device_id_configured' => !empty($DEVICE_ID) && $DEVICE_ID !== "0123456789abcdef",
        'last_particle_measurement' => null,
        'total_measurements' => 0
    ];
    
    if (file_exists('latest_analysis.json')) {
        $latest = json_decode(file_get_contents('latest_analysis.json'), true);
        $status['last_particle_measurement'] = $latest;
    }
    
    if (file_exists('particle_counts.csv')) {
        $lines = file('particle_counts.csv');
        $status['total_measurements'] = count($lines) - 1; // Subtract header
    }
    
    echo json_encode($status, JSON_PRETTY_PRINT);
    exit();
}

// Show encryption configuration if accessed directly
if ($_SERVER['REQUEST_METHOD'] === 'GET' && !isset($_GET['status'])) {
    ?>
    <!DOCTYPE html>
    <html>
    <head>
        <title>Particle Counter - Encryption Configuration</title>
        <style>
            body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
            .container { background: white; padding: 30px; border-radius: 10px; max-width: 800px; margin: 0 auto; }
            .status { padding: 15px; border-radius: 5px; margin: 20px 0; }
            .status.success { background: #d4edda; border: 1px solid #c3e6cb; color: #155724; }
            .status.warning { background: #fff3cd; border: 1px solid #ffeaa7; color: #856404; }
            .status.error { background: #f8d7da; border: 1px solid #f5c6cb; color: #721c24; }
            .code { background: #f8f9fa; padding: 15px; border-radius: 5px; font-family: monospace; margin: 10px 0; }
            h1 { color: #333; }
            h2 { color: #666; border-bottom: 2px solid #667eea; padding-bottom: 10px; }
        </style>
    </head>
    <body>
        <div class="container">
            <h1>🔒 Particle Counter Encryption Status</h1>
            
            <?php if ($DEVICE_ID === "0123456789abcdef"): ?>
                <div class="status error">
                    <strong>⚠️ Configuration Required</strong><br>
                    Default device ID detected. You need to configure your Pico's actual device ID.
                </div>
            <?php else: ?>
                <div class="status success">
                    <strong>✅ Encryption Configured</strong><br>
                    Device ID: <?php echo htmlspecialchars($DEVICE_ID); ?>
                </div>
            <?php endif; ?>
            
            <h2>Setup Instructions</h2>
            
            <h3>1. Get Your Pico's Device ID</h3>
            <p>When your Pico starts up, it will print its device ID in the console. Look for output like:</p>
            <div class="code">Device ID: a1b2c3d4e5f6a7b8</div>
            
            <h3>2. Update PHP Configuration</h3>
            <p>Edit <code>receive_data.php</code> and update this line with your actual device ID:</p>
            <div class="code">$DEVICE_ID = "a1b2c3d4e5f6a7b8"; // Replace with your actual device ID</div>
            
            <h3>3. Verify Connection</h3>
            <p>Once configured, your Pico will send encrypted data and you should see "ENCRYPTED" in the logs.</p>
            
            <h2>Security Features</h2>
            <ul>
                <li><strong>XTEA-64 encryption</strong> - Lightweight, secure block cipher</li>
                <li><strong>Device-specific keys</strong> - Each Pico has a unique encryption key</li>
                <li><strong>Backward compatibility</strong> - Still accepts unencrypted data during testing</li>
                <li><strong>Transmission verification</strong> - Logs show encryption status for each measurement</li>
            </ul>
            
            <h2>Current Status</h2>
            <p><strong>Encryption:</strong> <?php echo $DEVICE_ID !== "0123456789abcdef" ? "✅ Ready" : "❌ Needs Configuration"; ?></p>
            <p><strong>Server Time:</strong> <?php echo $timestamp; ?></p>
            
            <p><a href="?status">View detailed status (JSON)</a></p>
        </div>
    </body>
    </html>
    <?php
    exit();
}
?>