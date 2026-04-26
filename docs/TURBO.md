# coturn-turbo 功能说明与运维手册

> **版本**: v2.0  
> **适用**: coturn-turbo 分支  
> **前提**: 本文档假设您已熟悉 coturn 基本配置和 TURN 协议基础。

## 目录

1.  [特性概览](#1-特性概览)
2.  [快速入门](#2-快速入门)
3.  [核心概念](#3-核心概念)
4.  [配置参考](#4-配置参考)
5.  [网络后端选择与调优](#5-网络后端选择与调优)
6.  [房间广播功能](#6-房间广播功能)
7.  [监控与可观测性](#7-监控与可观测性)
8.  [运行时管理](#8-运行时管理)
9.  [运维手册](#9-运维手册)
10. [故障排查](#10-故障排查)
11. [与原生 coturn 的行为差异](#11-与原生-coturn-的行为差异)
12. [常见问题](#12-常见问题)

---

## 1. 特性概览

coturn-turbo 在保持与上游 coturn **100% 协议兼容**和**客户端零改动**的前提下，提供了以下增强：

| 特性 | 说明 | 状态 |
| :--- | :--- | :--- |
| **单端口收敛** | 所有 TURN 流量复用 UDP 3478，消除端口爆炸 | 稳定 |
| **io_uring 高性能后端** | 减少系统调用开销，性能提升约 8% | 稳定 |
| **AF_XDP 极致性能后端** | 内核旁路，性能提升最高 48% | 实验性 |
| **自适应快速路径** | 三级查找引擎，保证转发延迟 ≤ 原生 | 稳定 |
| **流量整形** | 信令/媒体流量隔离，防 DDoS 放大 | 稳定 |
| **动态热降级** | 运行时从 AF_XDP → io_uring → epoll 无中断降级 | 稳定 |
| **轻量房间广播** | 信令驱动、服务器端媒体复制，节省上行带宽 | 可选 |
| **可插拔身份识别** | 支持 static / HMAC Token / Lua 脚本与现有信令集成 | 可选 |
| **Prometheus 指标** | Per-allocation 粒度监控 | 稳定 |
| **Admin HTTP API** | 状态查询、降级控制、排空下线 | 稳定 |

---

## 2. 快速入门

### 2.1 安装依赖

```bash
# 基础依赖 (与 coturn 相同)
sudo apt update
sudo apt install -y libevent-dev libssl-dev build-essential autoconf automake libtool

# io_uring 支持 (推荐)
sudo apt install -y liburing-dev

# AF_XDP 支持 (可选)
sudo apt install -y libbpf-dev libxdp-dev clang llvm
```

### 2.2 编译

```bash
git clone https://github.com/your-org/coturn-turbo.git
cd coturn-turbo
autoreconf -fi

# 标准编译 (不带 Turbo)
./configure && make -j$(nproc)

# Turbo + io_uring (推荐生产)
./configure --turbo && make -j$(nproc)

# Turbo + AF_XDP
./configure --turbo --turbo-backend=afxdp && make -j$(nproc)

# Turbo + 房间广播
./configure --turbo --turbo-rooms && make -j$(nproc)
```

### 2.3 最小配置

创建 `/etc/turnserver.conf`：

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=<YOUR_PUBLIC_IP>
realm=my.realm
lt-cred-mech
user=test:test123

# Turbo 启用
turbo
turbo-backend=io_uring
turbo-api-port=8080
verbose
log-file=/var/log/turnserver/turbo.log
```

### 2.4 启动与验证

```bash
sudo turnserver -c /etc/turnserver.conf --turbo

# 验证 Turbo 是否生效
curl http://localhost:8080/admin/status
# 预期输出中包含 "turbo_enabled": true
```

---

## 3. 核心概念

### 3.1 单端口收敛

传统 coturn 为每个 allocation 分配独立的临时 UDP 端口。coturn-turbo 将所有流量复用到 **一个固定端口（3478）**。

- **优点**：防火墙规则简化、端口资源无限、安全审计集中。
- **实现**：Allocate 响应中 `XOR-RELAYED-ADDRESS` 始终返回 `3478`；数据包通过五元组 (src_ip, src_port, dst_ip, dst_port, protocol) 和信道绑定快速定位 allocation，无需多个 socket。

### 3.2 自适应快速路径

收包后经过三级查找：

1.  **信道绑定表**：`(src_ip + dst_ip + channel_number)` → 直接定位（用于 ChannelData）。
2.  **L1 快速缓存**：`(src_ip + src_port)` → 直接定位（用于 Send Indication，预热后命中率 >99.9%）。
3.  **辅助解析表**：通过 STUN USERNAME 解析，仅在 L1 未命中时触发，触发即警告。

### 3.3 网络后端与降级

- **epoll**：兜底后端，行为与原生 coturn 完全一致。
- **io_uring**：默认高性能后端，批量收发包，减少系统调用。
- **AF_XDP**：用户态驱动，零拷贝，极致性能，但依赖内核和网卡驱动。

降级路径：`AF_XDP → io_uring → epoll`，可通过 SIGUSR1、Admin API 触发，或由内置保险丝自动触发（AF_XDP 队列拥塞时），降级过程 **不会中断现有通话**。

### 3.4 房间广播（可选）

信令服务在下发 ICE 配置时，在 TURN `username` 字段携带一个 Token（如 JWT 格式）。coturn-turbo 通过可插拔的 **Room Identity Provider** 解析 Token，提取 `room_id` 和 `member_id`，自动将该 allocation 加入对应房间的成员表。

当房间内某个成员发布媒体包时，coturn-turbo 将包零拷贝克隆后，转发给房间内其他所有成员。信令服务仍是房间状态的唯一 Owner，coturn-turbo 是无状态的媒体转发节点。

---

## 4. 配置参考

### 4.1 完整配置示例

```ini
# ========== 基础 TURN ==========
listening-port=3478
listening-ip=0.0.0.0
relay-ip=122.51.14.87
realm=north.example.com
lt-cred-mech
user=legacy_user:legacy_password
# cert=/etc/turn/cert.pem
# pkey=/etc/turn/key.pem

# ========== Turbo 核心 ==========
turbo
turbo-backend=io_uring               # io_uring | af_xdp (默认 io_uring)
turbo-l1-warmup enable               # 启用 L1 缓存预热 (强烈推荐)
turbo-api-port=8080                  # Admin API 监听端口
# turbo-api-listen-ip=127.0.0.1      # 限制 API 仅本地访问

# ========== 流量整形 ==========
# turbo-stun-pps-limit 2000          # STUN 包每秒限制 (默认 2000)
# turbo-stun-burst-limit 4000        # STUN 突发容忍 (默认 4000)

# ========== AF_XDP 后端 (当 turbo-backend=af_xdp 时) ==========
# turbo-backend=af_xdp              # io_uring (默认) | af_xdp
# turbo-xdp-iface=eth0              # 绑定网卡

# ========== 房间广播 (可选) ==========
# turbo-rooms
# turbo-room-id-provider token_hmac # static | token_hmac | lua_script
# turbo-room-token-secret <密钥>    # 与信令服务共享
# turbo-room-token-expiry 3600      # Token 有效期 (秒)

# ========== 审计 ==========
# turbo-audit-log unix:///var/run/turn-audit.sock  # 审计事件流

# ========== 日志 ==========
verbose
log-file=/var/log/turnserver/turbo.log
```

### 4.2 参数详细说明

| 参数 | 类型 | 默认 | 说明 |
| :--- | :--- | :--- | :--- |
| `turbo` | flag | off | 启用 Turbo 功能 |
| `turbo-backend` | string | io_uring | 网络后端：`io_uring`, `af_xdp` |
| `turbo-l1-warmup` | flag | disable | 启用 L1 快速缓存预热（推荐） |
| `turbo-api-port` | int | 8080 | Admin API HTTP 端口 |
| `turbo-api-listen-ip` | string | 0.0.0.0 | Admin API 监听 IP |
| `turbo-stun-pps-limit` | int | 2000 | STUN 包速率限制 |
| `turbo-stun-burst-limit` | int | 4000 | STUN 突发容忍 |
| `turbo-backend` | string | io_uring | 网络后端：`io_uring`, `af_xdp` |
| `turbo-xdp-iface` | string | (无) | AF_XDP 绑定网卡 |
| `turbo-rooms` | flag | off | 启用房间广播功能 |
| `turbo-room-id-provider` | string | (无) | 房间身份提供者：`static`, `token_hmac`, `lua_script` |
| `turbo-room-token-secret` | string | (无) | HMAC 共享密钥 |
| `turbo-room-token-expiry` | int | 3600 | Token 有效期（秒） |
| `turbo-room-lua-script` | string | (无) | Lua 脚本路径（provider 为 lua_script 时） |
| `turbo-audit-log` | string | (无) | 审计日志 Unix Socket 路径 |

---

## 5. 网络后端选择与调优

### 5.1 选择建议

| 场景 | 推荐后端 | 理由 |
| :--- | :--- | :--- |
| 通用生产 | `io_uring` | 性能提升显著，兼容性最好，可降级 |
| 极致性能需求 | `af_xdp` | 单机并发 > 10Gbps 时考虑 |
| 保守 / 内核 <5.6 | `epoll` (不加 `--turbo`) | 与原生一致 |
| 开发测试 | `io_uring` | 易于部署和调试 |

### 5.2 AF_XDP 调优

```bash
# 1. 检查网卡驱动是否支持原生 XDP
ethtool -i eth0 | grep driver

# 2. 调整 ring buffer 大小
sudo ethtool -G eth0 rx 4096 tx 4096

# 3. 调整内核参数
sudo sysctl -w net.core.rmem_max=2147483647
sudo sysctl -w net.core.rmem_default=524288
```

如遇兼容性问题，可不指定 `turbo-backend`（默认使用 io_uring），或在启动时设置 `TURBO_AFXDP_MODE=skb` 环境变量回退到 SKB 通用模式。

### 5.3 保险丝与降级

AF_XDP 后端内置监控：连续 3 秒 RX Ring 满则自动降级到 io_uring，并记录 `turbo_afxdp_degraded_total` 指标。

手动触发降级：
```bash
# Admin API
curl -X POST http://localhost:8080/admin/turbo-disable

# 或信号
sudo kill -SIGUSR1 $(pidof turnserver)
```

---

## 6. 房间广播功能

### 6.1 启用步骤

1. **编译**：`./configure --turbo --turbo-rooms && make`
2. **配置**：
```ini
turbo-rooms
turbo-room-id-provider token_hmac
turbo-room-token-secret <与信令服务相同的密钥>
turbo-room-token-expiry 3600
```
3. **信令服务**生成 Token 并下发给客户端：
```javascript
// 示例 (Node.js)
const crypto = require('crypto');
function generateToken(roomId, memberId, secret, ttl = 3600) {
  const header = { alg: "HS256", typ: "JWT-like" };
  const body = { room_id: roomId, member_id: memberId,
                 exp: Math.floor(Date.now()/1000) + ttl };
  const hb64 = Buffer.from(JSON.stringify(header)).toString('base64url');
  const bb64 = Buffer.from(JSON.stringify(body)).toString('base64url');
  const sig = crypto.createHmac('sha256', secret).update(`${hb64}.${bb64}`).digest('base64url');
  return `${hb64}.${bb64}.${sig}`;
}
// 客户端 ICE 配置
iceServers: [{
  urls: "turn:server:3478",
  username: generateToken("room123", "userA", SHARED_SECRET),
  credential: "dummy"
}]
```

4. **客户端无需任何修改**。

### 6.2 内置 Provider

| Provider | 配置值 | 说明 |
| :--- | :--- | :--- |
| 静态解析 | `static` | 直接解析 `username`，格式 `room<ID>:user<ID>` |
| HMAC Token | `token_hmac` | 用共享密钥验证签名，防篡改 |
| Lua 脚本 | `lua_script` | 调用自定义 Lua 函数，可对接 Redis/HTTP 等 |

详见 [TURBO_ROOM_PROVIDER.md](./TURBO_ROOM_PROVIDER.md)。

### 6.3 限制与注意

- **房间上限**：每个房间最多 50 人（硬编码），超过拒绝加入。
- **成员上限**：默认 50，可在源码 `turbo_room.h` 中调整。
- **信令依赖**：房间创建、成员加入离开等操作必须由信令服务通过重新下发/撤销 Token 控制。
- **无 SFU 优化**：仅做简单克隆转发，不进行 Simulcast/SVC 层选择。

---

## 7. 监控与可观测性

### 7.1 Prometheus 指标

Admin API 提供 `/admin/metrics` 端点，返回 Prometheus 格式数据。关键指标：

| 指标名 | 类型 | 描述 |
| :--- | :--- | :--- |
| `turbo_allocations_current` | Gauge | 当前活跃 allocation 数 |
| `turbo_fastpath_hits_total` | Counter | 快速路径命中次数（按类型分） |
| `turbo_l1_miss_total` | Counter | L1 缓存未命中次数 |
| `turbo_stun_overload_drops_total` | Counter | STUN 限速丢弃包数 |
| `turbo_forwarded_packets_total` | Counter | 转发包总数（按用户名分） |
| `turbo_room_members_current` | Gauge | 各房间当前成员数 |
| `turbo_backend` | Gauge | 当前后端类型 (1=AF_XDP, 2=io_uring, 3=epoll) |
| `turbo_degrade_total` | Counter | 总降级次数 |

### 7.2 日志与审计

- **常规日志**：由 `log-file` 参数指定，包含 Turbo 事件（降级、Provider 验证失败等）。
- **审计流**：若配置 `turbo-audit-log`，每个转发包会以 JSON 格式写入 Unix Socket，供安全分析。

### 7.3 Admin API 端点

| 端点 | 方法 | 说明 |
| :--- | :--- | :--- |
| `/admin/metrics` | GET | Prometheus 指标 |
| `/admin/status` | GET | 运行时状态 JSON |
| `/admin/turbo-disable` | POST | 立即降级到 epoll |
| `/admin/turbo-enable` | POST | 重新启用 Turbo (需重启或手动切换) |
| `/admin/drain` | POST | 进入排空模式（拒绝新 alloc） |
| `/admin/drain/status` | GET | 排空进度 |

---

## 8. 运行时管理

### 8.1 状态查询

```bash
curl http://localhost:8080/admin/status
```

响应示例：
```json
{
  "turbo_enabled": true,
  "backend": "io_uring",
  "backend_health": "ok",
  "allocations": 432,
  "rooms": 5,
  "total_room_members": 34,
  "fastpath_hit_rate": 99.98,
  "uptime_seconds": 86400
}
```

### 8.2 动态降级

当遇到异常性能或想回退到安全模式时：
```bash
curl -X POST http://localhost:8080/admin/turbo-disable
```
降级后，`turbo_backend` 指标变为 3 (epoll)，所有通话不受影响。

### 8.3 节点排空与下线

1. 通知信令服务停止向该节点分配新用户。
2. 触发排空：
   ```bash
   curl -X POST http://localhost:8080/admin/drain
   ```
3. 等待所有 allocation 自然结束（可通过 `/admin/drain/status` 监控）。
4. 停止进程：
   ```bash
   sudo systemctl stop coturn-turbo
   ```

---

## 9. 运维手册

### 9.1 灰度上线策略

1. **编译并部署** coturn-turbo 到新节点。
2. 在信令服务侧，按一定比例将用户分配到 turbo 节点（通过 ICE 候选列表控制）。
3. 观察指标：`fastpath_hit_rate` 应 >99.9%，`turbo_l1_miss_total` 应平稳不上涨。
4. 逐步扩大流量，直至全量。
5. 始终保留原生 coturn 节点作为 fallback。

### 9.2 紧急回滚 SOP

| 步骤 | 操作 | 影响 |
| :--- | :--- | :--- |
| 1 | 信令层移除该节点 ICE 候选 | 新用户不再连接 |
| 2 | `kill -SIGUSR1 $(pidof turnserver)` | 1 秒内降级到 epoll，通话不断 |
| 3 | 验证 `turbo_backend` 变为 3 | 降级成功 |
| 4 | 排空后替换为旧版二进制 | 无感知 |

### 9.3 性能基线

基于 10Gbps 网络，单路 1.5Mbps 视频流的实验室数据：

| 后端 | 并发路数 | CPU 使用率 | 单包延迟 P50 |
| :--- | :--- | :--- | :--- |
| epoll (原生) | ~2000 | 72% | 180 µs |
| io_uring | ~2400 | 65% | 145 µs |
| AF_XDP | ~3400 | 48% | 80 µs |

---

## 10. 故障排查

| 现象 | 可能原因 | 检查方法 |
| :--- | :--- | :--- |
| Turbo 未生效 | 未加 `--turbo` 启动 | 查看 `/admin/status`，`turbo_enabled` 应为 true |
| `fastpath_hit_rate` 低 | 客户端大量使用 Send Indication 且 L1 未命中 | 开启 `turbo-l1-warmup`，检查网络是否频繁换端口 |
| AF_XDP 启动失败 | 网卡驱动不支持 | 查看日志，切换至默认 `turbo-backend=io_uring` 或设 `TURBO_AFXDP_MODE=skb` |
| 房间广播无效 | Provider 配置错误或 Token 验证失败 | 查看日志，验证 Token 生成逻辑 |
| L1 未命中告警 | 客户端更换了源端口 | 检查 NAT 行为，可能需调整 L1 缓存策略 |

启用调试日志：
```bash
CTURBO_DEBUG=1 turnserver -c /etc/turnserver.conf --turbo
```

---

## 11. 与原生 coturn 的行为差异

| 行为 | 原生 coturn | coturn-turbo |
| :--- | :--- | :--- |
| Relay 地址端口 | 每次 allocation 不同 | 固定 3478 |
| 端口占用 | N 个 allocation 占用 N+1 个端口 | 始终 1 个端口 |
| 数据包处理 | 内核 UDP socket 栈 | 用户态快速路径（可回退） |
| 默认 I/O | epoll + sendto/recvfrom | io_uring（性能更好） |
| 多人会话 | 无内置支持 | 可选房间广播 |
| 认证 | long-term cred / REST | 同左，扩展支持 Room Token |
| 配置文件 | 完全兼容 | 完全兼容，新增 `turbo*` 参数 |
| 降级能力 | 无 | 支持运行时热降级 |

---

## 12. 常见问题

**Q：启用 Turbo 后是否需要修改防火墙规则？**  
A：只需开放 UDP 3478 一个端口，比原生 coturn 更简单。

**Q：单端口收敛是否影响 NAT 穿透？**  
A：不影响，TURN 协议本身就支持一个端口多个 allocation，WebRTC 的 ICE 处理与端口无关。

**Q：如何确认客户端是否使用了房间广播？**  
A：客户端无感知，但在服务器日志或指标中可以看到 `room_id` 和 `member_id`。

**Q：io_uring 后端是否稳定？**  
A：生产验证稳定，内核 5.6 以上即可。如遇问题可随时热降级到 epoll。

**Q：AF_XDP 需要独占网卡吗？**  
A：不需要，XDP 程序仅将 TURN 端口流量 redirect 到 AF_XDP socket，其他流量正常走内核协议栈。

**Q：房间广播的 Token 被客户端篡改怎么办？**  
A：使用 `token_hmac` provider，签名验证失败会直接返回 401 拒绝分配。