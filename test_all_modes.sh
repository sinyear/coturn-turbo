#!/bin/bash
set -e

COTURN_DIR="/home/web/workspace/coturn-turbo-fast/coturn-turbo"
TEST_DIR="/home/web/workspace/coturn-turbo-fast/coturn-turbo/.claude/skills/webrtc-coturn-test-skill"

run_test() {
    local mode_name="$1"
    local config_file="$2"
    local extra_args="$3"

    echo "========================================"
    echo "Testing: $mode_name"
    echo "Config:  $config_file"
    echo "Args:    $extra_args"
    echo "========================================"

    cd "$COTURN_DIR"

    # Kill any existing turnserver
    sudo pkill -9 turnserver 2>/dev/null || true
    sleep 1

    # Start server
    sudo turnserver -c "$config_file" $extra_args > /tmp/turnserver_test.log 2>&1 &
    sleep 3

    # Check if running
    if ! pgrep -x turnserver > /dev/null; then
        echo "❌ FAILED: Server did not start"
        tail -20 /tmp/turnserver_test.log
        return 1
    fi
    echo "✅ Server started (PID: $(pgrep -x turnserver))"

    # Run test
    cd "$TEST_DIR"
    local result
    result=$(node regular-diagnostic.js 2>&1)
    local exit_code=$?

    echo "$result"

    # Stop server
    sudo pkill -9 turnserver 2>/dev/null || true
    sleep 1

    if echo "$result" | grep -q "SUCCESS"; then
        echo "✅ $mode_name PASSED"
        return 0
    else
        echo "❌ $mode_name FAILED"
        return 1
    fi
}

TOTAL=0
PASSED=0

# Mode 1: Standard (no turbo)
echo ""
echo "=== MODE 1: Standard (no turbo) ==="
# Build standard
cd "$COTURN_DIR"
make clean > /dev/null 2>&1
./configure > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
sudo make install > /dev/null 2>&1

TOTAL=$((TOTAL+1))
if run_test "Mode 1: Standard" "/tmp/turnserver_test_standard.conf" ""; then
    PASSED=$((PASSED+1))
fi

# Mode 2: Turbo + io_uring WITH --turbo flag
echo ""
echo "=== MODE 2: Turbo + io_uring (--turbo) ==="
cd "$COTURN_DIR"
make clean > /dev/null 2>&1
./configure --turbo > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
sudo make install > /dev/null 2>&1

TOTAL=$((TOTAL+1))
if run_test "Mode 2: Turbo+io_uring" "/tmp/turnserver_test_turbo_io_uring.conf" "--turbo"; then
    PASSED=$((PASSED+1))
fi

# Mode 3: Turbo + AF_XDP WITH --turbo flag
echo ""
echo "=== MODE 3: Turbo + AF_XDP (--turbo) ==="
cd "$COTURN_DIR"
make clean > /dev/null 2>&1
./configure --turbo --turbo-backend=afxdp > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
sudo make install > /dev/null 2>&1

TOTAL=$((TOTAL+1))
if run_test "Mode 3: Turbo+AF_XDP" "/tmp/turnserver_test_turbo_afxdp.conf" "--turbo"; then
    PASSED=$((PASSED+1))
fi

# Mode 4: Turbo + rooms WITH --turbo flag
echo ""
echo "=== MODE 4: Turbo + rooms (--turbo --turbo-rooms) ==="
cd "$COTURN_DIR"
make clean > /dev/null 2>&1
./configure --turbo --turbo-rooms > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
sudo make install > /dev/null 2>&1

TOTAL=$((TOTAL+1))
if run_test "Mode 4: Turbo+rooms" "/tmp/turnserver_test_turbo_rooms.conf" "--turbo"; then
    PASSED=$((PASSED+1))
fi

echo ""
echo "========================================"
echo "Results: $PASSED/$TOTAL modes passed"
echo "========================================"

# Cleanup
sudo pkill -9 turnserver 2>/dev/null || true

exit $(( TOTAL - PASSED ))
