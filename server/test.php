<?php
// Simple test script to debug what's happening
echo "=== PHP Server Test ===\n";
echo "Time: " . date('Y-m-d H:i:s') . "\n";
echo "Method: " . $_SERVER['REQUEST_METHOD'] . "\n";
echo "Content-Type: " . ($_SERVER['CONTENT_TYPE'] ?? 'not set') . "\n";
echo "Content-Length: " . ($_SERVER['CONTENT_LENGTH'] ?? 'not set') . "\n";

$input = file_get_contents('php://input');
echo "Raw Input Length: " . strlen($input) . "\n";
echo "Raw Input: '" . $input . "'\n";

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (empty($input)) {
        echo "ERROR: No POST data received!\n";
        echo "This might be a PowerShell/curl issue.\n";
    } else {
        $data = json_decode($input, true);
        if ($data === null) {
            echo "JSON Parse Error: " . json_last_error_msg() . "\n";
            echo "Raw bytes: ";
            for ($i = 0; $i < strlen($input); $i++) {
                echo ord($input[$i]) . " ";
            }
            echo "\n";
        } else {
            echo "JSON Parsed Successfully!\n";
            echo "Data: " . json_encode($data, JSON_PRETTY_PRINT) . "\n";
        }
    }
}

// Test creating a simple database entry
try {
    $db = new PDO('sqlite:test_sensor.db');
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    
    $db->exec("CREATE TABLE IF NOT EXISTS test_data (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        test_value TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )");
    
    $stmt = $db->prepare("INSERT INTO test_data (test_value) VALUES (?)");
    $stmt->execute(['test_from_php_' . time()]);
    
    echo "Database test: SUCCESS (ID: " . $db->lastInsertId() . ")\n";
} catch (Exception $e) {
    echo "Database test: FAILED - " . $e->getMessage() . "\n";
}

echo "=== End Test ===\n";
?>