#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_CONF="/etc/turnserver/turnserver.conf"

usage() {
  echo "Usage: $0 <mode> [--no-test] [--run-only]"
  echo ""
  echo "Modes:"
  echo "  1|standard  - Standard coturn (no Turbo)"
  echo "  2|iouring   - Turbo + io_uring (default high-perf backend)"
  echo "  3|afxdp     - Turbo + AF_XDP (extreme perf, experimental)"
  echo "  4|rooms     - Turbo + room broadcast (io_uring + rooms)"
  echo ""
  echo "Options:"
  echo "  --no-test   Skip WebRTC test"
  echo "  --run-only  Only start server, skip build & install"
  exit 1
}

MODE="${1:-}"
[ -z "$MODE" ] && usage

case "$MODE" in
  1|standard) MODE_NAME="standard";;
  2|iouring)  MODE_NAME="iouring";;
  3|afxdp)    MODE_NAME="afxdp";;
  4|rooms)    MODE_NAME="rooms";;
  *) usage;;
esac

shift
NO_TEST=false
RUN_ONLY=false
for arg in "$@"; do
  case "$arg" in
    --no-test) NO_TEST=true;;
    --run-only) RUN_ONLY=true;;
  esac
done

echo "============================================"
echo " coturn-turbo Test: Mode=$MODE_NAME"
echo "============================================"

stop_server() {
  pkill -9 turnserver 2>/dev/null || true
  pkill -9 apport 2>/dev/null || true
  sleep 1
}

start_server() {
  local conf="$1"
  echo "[start] Using config: $conf"
  stop_server
  # Run 30s then exit, to verify no crash
  local turbo_arg=""
  if [ "$MODE_NAME" != "standard" ]; then turbo_arg="--turbo"; fi
  timeout 30 turnserver -c "$conf" $turbo_arg 2>&1 | head -5 &
  local pid=$!
  sleep 4
  if kill -0 $pid 2>/dev/null; then
    echo "[start] Server started OK (PID=$pid)"
    if curl -sf http://localhost:8080/admin/status > /dev/null 2>&1; then
      echo "[start] Admin API OK"
      curl -sf http://localhost:8080/admin/status | python3 -m json.tool 2>/dev/null || true
    fi
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    return 0
  else
    echo "[start] Server crashed, check logs"
    return 1
  fi
}

build_and_install() {
  local configure_args="$1"

  echo "[build] Cleaning..."
  make distclean 2>/dev/null || make clean 2>/dev/null || true

  echo "[build] Configure: $configure_args"
  ./configure $configure_args 2>&1 | tail -5

  echo "[build] Compiling..."
  make -j$(nproc) 2>&1 | tail -5

  echo "[build] Installing..."
  sudo make install 2>&1 | tail -3
}

# Write mode-specific config to /tmp
write_config() {
  local mode="$1"
  local tmp_conf="/tmp/turnserver_test_${mode}.conf"

  cat > "$tmp_conf" << CONF
# coturn-turbo test: mode=$mode
listening-port=3478
listening-ip=0.0.0.0
relay-ip=10.0.2.174
external-ip=10.0.2.174
realm=mycoturn
lt-cred-mech
user=test:test123
verbose
simple-log
log-file=/var/log/turnserver/turbo.log
CONF

  case "$mode" in
    iouring)
      cat >> "$tmp_conf" << CONF
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
CONF
      ;;
    afxdp)
      cat >> "$tmp_conf" << CONF
turbo
turbo-backend=af_xdp
turbo-afxdp-mode=auto
turbo-xdp-iface=eth0
turbo-l1-warmup enable
turbo-api-port=8080
CONF
      ;;
    rooms)
      cat >> "$tmp_conf" << CONF
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
turbo-rooms
turbo-room-id-provider static
CONF
      ;;
  esac

  echo "$tmp_conf"
}

# ------------------------------------------------------------------
# Main flow
# ------------------------------------------------------------------

if [ "$RUN_ONLY" = true ]; then
  CONF_FILE=$(write_config "$MODE_NAME")
  sudo cp "$CONF_FILE" "$INSTALL_CONF"
  start_server "$CONF_FILE"
  exit $?
fi

echo "[build] Starting build & install for Mode=$MODE_NAME ..."

case "$MODE_NAME" in
  standard)
    build_and_install ""
    ;;
  iouring)
    build_and_install "--turbo"
    ;;
  afxdp)
    build_and_install "--turbo --turbo-backend=af_xdp"
    ;;
  rooms)
    build_and_install "--turbo --turbo-rooms"
    ;;
esac

# Install test config
CONF_FILE=$(write_config "$MODE_NAME")
sudo cp "$CONF_FILE" "$INSTALL_CONF"

echo ""
echo "[test] Startup verification..."
start_server "$CONF_FILE"
echo ""

if [ "$NO_TEST" = false ] && command -v node &>/dev/null; then
  echo "[test] Running WebRTC automated test..."
  TEST_DIR="$PROJECT_DIR/.claude/skills/webrtc-coturn-test-skill"
  if [ -d "$TEST_DIR" ] && [ -f "$TEST_DIR/relay-only-test.js" ]; then
    # Ensure TURN server is running (standard mode: no --turbo)
    sudo cp "$CONF_FILE" "$INSTALL_CONF"
    stop_server
    if [ "$MODE_NAME" = "standard" ]; then
      turnserver -c "$INSTALL_CONF" > /dev/null 2>&1 &
    else
      turnserver -c "$INSTALL_CONF" --turbo > /dev/null 2>&1 &
    fi
    TURN_PID=$!
    sleep 5

    cd "$TEST_DIR"
    node relay-only-test.js 2>&1
    TEST_EXIT=$?

    # Cleanup
    kill $TURN_PID 2>/dev/null || true
    cd "$PROJECT_DIR"

    echo ""
    if [ $TEST_EXIT -eq 0 ]; then
      echo "[test] WebRTC test PASSED"
    else
      echo "[test] WebRTC test FAILED (exit=$TEST_EXIT)"
    fi
    exit $TEST_EXIT
  else
    echo "[test] Skipping WebRTC test (test script not found)"
  fi
fi

echo "[test] Test complete"
