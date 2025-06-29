<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST, GET, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

$timestamp = date('Y-m-d H:i:s');

// Log all requests for debugging
$debug_log = "debug.log";
file_put_contents($debug_log, "[$timestamp] " . $_SERVER['REQUEST_METHOD'] . " from " . ($_SERVER['REMOTE_ADDR'] ?? 'unknown') . "\n", FILE_APPEND);

// Handle OPTIONS request
if ($_SERVER['REQUEST_METHOD'] == 'OPTIONS') {
    http_response_code(200);
    exit();
}

// Only accept POST
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    $error = ['error' => 'Method not allowed', 'method' => $_SERVER['REQUEST_METHOD']];
    echo json_encode($error);
    exit();
}

// Get POST data
$input = file_get_contents('php://input');
file_put_contents($debug_log, "[$timestamp] Raw input: '$input'\n", FILE_APPEND);

if (empty($input)) {
    $error = ['error' => 'No POST data received', 'content_length' => $_SERVER['CONTENT_LENGTH'] ?? 'unknown'];
    file_put_contents($debug_log, "[$timestamp] ERROR: No POST data\n", FILE_APPEND);
    echo json_encode($error);
    exit();
}

$data = json_decode($input, true);
if ($data === null) {
    $error = ['error' => 'Invalid JSON', 'json_error' => json_last_error_msg(), 'raw_input' => $input];
    file_put_contents($debug_log, "[$timestamp] ERROR: JSON decode failed\n", FILE_APPEND);
    echo json_encode($error);
    exit();
}

// Validate required fields
$required = ['sensor1_raw', 'sensor1_voltage', 'sensor2_raw', 'sensor2_voltage', 'timestamp'];
foreach ($required as $field) {
    if (!isset($data[$field])) {
        $error = ['error' => "Missing field: $field", 'received' => array_keys($data)];
        echo json_encode($error);
        exit();
    }
}

// Add server timestamp
$data['server_timestamp'] = $timestamp;
$data['id'] = time() . '_' . rand(1000, 9999); // Simple ID

// Store in CSV file (easy to read)
$csv_file = 'sensor_data.csv';
$csv_line = implode(',', [
    $data['id'],
    $data['sensor1_raw'],
    $data['sensor1_voltage'],
    $data['sensor2_raw'],
    $data['sensor2_voltage'],
    $data['timestamp'],
    '"' . $data['server_timestamp'] . '"'
]) . "\n";

// Create header if file doesn't exist
if (!file_exists($csv_file)) {
    file_put_contents($csv_file, "id,sensor1_raw,sensor1_voltage,sensor2_raw,sensor2_voltage,device_timestamp,server_timestamp\n");
}

file_put_contents($csv_file, $csv_line, FILE_APPEND | LOCK_EX);

// Also log in human readable format
$log_file = 'sensor_data.log';
$log_entry = "[$timestamp] " . json_encode($data) . "\n";
file_put_contents($log_file, $log_entry, FILE_APPEND | LOCK_EX);

// Success response
$response = [
    'status' => 'success',
    'message' => 'Data received and stored successfully',
    'record_id' => $data['id'],
    'server_time' => $timestamp,
    'data_count' => file_exists($csv_file) ? (count(file($csv_file)) - 1) : 0
];

file_put_contents($debug_log, "[$timestamp] SUCCESS: Data stored\n", FILE_APPEND);
echo json_encode($response, JSON_PRETTY_PRINT);
?>