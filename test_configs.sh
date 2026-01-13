#!/bin/bash

# Test script to measure timing with different producer/consumer configurations

BUILD_DIR="/home/emilec/dev/github_ec/rp6502_multitasking_btree/build"
EMULATOR="${BUILD_DIR}/rp6502_os.rp6502"
CFG_FILE="/home/emilec/dev/github_ec/rp6502_multitasking_btree/rp6502.cfg"

# Test configurations: (producers, consumers, items)
CONFIGS=(
    "1 1 100"
    "1 2 100"
    "2 1 100"
    "2 2 100"
    "3 2 100"
    "2 3 100"
    "4 2 100"
    "1 4 100"
)

echo "================================================"
echo "RP6502 Work-Queue Timing Analysis"
echo "================================================"
echo ""

for config in "${CONFIGS[@]}"; do
    read PROD CONS ITEMS <<< "$config"
    
    echo "Testing: $PROD producers, $CONS consumers, $ITEMS items"
    echo "---"
    
    # Update CMakeLists.txt with test configuration
    sed -i "s/NUM_PRODUCERS [0-9]\+/NUM_PRODUCERS $PROD/" /home/emilec/dev/github_ec/rp6502_multitasking_btree/CMakeLists.txt
    sed -i "s/NUM_CONSUMERS [0-9]\+/NUM_CONSUMERS $CONS/" /home/emilec/dev/github_ec/rp6502_multitasking_btree/CMakeLists.txt
    sed -i "s/TEST_ITEM_COUNT [0-9]\+/TEST_ITEM_COUNT $ITEMS/" /home/emilec/dev/github_ec/rp6502_multitasking_btree/CMakeLists.txt
    
    # Rebuild
    cd "$BUILD_DIR"
    cmake --build . > /dev/null 2>&1
    
    # Run test and capture timing info
    timeout 30 "$EMULATOR" < "$CFG_FILE" 2>&1 | grep -E "TIMING|items_consumed|CPU efficiency|items/sec|Avg ticks|CPU total|CPU active" | tail -12
    
    echo ""
done

echo "================================================"
echo "Test complete"
echo "================================================"
