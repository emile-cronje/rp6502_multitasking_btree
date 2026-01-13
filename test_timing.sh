#!/bin/bash

# Test timing script - runs tests with different producer/consumer configurations

cd /home/emilec/dev/github_ec/rp6502_multitasking_btree

echo "=== RP6502 Multitasking Work-Queue Timing Tests ==="
echo ""

# Function to run a test with given configuration
run_test() {
    local producers=$1
    local consumers=$2
    local item_count=$3
    local config_name="$producers producers, $consumers consumers, $item_count items"
    
    echo "=========================================="
    echo "Testing: $config_name"
    echo "=========================================="
    
    # Update main.c with new configuration
    sed -i "s/#define NUM_PRODUCERS.*/#define NUM_PRODUCERS $producers/" src/main.c
    sed -i "s/#define NUM_CONSUMERS.*/#define NUM_CONSUMERS $consumers/" src/main.c
    sed -i "s/#define TEST_ITEM_COUNT.*/#define TEST_ITEM_COUNT $item_count/" src/main.c
    
    # Rebuild
    cmake --build build 2>&1 | grep -E "(error|Error|ERROR)" || echo "Build successful"
    
    if [ ! -f build/rp6502_os.rp6502 ]; then
        echo "ERROR: Build failed!"
        return 1
    fi
    
    echo ""
    echo "Running test..."
    echo ""
    
    # Run the emulator and capture timing output
    timeout 60 python3 tools/rp6502.py build/rp6502_os.rp6502 2>&1 | grep -E "\[(TIMING|TEST_VALIDATOR)\]" | head -30
    
    echo ""
}

# Test configurations
# Start with smaller counts since validation can take time
echo "Test 1: Single producer, single consumer (baseline)"
run_test 1 1 100

echo ""
echo "Test 2: Two producers, one consumer (producer bottleneck)"
run_test 2 1 100

echo ""
echo "Test 3: One producer, two consumers (consumer underutilized)"
run_test 1 2 100

echo ""
echo "Test 4: Two producers, two consumers (balanced)"
run_test 2 2 100

echo ""
echo "Test 5: Three producers, two consumers"
run_test 3 2 100

echo ""
echo "Test 6: Two producers, three consumers"
run_test 2 3 100

echo ""
echo "Test 7: Four producers, four consumers (high contention)"
run_test 4 4 100

echo ""
echo "Test 8: Higher item count - Two producers, two consumers"
run_test 2 2 150

echo ""
echo "=========================================="
echo "All tests complete!"
echo "=========================================="
