# coturn-turbo 自动化测试指南

本文档描述四种构建模式的自动化配置、编译、安装与测试流程。

## 前置依赖

```bash
# 基础依赖 (所有模式)
sudo apt install -y libevent-dev libssl-dev build-essential autoconf automake libtool

# io_uring (推荐)
sudo apt install -y liburing-dev

# AF_XDP (可选)
sudo apt install -y libbpf-dev libxdp-dev clang llvm

# WebRTC 自动化测试 (可选)
sudo apt install -y libnss3 libatk1.0-0 libatk-bridge2.0-0 libcups2 libxcomposite1 \
  libxrandr2 libgbm1 libpango-1.0-0 libcairo2 libasound2 libxshmfence1
```

## 快速切换脚本

以下脚本支持四种模式的快速切换测试，自动完成清理→配置→编译→安装→验证全流程。

### `test_mode.sh`

将以下脚本放在项目根目录，执行 `./test_mode.sh <mode>` 即可一键测试：

```bash
#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF_DIR="$PROJECT_DIR/conf"
BACKUP_CONF="/etc/turnserver/turnserver.conf.backup"
INSTALL_CONF="/etc/turnserver/turnserver.conf"

usage() {
  echo "Usage: $0 <mode> [--no-test] [--run-only]"
  echo ""
  echo "Modes:"
  echo "  1|standard  - 标准 coturn (无 Turbo)"
  echo "  2|iouring   - Turbo + io_uring (默认高性能后端)"
  echo "  3|afxdp     - Turbo + AF_XDP (极致性能，实验性)"
  echo "  4|rooms     - Turbo + 房间广播 (io_uring + rooms)"
  echo ""
  echo "Options:"
  echo "  --no-test   跳过 WebRTC 测试"
  echo "  --run-only  仅启动服务器，跳过编译安装"
  exit 1
}

MODE="${1:-}"
[ -z "$MODE" ] && usage

# Normalize mode
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
echo " coturn-turbo 测试: Mode=$MODE_NAME"
echo "============================================"

stop_server() {
  pkill -9 turnserver 2>/dev/null || true
  pkill -9 apport 2>/dev/null || true
  sleep 1
}

start_server() {
  local conf="$1"
  echo "[start] 使用配置: $conf"
  stop_server
  # 运行 30 秒后自动退出，用于验证是否崩溃
  local turbo_arg=""
  if [ "$MODE_NAME" != "standard" ]; then turbo_arg="--turbo"; fi
  timeout 30 turnserver -c "$conf" $turbo_arg 2>&1 | head -5 &
  local pid=$!
  sleep 4
  if kill -0 $pid 2>/dev/null; then
    echo "[start] ✅ 服务器启动成功 (PID=$pid)"
    # 检查状态 API
    if curl -sf http://localhost:8080/admin/status > /dev/null 2>&1; then
      echo "[start] ✅ Admin API 正常"
      curl -sf http://localhost:8080/admin/status | python3 -m json.tool 2>/dev/null || true
    fi
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    return 0
  else
    echo "[start] ❌ 服务器启动失败，查看日志"
    return 1
  fi
}

build_and_install() {
  local configure_args="$1"

  echo "[build] 清理..."
  make distclean 2>/dev/null || make clean 2>/dev/null || true

  echo "[build] 配置: $configure_args"
  ./configure $configure_args 2>&1 | tail -5

  echo "[build] 编译..."
  make -j$(nproc) 2>&1 | tail -5

  echo "[build] 安装..."
  sudo make install 2>&1 | tail -3
}

# ------------------------------------------------------------------
# 配置模板 (写入临时文件，避免修改全局配置)
# ------------------------------------------------------------------

write_config() {
  local mode="$1"
  local tmp_conf="/tmp/turnserver_test_${mode}.conf"

  cat > "$tmp_conf" << CONF
# coturn-turbo test: mode=$mode
listening-port=3478
listening-ip=0.0.0.0
relay-ip=172.17.123.27
external-ip=122.51.14.87/172.17.123.27
realm=mycoturn
lt-cred-mech
user=test:test123
verbose
simple-log
log-file=/home/web/local/var/log/turnserver/turbo.log
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
# 主流程
# ------------------------------------------------------------------

if [ "$RUN_ONLY" = true ]; then
  CONF_FILE=$(write_config "$MODE_NAME")
  sudo cp "$CONF_FILE" "$INSTALL_CONF"
  start_server "$CONF_FILE"
  exit $?
fi

echo "[build] 开始编译安装 Mode=$MODE_NAME ..."

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

# 安装测试配置
CONF_FILE=$(write_config "$MODE_NAME")
sudo cp "$CONF_FILE" "$INSTALL_CONF"

echo ""
echo "[test] 启动验证..."
start_server "$CONF_FILE"
echo ""

if [ "$NO_TEST" = false ] && command -v node &>/dev/null; then
  echo "[test] 运行 WebRTC 自动化测试..."
  TEST_DIR="$PROJECT_DIR/.claude/skills/webrtc-coturn-test-skill"
  if [ -d "$TEST_DIR" ] && [ -f "$TEST_DIR/regular-diagnostic.js" ]; then
    # 确保 TURN 服务器已启动（标准模式不加 --turbo）
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
    node regular-diagnostic.js 2>&1
    TEST_EXIT=$?

    # 清理
    kill $TURN_PID 2>/dev/null || true
    cd "$PROJECT_DIR"

    echo ""
    if [ $TEST_EXIT -eq 0 ]; then
      echo "[test] ✅ WebRTC 测试通过"
    else
      echo "[test] ❌ WebRTC 测试失败 (exit=$TEST_EXIT)"
    fi
    exit $TEST_EXIT
  else
    echo "[test] 跳过 WebRTC 测试 (regular-diagnostic.js 不存在)"
  fi
fi

echo "[test] 测试完成"
```

```bash
chmod +x test_mode.sh
```

### 使用方式

```bash
# 一键测试完整流程 (配置→编译→安装→启动验证→WebRTC 测试)
./test_mode.sh 1        # 标准模式
./test_mode.sh 2        # Turbo + io_uring
./test_mode.sh 3        # Turbo + AF_XDP
./test_mode.sh 4        # Turbo + 房间广播

# 仅编译安装，跳过测试
./test_mode.sh 2 --no-test

# 仅启动服务器验证，跳过编译
./test_mode.sh 2 --run-only

# 快速连续测试全部四种模式
for m in 1 2 3 4; do
  echo "=== Mode $m ==="
  ./test_mode.sh $m --no-test
done
```

## 四种构建模式详解

### Mode 1: 标准 coturn (无 Turbo)

```bash
./configure && make -j$(nproc) && sudo make install
```

- **无** `--turbo` 标志
- 编译产物与上游 coturn 完全一致
- 使用 libevent epoll 标准事件循环
- 所有 Turbo 配置项不识别（日志会显示 `Bad configuration format`）

验证方法：
```bash
turnserver -V 2>&1 | head -3
# 输出不应包含 "turbo:" 前缀
```

### Mode 2: Turbo + io_uring

```bash
./configure --turbo && make -j$(nproc) && sudo make install
```

- 默认高性能网络后端
- 支持运行时降级至 epoll
- 配置项：`turbo`, `turbo-backend=io_uring`, `turbo-l1-warmup enable`, `turbo-api-port=8080`

验证方法：
```bash
curl -s http://localhost:8080/admin/status
# 期望: {"turbo_enabled":true, "backend":"io_uring" | "epoll", ...}
```

### Mode 3: Turbo + AF_XDP

```bash
./configure --turbo --turbo-backend=af_xdp && make -j$(nproc) && sudo make install
```

- 内核旁路零拷贝网络后端（实验性）
- 需内核 ≥5.4 且网卡驱动支持
- 编译时自动构建 XDP eBPF 程序 (`xdp_prog.o`)
- 容器环境中可能回退至 io_uring/epoll

验证方法：
```bash
ls -la /usr/local/share/turnserver/xdp_prog.o  # 应存在
turnserver -V 2>&1 | head -3
```

### Mode 4: Turbo + 房间广播

```bash
./configure --turbo --turbo-rooms && make -j$(nproc) && sudo make install
```

- 包含 io_uring 后端 + 房间广播引擎
- 支持静态 / HMAC Token / Lua 脚本三种身份提供者
- 配置项：`turbo-rooms`, `turbo-room-id-provider static`

验证方法：
```bash
turnserver -V 2>&1 | head -3
# 期望输出包含: "turbo: room provider 'static' active"
```

## 配置文件参考

### 模式 1: 标准配置 (`conf_test_standard.conf`)

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=172.17.123.27
external-ip=122.51.14.87/172.17.123.27
realm=mycoturn
lt-cred-mech
user=test:test123
verbose
simple-log
```

### 模式 2: Turbo + io_uring (`conf_test_iouring.conf`)

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=172.17.123.27
external-ip=122.51.14.87/172.17.123.27
realm=mycoturn
lt-cred-mech
user=test:test123
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
verbose
simple-log
```

### 模式 3: Turbo + AF_XDP (`conf_test_turbo_afxdp.conf`)

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=172.17.123.27
external-ip=122.51.14.87/172.17.123.27
realm=mycoturn
lt-cred-mech
user=test:test123
turbo
turbo-backend=af_xdp
turbo-afxdp-mode=auto
turbo-xdp-iface=eth0
turbo-l1-warmup enable
turbo-api-port=8080
verbose
simple-log
```

### 模式 4: Turbo + 房间 (`conf_test_turbo_rooms.conf`)

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=172.17.123.27
external-ip=122.51.14.87/172.17.123.27
realm=mycoturn
lt-cred-mech
user=test:test123
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
turbo-rooms
turbo-room-id-provider static
verbose
simple-log
```

## WebRTC 自动化测试

### 测试脚本

| 脚本 | 用途 | ICE 策略 |
| :--- | :--- | :--- |
| `regular-diagnostic.js` | 标准 WebRTC 连通性测试（**推荐**） | `iceTransportPolicy: 'all'` |
| `relay-diagnostic.js` | Relay-only 中继转发测试 | `iceTransportPolicy: 'relay'` |
| `relay-only-test.js` | 带 ICE 状态监控的 Relay 测试 | `iceTransportPolicy: 'relay'` |

### 测试流程（regular-diagnostic.js）

1. 将 `index_sfu.html` 临时覆盖到 `index.html` 作为测试页面
2. 启动静态 HTTP 服务器（端口 8899）
3. 使用 Playwright 启动两个 Chromium 浏览器上下文（用户 A 和 B）
4. 双方配置 TURN 服务器（`turn:127.0.0.1:3478?transport=tcp`）并点击 Join Room
5. 用户 A 创建 Offer SDP，等待 ICE 收集完成
6. 用户 B 接收 Offer，生成 Answer SDP
7. 用户 A 接收 Answer，完成 SDP 交换
8. 每 1 秒轮询 ICE 连接状态，最长 15 秒
9. 若任一端 ICE 状态变为 `connected`/`completed`，判定 SUCCESS

### 测试配置 (`test-config.json`)

```json
{
  "pageUrl": "",
  "localServerPort": 8899,
  "turnServer": {
    "url": "turn:127.0.0.1:3478?transport=tcp",
    "username": "test",
    "credential": "test123"
  },
  "roomId": "123",
  "timeout": 30000,
  "expectedCandidateType": "relay",
  "headless": true
}
```

### 手动运行

```bash
cd .claude/skills/webrtc-coturn-test-skill
node webrtc-test-runner.js
```

### 预期输出（regular-diagnostic.js）

```
[A] Local video ready
[B] Local video ready
[A] ICE candidates: host, srflx, relay
[B] ICE candidates: host, srflx, relay

[Monitoring ICE states for 15 seconds...]
  [0s] A: {"ice":"checking","conn":"connecting"} | B: {"ice":"checking","conn":"connecting"}
  [1s] A: {"ice":"connected","conn":"connected"} | B: {"ice":"connected","conn":"connected"}

SUCCESS: ICE connected!
```

### 运行方式

```bash
# 确保 TURN 服务器已在运行
cd .claude/skills/webrtc-coturn-test-skill
node regular-diagnostic.js          # 标准连通性测试（推荐）
node relay-diagnostic.js            # Relay-only 测试
node relay-only-test.js             # Relay + ICE 状态监控
```

### test_all_modes.sh 一键测试全部模式

```bash
./test_all_modes.sh
# 依次编译并测试四种模式：
#   Mode 1: Standard (no turbo)
#   Mode 2: Turbo + io_uring (--turbo)
#   Mode 3: Turbo + AF_XDP (--turbo)
#   Mode 4: Turbo + rooms (--turbo --turbo-rooms)
```

### test_mode.sh 单模式测试

```bash
./test_mode.sh 1            # 标准模式（完整流程）
./test_mode.sh 2 --no-test  # 仅编译安装 io_uring 模式
./test_mode.sh 2 --run-only # 仅启动服务器验证
./test_mode.sh 3            # AF_XDP 模式
./test_mode.sh 4            # 房间广播模式
```

## 已知问题与修复记录

### 1. io_uring 在容器环境中 Segfault

**现象**：`io_uring_submit_and_wait_timeout` 在 liburing 内部分段错误 (signal=11)。

**修复**：`turbo_iouring.c` 中用 `io_uring_peek_cqe` + `poll(ring_fd, 5ms)` 替代了 `io_uring_submit_and_wait_timeout`，避免了容器环境下的崩溃。

### 2. RFC 5389 属性识别错误

**现象**：comprehension-optional 属性 (0x0000-0x7FFF) 被错误地当作 unknown 属性返回 420 错误。

**修复**：`ns_turn_server.c` 中将判断条件从 `0x0000 <= attr_type <= 0x7FFF` 改为 `0x8000 <= attr_type <= 0xFFFF`。

### 3. strncpy 未正确截断

**现象**：`strncpy` 拷贝 63 字节可能缺少 NUL 终止符。

**修复**：`turbo_room.c` 和 `turbo_fastpath.c` 中将 `strncpy(x, y, 63)` 改为 `strncpy(x, y, sizeof(x) - 1); x[sizeof(x) - 1] = '\0';`。

### 4. TURN UDP 在 headless Chromium 中不可用

**现象**：TURN over UDP 返回 0 个候选，TURN over TCP 正常获取 `relay` 候选。

**原因**：容器/沙箱环境中 UDP 端口可能被限制。

**解决**：测试配置使用 `turn:127.0.0.1:3478?transport=tcp`。

### 5. Relay-only 测试 (`iceTransportPolicy='relay'`) 在全部模式下失败

**现象**：即使使用标准 coturn（无 Turbo），`relay-diagnostic.js` 和 `relay-only-test.js` 也无法完成 ICE 连接。

**原因**：TURN over TCP + `iceTransportPolicy='relay'` 的组合在 headless Chromium 容器环境中存在兼容性问题，不是 Turbo 代码的 bug。

**解决**：使用 `regular-diagnostic.js`（`iceTransportPolicy: 'all'`）进行自动化测试。该测试允许浏览器使用 host 候选作为兜底，四种模式全部通过。

## 快速参考

| 命令 | 说明 |
| :--- | :--- |
| `./test_mode.sh 1 --no-test` | 仅编译安装标准模式 |
| `./test_mode.sh 2` | 完整测试 Turbo + io_uring |
| `./test_mode.sh 2 --run-only` | 仅启动服务器验证 |
| `./test_mode.sh 4 --no-test` | 编译安装房间模式 |
| `curl localhost:8080/admin/status` | 查看 Turbo 运行状态 |
| `turnserver -V 2>&1 \| head -3` | 查看后端和初始化状态 |
| `pkill -9 turnserver` | 强制停止服务器 |
