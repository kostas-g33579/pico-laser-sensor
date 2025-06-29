<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

$csv_file = 'sensor_data.csv';

if (!file_exists($csv_file)) {
    echo json_encode(['status' => 'error', 'message' => 'No data file found', 'readings' => []]);
    exit();
}

$lines = file($csv_file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
if (count($lines) < 2) {
    echo json_encode(['status' => 'success', 'count' => 0, 'readings' => []]);
    exit();
}

$header = str_getcsv($lines[0]);
$readings = [];

// Get last 50 readings (skip header)
$data_lines = array_slice($lines, 1);
$data_lines = array_reverse($data_lines); // Most recent first
$data_lines = array_slice($data_lines, 0, 50); // Last 50

foreach ($data_lines as $line) {
    $values = str_getcsv($line);
    if (count($values) >= count($header)) {
        $reading = array_combine($header, $values);
        $readings[] = $reading;
    }
}

echo json_encode([
    'status' => 'success',
    'count' => count($readings),
    'last_updated' => date('Y-m-d H:i:s'),
    'readings' => $readings
], JSON_PRETTY_PRINT);
?>