# coturn-turbo WebRTC SFU 中继方案汇总

> 本文档整理了从 2026-04-24 至 2026-04-25 期间讨论的所有 WebRTC SFU 中继加速方案，
> 包含背景分析、架构图、时序图、报文交互说明、代码修改清单及风险评估。

---

## 目录

- [1. 背景与问题定义](#1-背景与问题定义)
- [2. 当前架构分析](#2-当前架构分析)
  - [2.1 数据平面双路径现状](#21-数据平面双路径现状)
  - [2.2 根因分析](#22-根因分析)
- [3. 方案 0：修复现有架构](#3-方案-0修复现有架构)
  - [3.1 原理](#31-原理)
  - [3.2 架构图](#32-架构图)
  - [3.3 已完成的代码修改](#33-已完成的代码修改)
  - [3.4 仍需修改的部分](#34-仍需修改的部分)
  - [3.5 优点与缺点](#35-优点与缺点)
- [4. 方案 A：用户名路由 — 拦截 Allocate 响应](#4-方案-a用户名路由--拦截-allocate-响应)
  - [4.1 原理](#41-原理)
  - [4.2 架构图](#42-架构图)
  - [4.3 时序图](#43-时序图)
  - [4.4 报文交互](#44-报文交互)
  - [4.5 代码修改清单](#45-代码修改清单)
  - [4.6 优点与缺点](#46-优点与缺点)
- [5. 方案 B：外部信令 + Session Token 路由](#5-方案-b外部信令--session-token-路由)
  - [5.1 原理](#51-原理)
  - [5.2 架构图](#52-架构图)
  - [5.3 时序图](#53-时序图)
  - [5.4 流程图](#54-流程图)
  - [5.5 报文交互详解](#55-报文交互详解)
  - [5.6 关键技术细节](#56-关键技术细节)
  - [5.7 代码修改清单](#57-代码修改清单)
  - [5.8 优点与缺点](#58-优点与缺点)
- [6. 方案 C：全 UDP 捕获（XDP 全端口）](#6-方案-c全-udp-捕获xdp-全端口)
  - [6.1 原理](#61-原理)
  - [6.2 架构图](#62-架构图)
  - [6.3 代码修改清单](#63-代码修改清单)
  - [6.4 优点与缺点](#64-优点与缺点)
- [7. 方案 D：自定义 SDP 属性 + STUN 扩展（不可行方案）](#7-方案-d自定义-sdp-属性--stun-扩展不可行方案)
  - [7.1 原理](#71-原理)
  - [7.2 不可行原因](#72-不可行原因)
- [8. 方案对比矩阵](#8-方案对比矩阵)
- [9. 已完成的代码修改记录](#9-已完成的代码修改记录)
  - [9.1 诊断日志增强](#91-诊断日志增强)
  - [9.2 turbo_core 引擎启动修复](#92-turbo_core-引擎启动修复)
  - [9.3 CORS 跨域支持](#93-cors-跨域支持)
  - [9.4 测试页面](#94-测试页面)
- [10. 附录：测试页面说明](#10-附录测试页面说明)

---

## 1. 背景与问题定义

### 1.1 项目概述

coturn-turbo 是基于 coturn 4.10.0 扩展的高性能 TURN-SFU 融合服务器，通过集成 DPDK/AF_XDP 内核旁路技术、单端口复用及房间广播机制，将传统的一对一中继升级为面向多人会议的选择性转发（SFU）。

### 1.2 问题陈述

在使用 `index.html` 静态页面进行 WebRTC 音视频测试时，发现：

1. **ICE 候选返回的是随机中继端口**（如 `64192`），而非 Turbo 单端口
2. **媒体流量走了标准 coturn 中继路径**，而非 Turbo DPDK/AF_XDP 零拷贝路径
3. **Turbo poll 线程从未被调用** — 日志中无 FAST PATH HIT/MISS 输出

### 1.3 根因

```
AF_XDP 只捕获端口 3478 → 但 Allocate 返回中继端口 64192
→ 64192 不在 XDP 过滤范围内 → 走内核标准路径 → Turbo 从未被触发
```

具体原因有两个：
1. `turbo_core_init()` 和 `turbo_core_start()` **从未在 mainrelay.c 中被调用** — AF_XDP socket 虽然创建了，但没有 poll 线程读取
2. 从未调用 Turbo Room API — `ss->room_id = 0`，dtls_listener 的 fast path 也跳过

---

## 2. 当前架构分析

### 2.1 数据平面双路径现状

```
                    NIC (eth0)
                       │
            ┌──────────┼──────────┐
            │          │          │
      [XDP Program]  [Kernel]    │
      (port 3478)   Stack       │
            │          │        │
    AF_XDP Socket   标准 UDP    │
            │       Sockets    │
            │          │       │
    turbo_core_poll  libevent   │
    thread (未启动!) event loop │
            │          │       │
    ┌───────┤          ├──┐    │
    │       │          │  │    │
  HIT    MISS         room_id>0 room_id==0
    │       │          │    │
  broadcast 包丢弃    broadcast  full STUN/TURN
  to room             + continue relay processing
  members             processing
```

**关键发现**：AF_XDP 路径和标准 coturn 路径是**完全分离的两条流水线**。AF_XDP 捕获的包不会回退到标准路径，标准路径的包也不会进入 AF_XDP。

### 2.2 根因分析

| 组件 | 状态 | 说明 |
|------|------|------|
| `turbo_netif` (AF_XDP socket) | ✅ 已初始化 | `turbo_af_xdp.c` 正常创建 |
| `turbo_room_mgr` | ✅ 已初始化 | 房间管理器正常创建 |
| `turbo_api` (HTTP 服务) | ✅ 已启动 | 监听 `--turbo-api-port` |
| `turbo_core_engine` | ❌ **未初始化** | `turbo_core_init()` 和 `turbo_core_start()` 未调用 |
| XDP 过滤端口 | 3478 | 只捕获 3478 的流量 |
| Allocate 返回的中继端口 | 49152-65535 | 标准 coturn 随机分配 |

---

## 3. 方案 0：修复现有架构

### 3.1 原理

不改变现有的双路径架构，仅修复 bug + 完善 Turbo Room API 调用流程，使现有 SFU 广播机制正常工作。

### 3.2 架构图

```
[外部 Conductor/信令] ──HTTP──→ [Turbo API :9999]
  POST /v1/room/create                │
  POST /v1/room/join                  ├── 注册 turbo_room_mgr
  POST /v1/room/leave                 └── 注册 port_map
                                      │
[Browser A] ──STUN──→ [coturn:3478] ←─┘
  Allocate                            │
  → relay:64192                       │
  → ICE 候选: typ relay :64192        │
  → 媒体发到 64192                    │
  → 走标准 coturn 中继路径             │
  → dtls_listener: ss->room_id > 0    │
  → turbo_room_broadcast (二次加速)    │
```

**数据流**：
1. 媒体仍走标准 coturn 中继端口（如 64192）
2. 但如果客户端已通过 API 加入房间（`room_id > 0`），`dtls_listener.c` 中的 fast path 会触发 `turbo_room_broadcast`
3. 这是对现有架构的最小修复，**不是真正的单端口方案**

### 3.3 已完成的代码修改

#### 3.3.1 turbo_core 引擎启动（`mainrelay.c`）

```c
/* 在 Turbo 组件初始化流程中添加 */
turbo_core_engine = calloc(1, sizeof(struct turbo_core));
turbo_core_init(turbo_core_engine, turbo_netif, turbo_room_mgr);
turbo_core_start(turbo_core_engine);
```

#### 3.3.2 诊断日志增强

| 文件 | 修改内容 |
|------|---------|
| `turbo_core.c` | 添加 FAST PATH HIT/MISS 日志（含五元组详情）、每万包统计、线程启停日志 |
| `dtls_listener.c` | 记录 room_id=0 跳过、turbo 禁用原因 |
| `turbo_api.c` | join/leave 时自动注册 port_map |
| `turbo_af_xdp.c` | 启动时显示捕获端口和 XDP 模式 |
| `turbo_api.c` | 新增 `GET /v1/turbo/stats` 端点 |

#### 3.3.3 CORS 跨域支持

`turbo_api.c` 中所有响应自动添加 CORS 头，支持 OPTIONS 预检。

### 3.4 仍需修改的部分

| 文件 | 修改内容 | 说明 |
|------|---------|------|
| `ns_turn_server.c` ~line 4330 | Allocate 成功后注册 port_map | 需要将 `ss->client_addr` 和 `ss->client_port` 注册到 port_map |
| Conductor/信令集成 | 外部服务在客户端建立 TURN 连接后调用 `POST /v1/room/join` | 需要 session token ↔ TURN session 的关联机制 |

### 3.5 优点与缺点

| 优点 | 缺点 |
|------|------|
| 改动最小 | 不是真正的单端口方案 |
| 保留现有所有功能 | 媒体仍走标准 coturn 中继端口 |
| 易于验证 | AF_XDP poll 线程实际上接收不到媒体包 |
| | 二次加速（dtls_listener 层）效率不如 AF_XDP 层 |

---

## 4. 方案 A：用户名路由 — 拦截 Allocate 响应

### 4.1 原理

客户端在 TURN 用户名中编码 room/member 信息（如 `room123:user456`），coturn 在 Allocate 时解析该信息，跳过 relay socket 创建，强制中继地址使用固定端口（如 7878）。

### 4.2 架构图

```
┌─────────────────────────────────────────────────────────┐
│                                                          │
│  ┌──────────┐                                            │
│  │ Browser A│                                            │
│  │          │                                            │
│  │ PeerConn │                                            │
│  │ iceServers: [{                                       │
│  │   urls: "turn:122.51.14.87:3478",                    │
│  │   username: "room123:user456",  ← 房间信息在用户名中   │
│  │   credential: "password"                             │
│  │ }]                                                   │
│  └────┬─────┘                                            │
│       │ STUN Allocate (USERNAME="room123:user456")       │
│       ▼                                                   │
│  ┌───────────────────────┐                               │
│  │   coturn-turbo:3478   │                               │
│  │                       │                               │
│  │  1. 解析 username     │                               │
│  │     → room=123        │                               │
│  │     → member=456      │                               │
│  │  2. 跳过 relay socket │                               │
│  │  3. port_map 注册     │                               │
│  │     src_ip:port       │                               │
│  │     → room, member    │                               │
│  │  4. Allocate 响应     │                               │
│  │     relay=IP:7878     │                               │
│  └────────┬──────────────┘                               │
│           │ ICE 候选: relay IP:7878                      │
│           ▼                                               │
│  ┌───────────────────────┐                               │
│  │   XDP Filter          │                               │
│  │   捕获 UDP dst=7878   │                               │
│  └────────┬──────────────┘                               │
│           │                                               │
│  ┌────────▼──────────────┐                               │
│  │   AF_XDP Socket       │                               │
│  │   turbo_core poll     │                               │
│  │   port_map 查找: HIT  │                               │
│  │   → turbo_broadcast   │                               │
│  └───────────────────────┘                               │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 4.3 时序图

```
Browser A                        coturn-turbo
  │                                  │
  │ 1. PeerConnection 初始化          │
  │  iceServers:                     │
  │  username="room123:user456"      │
  │  credential="password"           │
  │                                  │
  │ 2. STUN Allocate Request         │
  │  USERNAME="room123:user456"      │
  │─────────────────────────────────►│
  │                                  │
  │ 3. coturn 处理:                  │
  │  a. 解析 username                │
  │     → room_id=123               │
  │     → member_id=456             │
  │  b. 跳过 create_relay_connection │
  │     (不分配随机端口)             │
  │  c. 注册 port_map:               │
  │     src(5.6.7.8:4000)           │
  │     → room=123, member=456      │
  │  d. Allocate 响应                │
  │     XOR-RELAYED-ADDRESS         │
  │     = 122.51.14.87:7878         │
  │                                  │
  │ 4. STUN Allocate Response       │
  │  relay_addr=122.51.14.87:7878   │
  │◄─────────────────────────────────│
  │                                  │
  │ 5. ICE 候选收集                  │
  │  a=candidate:xxx udp 122.51.14.87 7878 typ relay
  │                                  │
  │ 6. SDP 交换 (通过外部信令服务)     │
  │←──────────────SDP──────────────→│
  │                                  │
  │ 7. 媒体流发送到 7878             │
  │──RTP over UDP:7878─────────────►│
  │  XDP 捕获 → AF_XDP → port_map   │
  │  查找 HIT → turbo_broadcast     │
  │                                  │
```

### 4.4 报文交互

**STUN Allocate Request**：
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0| STUN Allocate Request      | Message Length               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic Cookie = 0x2112A442                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Transaction ID (12 bytes)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   USERNAME Attribute (Type=0x0006)                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Length = 18   | Padding     | "room123:user456\0\0"           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   NONCE | REALM | MESSAGE-INTEGRITY | FINGERPRINT             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

网络层: UDP src=5.6.7.8:4000, dst=122.51.14.87:3478
```

**STUN Allocate Response**（Turbo 模式修改后）：
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 1| STUN Allocate Response     | Message Length               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic Cookie = 0x2112A442                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Transaction ID (12 bytes)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   LIFETIME = 3600                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   XOR-RELAYED-ADDRESS                                           |
| Family=0x01, Port=XOR(7878), Addr=XOR(122.51.14.87)           |
| ← 固定端口 7878，而非随机分配                                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   MESSAGE-INTEGRITY | FINGERPRINT                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

网络层: UDP src=122.51.14.87:3478, dst=5.6.7.8:4000
```

### 4.5 代码修改清单

| # | 文件 | 修改内容 | 复杂度 |
|---|------|---------|--------|
| 1 | `src/server/ns_turn_server.c` ~line 4330 (`create_relay_connection`) | Turbo 模式下：跳过 relay socket 创建和端口分配 | 中 |
| 2 | `src/server/ns_turn_server.c` ~line 1415 (Allocate 响应构造) | Turbo 模式下：`relay_addr.port = TURBO_RELAY_PORT` (如 7878) | 小 |
| 3 | `src/server/ns_turn_server.c` ~line 900 (Allocate 处理入口) | 解析 username 格式 `roomXXX:userYYY`，注册 port_map | 中 |
| 4 | `src/turbo/network/xdp_prog.c` | XDP 过滤端口改为 `TURBO_RELAY_PORT` (7878) | 极小 |
| 5 | `src/apps/relay/mainrelay.c` | 新增 `--turbo-relay-port` 参数 (默认 7878) | 小 |
| 6 | `src/server/ns_turn_server.c` (ChannelBind 处理) | Turbo 模式下 ChannelBind 返回空操作或忽略 | 小 |

### 4.6 优点与缺点

| 优点 | 缺点 |
|------|------|
| 不需要外部信令服务 | 用户名格式约定（客户端需遵循 `roomID:memberID`） |
| 不需要 fork 浏览器 | 安全性低：客户端可伪造任意 roomID |
| 纯服务端改动 | 信道映射 (ChannelBind) 需要特殊处理 |
| 改动最小，集中在 Allocate 路径 | 多租户隔离依赖用户名格式 |
| 真正的单端口方案 | |

---

## 5. 方案 B：外部信令 + Session Token 路由

### 5.1 原理

通过独立的外部信令服务（WebSocket/HTTP）管理房间和成员，信令服务调用 coturn-turbo API 生成带 HMAC 签名的 session token，客户端在 TURN Allocate 时使用该 token 作为用户名。coturn 验证 token 后注册 port_map，返回固定中继端口。

### 5.2 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         网络边界                                    │
│                                                                     │
│  ┌──────────┐         ┌──────────────────────┐                     │
│  │ Browser A│         │   外部信令服务        │                     │
│  │          │◄──WS──►│   (WebSocket/HTTP)    │                     │
│  │          │         │   • 房间管理          │                     │
│  │  WebRTC  │         │   • 信令交换          │                     │
│  │  ICE/TURN│◄──SDP──►│   • Token 生成        │                     │
│  │          │         └───────┬──────────────┘                     │
│  └────┬─────┘                 │ HTTP POST /v1/token/bind           │
│       │ UDP (STUN+RTP)        ▼                                    │
│       │ 到 7878         ┌──────────────────────┐                   │
│       └───────────────►│   coturn-turbo        │                   │
│                        │   ┌────────────────┐  │                   │
│           ┌───────────►│   │ XDP Filter     │  │                   │
│           │            │   │ 捕获 UDP 7878  │  │                   │
│           │            │   └───────┬────────┘  │                   │
│           │            │           │            │                   │
│           │            │   ┌───────▼────────┐  │                   │
│           │            │   │ AF_XDP Socket   │  │                   │
│           │            │   └───────┬────────┘  │                   │
│           │            │           │            │                   │
│           │            │   ┌───────▼────────┐  │                   │
│           │            │   │ turbo_core     │  │                   │
│           │            │   │ port_map 查找  │  │                   │
│           │            │   └───────┬────────┘  │                   │
│           │            │           │            │                   │
│           │            │   ┌───────▼────────┐  │                   │
│           │            │   │ turbo_room     │  │                   │
│           │            │   │ broadcast 零拷贝│  │                   │
│           │            │   └────────────────┘  │                   │
│           │            └───────────────────────┘                   │
│           │                                                       │
│           │ (仅媒体流量走 7878，信令走独立 WS)                       │
└───────────┼───────────────────────────────────────────────────────┘
            │
            ▼
    ┌───────────────────┐
    │  Browser B        │
    │   • 同样走上述流程  │
    │   • room_id 相同   │
    └───────────────────┘
```

### 5.3 时序图

```
Browser A                    外部信令服务                 coturn-turbo
  │                               │                          │
  │  1. 加入房间请求              │                          │
  │─────────────────────────────►│                          │
  │  POST /join {room:"room123"} │                          │
  │                               │  2. 生成 session token   │
  │                               │  ────────────────────►   │
  │                               │  POST /v1/token/bind     │
  │                               │  {room:123, member:1}    │
  │                               │                          │
  │                               │  3. 返回 token           │
  │                               │◄─────────────────────    │
  │                               │  {token:"tok_abc123",    │
  │                               │   relay:"122.51.14.87:7878"}│
  │                               │                          │
  │  4. 返回 TURN 配置            │                          │
  │◄─────────────────────────────│                          │
  │  {turnServer:"turn:122.51.14.87:3478",                   │
  │   username:"tok_abc123",     │                          │
  │   password:"<hmac签名>"}      │                          │
  │                               │                          │
  │  5. ICE 候选收集              │                          │
  │  PeerConnection.iceServers = │                          │
  │    [{urls:"turn:...",        │                          │
  │      username:"tok_abc123",  │                          │
  │      credential:"<hmac>"}]   │                          │
  │                               │                          │
  │  6. STUN Allocate            │                          │
  │  ── STUN Allocate Request ──►│                          │
  │     USERNAME="tok_abc123"    │                          │
  │                              │                          │
  │  7. coturn 验证 token         │
  │  8. 注册 port_map            │
  │     五元组(5.6.7.8:4000→3478)│
  │     → room=123, member=1     │
  │  9. Allocate 响应             │
  │     XOR-RELAYED-ADDRESS      │
  │     = 122.51.14.87:7878      │
  │                              │                          │
  │  10. ICE relay 候选          │                          │
  │◄── a=candidate:xxx udp 122.51.14.87 7878 typ relay     │
  │                               │                          │
  │  11. SDP 交换 (通过信令服务)    │                          │
  │─────────────────────────────►│ SDP Offer                 │
  │                               │─────────────SDP Offer ──►│
  │                               │                          │
  │                               │◄────────────SDP Answer ──│
  │◄─────────────────────────────│ SDP Answer                │
  │                               │                          │
  │  12. 媒体流发送到 7878        │                          │
  │  ── RTP over UDP:7878 ──────►│ XDP 捕获                  │
  │                               │ AF_XDP 零拷贝接收         │
  │                               │ port_map 查找: HIT        │
  │                               │ room=123 → broadcast      │
  │                               │─────────────RTP─────────►│ Browser B
```

### 5.4 流程图

```
                          coturn-turbo 主流程
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │  启动: 初始化 AF_XDP     │
                    │  固定中继端口 = 7878     │
                    │  XDP 过滤: UDP dst=7878  │
                    │  port_map (空哈希表)     │
                    │  token_table (空哈希表)  │
                    └────────────┬────────────┘
                                 │
          ┌──────────────────────┼──────────────────────┐
          ▼                      ▼                      ▼
  ┌───────────────┐    ┌─────────────────┐    ┌──────────────┐
  │ API 线程       │    │ STUN/TURN 线程  │    │ AF_XDP 线程  │
  │ :9999         │    │ :3478           │    │ (poll loop)  │
  └───────┬───────┘    └───────┬─────────┘    └──────┬───────┘
          │                    │                      │
          ▼                    ▼                      ▼
  ┌───────────────┐    ┌─────────────────┐    ┌──────────────┐
  │POST /v1/token │    │Allocate Request │    │  RX Burst    │
  │/bind          │    │解析 USERNAME    │    │(从 AF_XDP)   │
  └───────┬───────┘    └───────┬─────────┘    └──────┬───────┘
          │                    │                      │
          ▼                    ▼                      ▼
  ┌───────────────┐    ┌─────────────────┐    ┌──────────────┐
  │生成 session   │    │Turbo 模式:      │    │提取五元组    │
  │记录:          │    │1. 验证 token    │    │src=5.6.7.8:P │
  │ token→room,   │    │2. 查 token 表  │    │dst=122.x:7878│
  │ member        │    │3. 获取room/memb│    └──────┬───────┘
  │返回 token     │    └───────┬─────────┘           │
  └───────────────┘           │                      ▼
                              │              ┌──────────────┐
                              ▼              │port_map 查找 │
  ┌──────────────────────────────────────────┤(src→room,   │
  │              数据平面                     │ member)     │
  │                                          └──────┬───────┘
  │         ┌─────────────────┐                     │
  │    ┌────┤ HIT             ├────┐                │
  │    │    │(客户端已加入房间)│    │                ▼
  │    │    └─────────────────┘    │         ┌────────────┐
  │    ▼                           ▼         │  MISS       │
  │  ┌───────────────────────────────────┐   │(未加入房间  │
  │  │ turbo_room_broadcast              │   │ 或非媒体包)│
  │  │ 1. 遍历 room 成员                  │   └─────┬──────┘
  │  │ 2. 对每个成员:                     │         │
  │  │    - 克隆包头(零拷贝)              │         ▼
  │  │    - 重写目的IP/端口               │  ┌──────────────┐
  │  │    - tx_burst 发送                │  │回退标准TURN   │
  │  │    - refcount--                   │  │或丢弃         │
  │  └───────────────────────────────────┘  └──────────────┘
  │
  └─────────────────────────────────────
```

### 5.5 报文交互详解

#### 阶段 1：Token 绑定

**浏览器 → 信令服务**
```http
POST /join HTTP/1.1
Host: signaling.example.com
Content-Type: application/json

{
  "room": "room123",
  "userId": "user456"
}
```

**信令服务 → coturn-turbo API**
```http
POST /v1/token/bind HTTP/1.1
Host: 122.51.14.87:9999
Content-Type: application/json

{
  "room_id": 123,
  "member_id": 456
}
```

**coturn-turbo API → 信令服务**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "token": "tok_789abc",
  "relay_ip": "122.51.14.87",
  "relay_port": 7878
}
```

#### 阶段 2：TURN 配置下发

**信令服务 → 浏览器**
```json
{
  "turn": {
    "urls": "turn:122.51.14.87:3478",
    "username": "tok_789abc",
    "credential": "hmac_sig_here"
  },
  "relay_candidate": "122.51.14.87:7878"
}
```

#### 阶段 3：STUN Allocate 交互

**Allocate Request**：
```
STUN Header: Allocate Request (0x0003)
Magic Cookie: 0x2112A442
Transaction ID: 12 bytes

Attributes:
  USERNAME: "tok_789abc"
  NONCE: (服务器返回的随机数)
  REALM: "north"
  MESSAGE-INTEGRITY: HMAC-SHA1(USERNAME + NONCE + ...)
  FINGERPRINT: CRC32

网络层: UDP src=5.6.7.8:4000, dst=122.51.14.87:3478
```

**Allocate Response**：
```
STUN Header: Allocate Response (0x0113)
Magic Cookie: 0x2112A442
Transaction ID: (与 Request 相同)

Attributes:
  XOR-MAPPED-ADDRESS: 5.6.7.8:4000
  LIFETIME: 3600
  XOR-RELAYED-ADDRESS: 122.51.14.87:7878  ← 固定端口
  MESSAGE-INTEGRITY: HMAC-SHA1
  FINGERPRINT: CRC32

网络层: UDP src=122.51.14.87:3478, dst=5.6.7.8:4000
```

#### 阶段 4：媒体流（RTP over UDP:7878）

```
以太网: src=MAC_Browser, dst=MAC_Server, EtherType=0x0800
IPv4:   src=5.6.7.8, dst=122.51.14.87, proto=UDP(17)
UDP:    src=Browser端口(如 5000), dst=7878
Payload: RTP 数据 (SRTP 加密)

XDP 层处理:
  ┌─────────────────────────────────────┐
  │ XDP Program (xdp_prog.c)            │
  │ 检查: dst_port == 7878?             │
  │   YES → XDP_REDIRECT → AF_XDP socket│
  │   NO  → XDP_PASS (SSH/HTTP等)       │
  └─────────────────────────────────────┘
```

### 5.6 关键技术细节

#### 5.6.1 端口匹配问题

Allocate 时浏览器用的源端口（如 4000）和实际发 RTP 时的源端口（如 5000）可能不同。

**解决方案：port_map 的键使用 `src_ip` 而非精确五元组**

```c
/* port_map 条目 */
struct port_map_entry {
    uint32_t src_ip;        /* 客户端源 IP */
    uint32_t room_id;
    uint32_t member_id;
    uint16_t src_port_hint; /* Allocate 时的端口（提示用） */
    time_t expire;
};

/* 查找时:
 * 1. 提取 dst_port，确认是 Turbo 端口 (7878)
 * 2. 提取 src_ip 查 port_map
 * 3. 找到 → room_id, member_id
 */
```

#### 5.6.2 Token 存储和验证

```c
struct turn_token_entry {
    char token[64];          /* "tok_789abc" */
    char hmac_sig[64];       /* HMAC 签名 */
    uint32_t room_id;
    uint32_t member_id;
    uint32_t client_ip;      /* Allocate 时填充 */
    uint16_t client_port;    /* Allocate 时填充 */
    time_t expire;
};
```

**HMAC 签名生成（Token Bind 时）**：
```
hmac = HMAC-SHA256(
    key: server_secret_key,
    data: "tok_789abc:123:456:timestamp"
)
credential = base64(hmac)
```

**HMAC 验证（Allocate 时）**：
```
coturn 收到 username="tok_789abc", credential=<hmac>
→ 查 token 表获取 room_id, member_id, secret
→ 重新计算 HMAC 验证 credential
→ 验证通过 → 继续处理
```

#### 5.6.3 Nginx 反向代理（生产环境）

```
外部信令服务和 coturn-turbo API 通过 Nginx 统一暴露:

                    Nginx (:443 TLS)
                    ├── /ws        → 信令服务 (WebSocket)
                    ├── /api/token → coturn-turbo API:9999
                    └── /turn      → coturn-turbo:3478 (TCP/UDP)

浏览器连接:
  wss://example.com/ws         → 信令
  https://example.com/api/token → Token 绑定
  turn:example.com:3478         → TURN (UDP)
```

### 5.7 代码修改清单

| # | 文件 | 修改内容 | 复杂度 |
|---|------|---------|--------|
| 1 | `src/apps/relay/turbo_api.c` | 新增 `POST /v1/token/bind` 端点，生成 token 并写入 token 表 | 中 |
| 2 | `src/server/ns_turn_server.c` ~line 4330 | Turbo 模式下 `create_relay_connection` 跳过 socket 创建 | 中 |
| 3 | `src/server/ns_turn_server.c` ~line 1415 | Allocate 响应中 `XOR-RELAYED-ADDRESS.port` 强制为 `TURBO_RELAY_PORT` | 小 |
| 4 | `src/server/ns_turn_server.c` ~line 900 | Allocate 处理中提取 username，查 token 表，注册 port_map | 中 |
| 5 | `src/turbo/network/xdp_prog.c` | XDP 过滤端口改为 `TURBO_RELAY_PORT` (7878) | 极小 |
| 6 | `src/apps/relay/turbo_core.c` | port_map 改为按 `src_ip` 匹配（而非精确五元组） | 中 |
| 7 | `src/apps/relay/mainrelay.c` | 新增 `--turbo-relay-port` 命令行参数 (默认 7878) | 小 |

### 5.8 优点与缺点

| 优点 | 缺点 |
|------|------|
| 安全性高（HMAC 签名，不可伪造） | 需要外部信令服务配合 |
| 不破坏现有架构（扩展而非替换） | 部署复杂度增加（信令 + coturn 协调） |
| 支持多租户 | 需要 token 生命周期管理 |
| 与现有 SFU room API 兼容 | 端口匹配需特殊处理 |
| 真正的单端口方案 | |

---

## 6. 方案 C：全 UDP 捕获（XDP 全端口）

### 6.1 原理

修改 XDP 过滤器，不再按端口过滤，而是捕获**所有 UDP 流量**到 AF_XDP socket。在 `turbo_core_process_packet` 中区分处理：
- port_map HIT → turbo_room_broadcast（零拷贝转发）
- port_map MISS + STUN 报文 → 回退到标准 coturn 处理
- port_map MISS + 非 STUN 报文 → 丢弃

### 6.2 架构图

```
                    NIC (eth0)
                       │
              ┌────────┴────────┐
              │                 │
        [XDP Program]      [Kernel Stack]
        捕获所有 UDP        TCP 流量(SSH/HTTP)
              │                 │
              ▼                 │
      AF_XDP Socket             │
              │                 │
    ┌─────────┴─────────┐       │
    │                   │       │
    ▼                   ▼       │
turbo_core          信令回退     │
process_packet      机制        │
    │                   │       │
    ├── HIT             │       │
    │   │               │       │
    │   ▼               │       │
    │ broadcast ────────┘       │
    │                           │
    └── MISS                    │
        ├── STUN → 回送内核     │
        │          (raw socket) │
        └── 非STUN → 丢弃       │
```

### 6.3 代码修改清单

| # | 文件 | 修改内容 | 复杂度 |
|---|------|---------|--------|
| 1 | `src/turbo/network/xdp_prog.c` | 去掉端口过滤，所有 UDP 走 `XDP_REDIRECT` | 小 |
| 2 | `src/apps/relay/turbo_core.c` | MISS 路径增加 STUN 检测 + 回退到内核 | 大 |
| 3 | `src/apps/relay/mainrelay.c` | 创建 raw socket 用于回送 MISS 包到内核 | 中 |
| 4 | `src/turbo/network/turbo_af_xdp.c` | 可能需要调整 UMEM 配置以适应全 UDP 流量 | 小 |

### 6.4 优点与缺点

| 优点 | 缺点 |
|------|------|
| 所有 UDP 流量都走 AF_XDP | 信令回退机制复杂（raw socket 性能差） |
| 无需修改 Allocate 逻辑 | 影响所有 UDP 流量（DNS、NTP、QUIC） |
| 理论上可以捕获一切 | SSH 虽不受影响，但 DNS 解析可能延迟 |
| | 安全面扩大（全 UDP 暴露） |

---

## 7. 方案 D：自定义 SDP 属性 + STUN 扩展（不可行方案）

### 7.1 原理

在 WebRTC SDP 中添加自定义属性携带身份标识，然后扩展 STUN 协议将该标识传递给 coturn。

```
SDP:
  a=reuse-port-id:room123:user456

期望通过 STUN Allocate 传递:
  STUN Attribute: CUSTOM_ID = "room123:user456"
```

### 7.2 不可行原因

| 层级 | 隔离问题 |
|------|---------|
| SDP 交换 | WebSocket/HTTP，浏览器 ↔ 信令服务 |
| ICE/TURN | STUN over UDP/TCP，浏览器 ↔ coturn:3478 |
| 媒体 | RTP over UDP，浏览器 ↔ 中继端口 |

**SDP 和 TURN 是完全隔离的两条通道：**

1. 浏览器在 ICE 候选收集中发送的 STUN Allocate 请求**不包含任何 SDP 内容**
2. 标准 STUN 报文格式是固定的（Header + Attributes），浏览器 WebRTC 栈内部实现
3. 无法在标准浏览器中注入自定义 STUN attribute
4. 即使 fork libwebrtc 注入自定义 attribute，也意味着只有自行编译的客户端能用，失去通用性

**结论：此方案不可行，已排除。**

---

## 8. 方案对比矩阵

| 维度 | 方案 0: 修复现有 | 方案 A: 用户名路由 | 方案 B: Token 路由 | 方案 C: 全 UDP |
|------|-----------------|-------------------|-------------------|---------------|
| **单端口** | ❌ 仍用随机端口 | ✅ 固定端口 | ✅ 固定端口 | ✅ 固定端口 |
| **AF_XDP 捕获媒体** | ❌ 不捕获 | ✅ 捕获 | ✅ 捕获 | ✅ 捕获 |
| **外部信令** | 需要（Room API） | 不需要 | **需要** | 不需要 |
| **客户端改动** | 无 | username 格式约定 | 从信令获取配置 | 无 |
| **安全性** | 中 | 低（可伪造 roomID） | **高（HMAC）** | 低 |
| **代码改动量** | 小 | 中 | 中 | 大 |
| **信令回退机制** | 不需要 | 不需要 | 不需要 | **需要** |
| **兼容标准浏览器** | ✅ | ✅ | ✅ | ✅ |
| **部署复杂度** | 低 | 低 | 中 | 高 |
| **适用场景** | 验证现有架构 | 内部测试/受信任客户端 | **生产环境/多租户** | 不推荐 |

### 推荐路径

```
阶段 1（验证）: 方案 0 → 确认 Turbo 引擎能启动、诊断日志正常
阶段 2（功能）: 方案 A → 最小改动跑通单端口 AF_XDP 转发
阶段 3（生产）: 方案 B → 叠加 Token 认证，满足安全要求
```

---

## 9. 已完成的代码修改记录

### 9.1 诊断日志增强

#### `src/apps/relay/turbo_core.c`
- poll 线程启动/停止日志增加 burst 和 interval 参数
- 每 10000 包输出一次统计摘要（rx/tx/drop/hit/miss）
- `turbo_core_process_packet()` FAST PATH HIT 日志（含五元组、room_id、member_id、广播数量、包长度）
- FAST PATH MISS 日志（前 100 次详细记录，之后每 1000 次记录一次，含五元组信息）

#### `src/apps/relay/dtls_listener.c`
- turbo 已启用但 `room_id=0` 时记录跳过原因
- turbo 未启用或 `room_mgr/session` 为空时记录诊断信息

#### `src/apps/relay/turbo_api.c`
- `POST /v1/room/join` 时自动注册 port_map（记录 room_id、member_id、IP、端口）
- `POST /v1/room/leave` 时记录注销事件
- 新增 `GET /v1/turbo/stats` 端点（返回 rx/tx/drop/hit/miss、组件状态）

#### `src/turbo/network/turbo_af_xdp.c`
- 启动时显示：接口名、ifindex、XDP 模式、捕获端口
- XDP 程序加载状态、port_map 填充确认

### 9.2 turbo_core 引擎启动修复

#### `src/apps/relay/mainrelay.h`
- 添加 `#include "turbo_core.h"`（正确路径）
- 添加 `extern struct turbo_core *turbo_core_engine;`

#### `src/apps/relay/mainrelay.c`
- 添加全局变量定义 `struct turbo_core *turbo_core_engine = NULL;`
- 在 Turbo 组件初始化流程中添加：
  ```c
  turbo_core_engine = calloc(1, sizeof(struct turbo_core));
  turbo_core_init(turbo_core_engine, turbo_netif, turbo_room_mgr);
  turbo_core_start(turbo_core_engine);
  ```
- 在清理流程中添加：
  ```c
  if (turbo_core_engine) {
      turbo_core_cleanup(turbo_core_engine);
      free(turbo_core_engine);
  }
  ```

### 9.3 CORS 跨域支持

#### `src/apps/relay/turbo_api.c`
- `send_json_response()` 自动添加 CORS 头：
  - `Access-Control-Allow-Origin: *`
  - `Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS`
  - `Access-Control-Allow-Headers: Content-Type`
- 新增 `turbo_api_cors_preflight()` 处理 OPTIONS 预检
- 所有 6 个 API 端点（create、join、leave、info、list、stats）入口增加 OPTIONS 检查

### 9.4 测试页面

#### `index.html` — 原始页面（未修改）
- 手动 SDP 交换的 P2P WebRTC 测试页
- 支持 Offer/Answer 交换、ICE 候选显示

#### `index_sfu.html` — 新增 SFU 测试页面
- **Tab 1: WebRTC 中继测试**
  - ICE 传输策略选择（relay/all）
  - relay 候选实时计数
  - 中继状态指示器
- **Tab 2: Turbo Room API**
  - 健康检查、创建房间、房间列表、房间信息、加入/离开
  - 自动轮询房间状态
  - 成员表格展示
  - **Turbo Engine 统计面板**（RX/TX/丢弃/命中/未命中/状态）

---

## 10. 附录：测试页面说明

### 文件位置
- `index.html` — 原始测试页面（保留）
- `index_sfu.html` — SFU 增强测试页面（新增）

### 使用方式

1. 用浏览器打开 `index_sfu.html`
2. **Tab 1** 进行 WebRTC 中继测试
3. **Tab 2** 调用 Turbo Room API
4. 配置 TURN 服务器地址、用户名、密码
5. 观察日志中的 ICE 候选类型和 relay 候选数

### 预期行为

| 模式 | ICE relay 候选端口 | 日志关键字 |
|------|-------------------|-----------|
| 标准 coturn | 49152-65535 随机 | 无 turbo 相关日志 |
| Turbo (方案 0) | 49152-65535 随机 | `room_id > 0, fast path broadcast` |
| Turbo (方案 A/B) | **7878 固定** | `FAST PATH HIT` |
| Turbo 但未加入房间 | 7878 固定 | `FAST PATH MISS` |
