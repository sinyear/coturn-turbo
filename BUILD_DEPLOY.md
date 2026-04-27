# coturn-turbo 构建、打包、部署与测试指南

本文档描述 coturn-turbo 四种构建模式的完整生命周期：**标准模式**、**Turbo + io_uring**、**Turbo + AF_XDP**、**Turbo + 房间广播**。

---

## 目录

- [一、环境准备](#一环境准备)
- [二、四种模式构建](#二四种模式构建)
  - [2.1 标准模式](#21-标准模式)
  - [2.2 Turbo + io_uring 模式](#22-turbo--io_uring-模式)
  - [2.3 Turbo + AF_XDP 模式](#23-turbo--af_xdp-模式)
  - [2.4 Turbo + 房间广播模式](#24-turbo--房间广播模式)
- [三、打包](#三打包)
- [四、部署](#四部署)
  - [4.1 标准模式部署](#41-标准模式部署)
  - [4.2 Turbo + io_uring 模式部署](#42-turbo--io_uring-模式部署)
  - [4.3 Turbo + AF_XDP 模式部署](#43-turbo--af_xdp-模式部署)
- [五、运行](#五运行)
- [六、测试](#六测试)
- [七、故障排查](#七故障排查)
- [八、配置说明](#八配置说明)

---

## 一、环境准备

### 通用依赖

三种模式均需以下基础依赖：

```bash
# Ubuntu/Debian
apt-get install -y build-essential libssl-dev libevent-dev libsqlite3-dev pkg-config

# CentOS/RHEL/Fedora
yum install -y gcc gcc-c++ make openssl-devel libevent-devel libsqlite3-devel pkgconfig
```

### 各模式额外依赖

| 模式 | 额外依赖 | 内核要求 |
|------|----------|----------|
| 标准 | 无 | 任意 Linux |
| Turbo + io_uring | liburing-dev (≥2.0) | Linux ≥5.6 |
| Turbo + AF_XDP | libbpf-dev、libxdp-dev、libjson-c-dev、clang/llvm | Linux ≥5.4（推荐 ≥5.10）|
| Turbo + 房间 | 同 io_uring，可选 LuaJIT（Lua Provider）| 同 io_uring |

---

## 二、四种模式构建

### 2.1 标准模式

完全兼容原生 coturn，无 Turbo 扩展，适合不需要单端口收敛和房间广播的场景。

```bash
cd coturn-turbo

# 生成 Makefile
./configure

# 编译
make -j$(nproc)

# 安装（可选）
sudo make install
```

**构建产物**：
- `turnserver` — TURN/STUN 主服务
- `turnadmin` — 用户数据库管理工具
- `turnutils_*` — 测试工具集

### 2.2 Turbo + io_uring 模式

默认高性能模式，使用 io_uring 异步 I/O 后端。

```bash
cd coturn-turbo

# 生成 Makefile
./configure --turbo

# 编译
make -j$(nproc)

# 安装
sudo make install
```

**关键**：`--turbo` 启用 Turbo 模式，默认使用 io_uring 网络后端。需内核 ≥5.6。

### 2.3 Turbo + AF_XDP 模式

极致性能模式，使用 AF_XDP 内核旁路后端。

```bash
cd coturn-turbo

# 生成 Makefile
./configure --turbo --turbo-backend=af_xdp

# 编译
make -j$(nproc)

# 安装
sudo make install
```

**关键**：`--turbo-backend=af_xdp` 选择 AF_XDP 后端。需 libbpf、libxdp、clang。

### 2.4 Turbo + 房间广播模式

包含 io_uring 后端 + 房间广播引擎。

```bash
cd coturn-turbo

# 生成 Makefile
./configure --turbo --turbo-rooms

# 编译
make -j$(nproc)

# 安装
sudo make install
```

---

## 三、打包

### 3.1 标准模式打包

```bash
# 方式一：使用 make install 到临时目录
make install DESTDIR=/tmp/coturn-turnserver-pkg

# 方式二：手动打包构建产物
mkdir -p /tmp/coturn-turnserver-pkg/{bin,lib,etc,var/db}
cp bin/turnserver bin/turnadmin bin/turnutils_* /tmp/coturn-turnserver-pkg/bin/
cp examples/etc/turnserver.conf /tmp/coturn-turnserver-pkg/etc/
cp -r examples/scripts /tmp/coturn-turnserver-pkg/
cp sqlite/turndb /tmp/coturn-turnserver-pkg/var/db/ 2>/dev/null || true
```

### 3.2 Turbo 模式打包（io_uring / AF_XDP / rooms）

```bash
PKG_DIR=/tmp/coturn-turbo-pkg
mkdir -p ${PKG_DIR}/{bin,lib,etc,var/db,scripts}

# 复制二进制
cp bin/turnserver bin/turnadmin bin/turnutils_* ${PKG_DIR}/bin/

# 复制配置文件
cp examples/etc/turnserver.conf ${PKG_DIR}/etc/
cp conf/turbo.conf.example ${PKG_DIR}/etc/turbo.conf.example

# 复制数据库
cp sqlite/turndb ${PKG_DIR}/var/db/ 2>/dev/null || true

# 打包
cd /tmp && tar czf coturn-turbo-$(date +%Y%m%d).tar.gz coturn-turbo-pkg/
```

---

## 四、部署

### 4.1 标准模式部署

#### 4.1.1 手动部署

```bash
# 创建运行用户
sudo useradd -r -s /sbin/nologin turnserver

# 安装文件
sudo cp bin/turnserver /usr/bin/
sudo cp bin/turnadmin /usr/bin/
sudo cp bin/turnutils_* /usr/bin/
sudo mkdir -p /etc/turnserver /var/lib/turn
sudo cp examples/etc/turnserver.conf /etc/turnserver/turnserver.conf
sudo cp sqlite/turndb /var/lib/turn/turndb 2>/dev/null || true
sudo chown -R turnserver:turnserver /var/lib/turn /var/log/turnserver
```

#### 4.1.2 systemd 服务

使用项目自带的 service 文件：

```bash
sudo cp examples/etc/coturn.service /lib/systemd/system/coturn.service
sudo systemctl daemon-reload
sudo systemctl enable coturn
sudo systemctl start coturn
sudo systemctl status coturn
```

#### 4.1.3 端口要求

| 协议 | 端口 | 说明 |
|------|------|------|
| UDP | 3478 | TURN/STUN 主端口 |
| TCP | 3478 | TURN/STUN TCP |
| UDP | 49152-65535 | 中继端口范围（可配置） |
| TCP | 5349 | TLS（可选） |

```bash
# 开放防火墙
sudo ufw allow 3478/udp
sudo ufw allow 3478/tcp
sudo ufw allow 49152:65535/udp
```

### 4.2 Turbo + io_uring 模式部署

#### 4.2.1 配置文件

编辑 `/etc/turnserver/turnserver.conf`：

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=10.0.0.1
external-ip=122.51.14.87/10.0.0.1
realm=north
lt-cred-mech
user=claude:password

# Turbo 模式
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
```

#### 4.2.2 启动

```bash
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

#### 4.2.3 部署要求

| 维度 | 要求 |
|------|------|
| 内核 | Linux ≥5.6 |
| 运行依赖 | liburing |
| 权限 | 需要 root 或 CAP_NET_BIND_SERVICE（端口 3478） |

### 4.3 Turbo + AF_XDP 模式部署

### 4.3 Turbo + AF_XDP 模式部署

#### 4.3.1 内核检查

```bash
# 检查内核版本（需 ≥5.4）
uname -r

# 检查 XDP 支持
ethtool -k eth0 | grep xdp

# 检查 clang 是否可用（编译 XDP BPF 程序必需）
clang --version
```

#### 4.3.2 XDP 模式选择

AF_XDP 支持三种运行模式：

| 模式 | 说明 | 性能 | 兼容性 |
|------|------|------|--------|
| `auto` | 自动检测：先尝试 DRV 原生零拷贝，失败降级 SKB | 最优 | 最佳 |
| `drv` | XDP 驱动原生模式（真正的零拷贝） | 最高 | 需网卡驱动支持 |
| `skb` | SKB 内核回退模式（通过内核协议栈） | 中等 | 几乎所有 Linux ≥5.4 |

**重要**：无论哪种模式，代码都会自动加载 XDP 过滤程序（`xdp_prog.o`），该程序仅将 TURN 端口的 UDP 流量 redirect 到 AF_XDP socket，其他流量（SSH、HTTP 等）正常进入内核协议栈，**不会导致断网**。

配置方式：
```ini
# 在 turnserver.conf 中
turbo-afxdp-mode=auto    # 默认值
```

或通过命令行：
```bash
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo --turbo-afxdp-mode=auto -o -v
```

#### 4.3.3 配置文件

编辑 `/etc/turnserver/turnserver.conf`：

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=10.0.0.1
external-ip=122.51.14.87/10.0.0.1
realm=north
lt-cred-mech
user=claude:password

# Turbo 模式
turbo
turbo-backend=af_xdp
turbo-l1-warmup enable
turbo-api-port=8080

# AF_XDP 模式选择（auto/drv/skb，默认 auto）
turbo-afxdp-mode=auto
turbo-xdp-iface=eth0
```

#### 4.3.4 启动

```bash
# AF_XDP 模式需要 root 权限（加载 XDP BPF 程序）
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

#### 4.3.5 部署要求

| 维度 | 要求 |
|------|------|
| 内核 | Linux ≥5.4（推荐 ≥5.10） |
| 编译依赖 | clang（用于编译 `xdp_prog.c` → `xdp_prog.o`） |
| 运行依赖 | libbpf、libxdp |
| 网卡 | DRV 模式需驱动支持；SKB 模式兼容所有网卡 |
| 网卡独占 | **否** — XDP 过滤程序仅 redirect TURN 流量，其他服务不受影响 |
| 权限 | 需要 root 或 CAP_BPF + CAP_NET_ADMIN |

#### 4.3.6 云服务器注意事项

云服务器（如阿里云 ECS、腾讯云 CVM）的虚拟网卡（virtio/netvsc）通常**不支持原生 XDP（DRV 模式）**。使用 `auto` 模式会自动降级到 SKB 模式，无需手动配置。SKB 模式下：
- XDP 程序仍会正确加载并过滤流量
- SSH 和其他服务保持可达
- 性能略低于 DRV 模式，但仍高于纯内核协议栈处理

### 4.4 Turbo + 房间广播模式部署

编辑 `/etc/turnserver/turnserver.conf`：

```ini
# 基础 TURN 配置同上...

# Turbo + 房间
turbo
turbo-backend=io_uring
turbo-l1-warmup enable
turbo-api-port=8080
turbo-rooms
turbo-room-id-provider token_hmac
turbo-room-token-secret your_shared_secret
turbo-room-token-expiry 3600
```

启动：
```bash
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

---

## 五、运行

### 5.1 标准模式

```bash
# 前台运行（调试）
turnserver -c /etc/turnserver/turnserver.conf -v

# 后台运行
turnserver -c /etc/turnserver/turnserver.conf -o -v

# 使用 REST API 认证
turnserver -c /etc/turnserver/turnserver.conf -o \
    --use-auth-secret --static-auth-secret=mysecret
```

### 5.2 Turbo 模式（io_uring / AF_XDP / rooms）

> **警告**：`--turbo` 选项**仅在编译时启用了 Turbo 时可用**。标准模式（无 `--turbo` 编译）设置 `--turbo` 会导致启动失败。

```bash
# 前台运行（调试）
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -v

# 后台运行
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v

# 指定 API 端口
sudo turnserver -c /etc/turnserver/turnserver.conf \
    --turbo --turbo-api-port 8080 -o -v
```

### 5.3 验证运行状态

```bash
# 检查进程
ps aux | grep turnserver

# 检查端口
ss -ulnp | grep 3478

# 检查日志
tail -f /var/log/turnserver/turbo.log

# 检查 Turbo 状态（Turbo 模式）
curl -s http://localhost:8080/admin/status | python3 -m json.tool
```

---

## 六、测试

### 6.1 标准模式测试

#### 6.1.1 协议一致性测试

```bash
cd examples
./scripts/rfc5769.sh
```

#### 6.1.2 基本中继测试

```bash
# 终端 1: 启动服务器
cd examples
./scripts/basic/relay.sh

# 终端 2: 运行 UDP 客户端
cd examples
./scripts/basic/udp_c2c_client.sh
```

#### 6.1.3 完整测试套件

```bash
cd examples
./run_tests.sh
```

### 6.2 Turbo 模式测试

#### 6.2.1 Admin API 测试

```bash
# 查看状态
curl -s http://localhost:8080/admin/status | python3 -m json.tool

# 查看 Prometheus 指标
curl -s http://localhost:8080/admin/metrics

# 运行时降级到 epoll
curl -X POST http://localhost:8080/admin/turbo-disable

# 排空模式
curl -X POST http://localhost:8080/admin/drain
curl -s http://localhost:8080/admin/drain/status | python3 -m json.tool
```

#### 6.2.2 WebRTC 自动化测试

项目提供 `test_mode.sh` 和 `test_all_modes.sh` 脚本，可自动完成编译、安装、启动验证和 WebRTC 连通性测试。

```bash
# 一键测试全部四种模式
./test_all_modes.sh

# 单模式测试
./test_mode.sh 1            # 标准模式
./test_mode.sh 2            # Turbo + io_uring
./test_mode.sh 3            # Turbo + AF_XDP
./test_mode.sh 4            # Turbo + 房间广播

# 仅编译安装，跳过 WebRTC 测试
./test_mode.sh 2 --no-test

# 仅启动服务器验证
./test_mode.sh 2 --run-only
```

测试使用 Playwright 启动两个 Chromium 浏览器，通过 TURN 服务器建立 WebRTC 连接，验证 ICE 连通性。测试脚本位于 `.claude/skills/webrtc-coturn-test-skill/regular-diagnostic.js`。

#### 6.2.3 性能测试

```bash
# 监控 CPU 和内存
cd examples
./cpu-mem.sh

# 使用 turnutils_uclient 进行并发连接测试
bin/turnutils_uclient -n 100 -u test -w test123 -T 10 127.0.0.1
```

---

## 七、故障排查

### 7.1 标准模式

| 问题 | 解决方案 |
|------|----------|
| 端口被占用 | `ss -ulnp \| grep 3478`，关闭占用进程 |
| 认证失败 | 检查 `user=` 配置格式为 `username:password` |
| 日志无法写入 | 检查日志目录权限 |

### 7.2 Turbo + io_uring 模式

| 问题 | 解决方案 |
|------|----------|
| `io_uring_queue_init` 失败 | 检查内核版本 ≥5.6；检查 liburing 是否安装 |
| 启动崩溃（`--turbo`） | 确认编译时使用了 `--turbo`；通过 `ldd turnserver \| grep uring` 验证 |

### 7.3 Turbo + AF_XDP 模式

| 问题 | 解决方案 |
|------|----------|
| `interface 'xxx' not found` | 确认网卡名称正确：`ip link show` |
| `xsk_umem__create failed` | 内存不足或权限不够，确保 root 运行 |
| `xsk_socket__create_shared failed` | 网卡不支持 XDP，检查驱动：`ethtool -k <网卡> \| grep xdp`；某些虚拟化网卡（如 virtio）不支持原生 XDP，使用 `--turbo-afxdp-mode=skb` 强制 SKB 模式 |
| `xdp program not found` | XDP BPF 程序未编译。确保 clang 已安装，重新运行 `make`；或手动编译：`clang -O2 -target bpf -c src/turbo/netif/xdp_prog.c -o xdp_prog.o`，将结果放到 `/usr/local/share/turnserver/xdp_prog.o` |
| XDP 程序加载失败 | 安装 libbpf-dev、libxdp-dev；确认内核 ≥5.4 |
| SSH 或其他服务断连 | 此问题在新代码中已修复（自动加载 XDP 过滤程序）。如仍发生，确认 `xdp_prog.o` 已正确安装到 `/usr/local/share/turnserver/` 目录 |

**AF_XDP 详细排查步骤**：

```bash
# 1. 确认网卡存在且有 IP
ip addr show eth0

# 2. 检查驱动是否支持 XDP
ethtool -k eth0 | grep -i xdp

# 3. 确认 XDP 程序是否加载
ip link show eth0
# 应显示 "prog/xdp" 字样，表示 XDP 程序已附加

# 4. 手动测试 AF_XDP socket 创建
sudo ip link set dev eth0 xdp off  # 先清理残留
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -V 2>&1 | grep af_xdp

# 5. 如果仍失败，检查内核日志
dmesg | tail -20

# 6. 确认 libbpf/libxdp 链接正确
ldd turnserver | grep -E "bpf|xdp"
```

### 7.4 通用排查

```bash
# 查看最详细日志
turnserver -c /etc/turnserver/turnserver.conf --turbo -V

# 检查编译模式（通过启动日志判断）
turnserver -c /etc/turnserver/turnserver.conf --turbo -v 2>&1 | grep -i turbo

# 检查动态库依赖
ldd turnserver | grep -E "uring|bpf|xdp"
```

---

## 八、配置说明

### 8.1 Turbo 相关配置参数

| 参数 | 类型 | 默认值 | 命令行 | 配置文件 | 说明 |
|------|------|--------|--------|----------|------|
| `turbo` | bool | false | `--turbo` | `turbo` | 启用 Turbo 模式。仅在编译时使用 `--turbo` 时可设 |
| `turbo-backend` | string | "io_uring" | `--turbo-backend=` | `turbo-backend=io_uring` | 网络后端：io_uring（默认）或 af_xdp |
| `turbo-api-port` | uint16 | 0（禁用） | `--turbo-api-port <port>` | `turbo-api-port=<port>` | Turbo HTTP API 端口，提供管理接口 |
| `turbo-l1-warmup` | bool | enable | — | `turbo-l1-warmup enable` | 预热 L1 缓存 |
| `turbo-afxdp-mode` | string | "auto" | `--turbo-afxdp-mode <mode>` | `turbo-afxdp-mode=<mode>` | AF_XDP 模式：auto/drv/skb |
| `turbo-xdp-iface` | string | "" | — | `turbo-xdp-iface=<name>` | AF_XDP 绑定的网卡接口 |
| `turbo-rooms` | bool | false | `--turbo-rooms` | `turbo-rooms` | 启用房间广播 |
| `turbo-room-id-provider` | string | "static" | — | `turbo-room-id-provider=...` | Provider 类型：static/token_hmac/lua_script |

### 8.2 四种模式对比

| 特性 | 标准模式 | Turbo + io_uring | Turbo + AF_XDP | Turbo + 房间 |
|------|----------|-----------------|---------------|-------------|
| 编译标志 | 无 | `--turbo` | `--turbo --turbo-backend=af_xdp` | `--turbo --turbo-rooms` |
| 网络后端 | epoll (libevent) | io_uring | AF_XDP | io_uring |
| 端口模型 | 每会话分配端口 | 单端口复用 (3478) | 单端口复用 (3478) | 单端口复用 (3478) |
| SFU 广播 | 不支持 | 不支持 | 不支持 | 支持 |
| HTTP API | 无 | /admin/* | /admin/* | /admin/* |
| 内核要求 | 任意 | ≥5.6 | ≥5.4 | ≥5.6 |
| 部署复杂度 | 低 | 低 | 中 | 低 |

### 8.3 配置文件位置

| 文件 | 说明 |
|------|------|
| `examples/etc/turnserver.conf` | 完整参考配置，包含所有 coturn 原生选项 |
| `conf/turbo.conf.example` | Turbo 模式最小配置示例，可直接使用 |

---

## 附录：快速参考卡

### 标准模式一键启动

```bash
./configure && make -j$(nproc) && sudo make install
sudo cp examples/etc/turnserver.conf /etc/turnserver/turnserver.conf
sudo turnserver -c /etc/turnserver/turnserver.conf -o
```

### Turbo + io_uring 一键启动

```bash
# 1. 安装依赖
apt-get install -y liburing-dev

# 2. 编译
./configure --turbo
make -j$(nproc) && sudo make install

# 3. 启动
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo --turbo-api-port 8080 -o -v

# 4. 验证
curl -s http://localhost:8080/admin/status | python3 -m json.tool
```

### Turbo + AF_XDP 一键启动

```bash
# 1. 安装依赖（clang 用于编译 XDP BPF 程序）
apt-get install -y clang llvm libbpf-dev libxdp-dev libjson-c-dev

# 2. 编译（会自动编译 xdp_prog.o）
./configure --turbo --turbo-backend=af_xdp
make -j$(nproc) && sudo make install

# 3. 启动（auto 模式：自动选择 DRV 或降级 SKB，XDP 过滤程序自动加载）
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo --turbo-api-port 8080 -o -v

# 4. 验证：检查 XDP 程序是否已加载
ip link show eth0
# 应显示 "prog/xdp" 字样
```

### Turbo + 房间广播一键启动

```bash
# 1. 编译
./configure --turbo --turbo-rooms
make -j$(nproc) && sudo make install

# 2. 配置（turnserver.conf 中新增）
# turbo-rooms
# turbo-room-id-provider token_hmac
# turbo-room-token-secret your_shared_secret

# 3. 启动
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

### 自动化测试

```bash
# 一键测试全部四种模式
./test_all_modes.sh

# 单模式测试（支持 1|standard, 2|iouring, 3|afxdp, 4|rooms）
./test_mode.sh 2            # Turbo + io_uring
./test_mode.sh 2 --no-test  # 仅编译安装
./test_mode.sh 2 --run-only # 仅启动验证
```
