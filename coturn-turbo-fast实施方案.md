# coturn-turbo 生产就绪方案 (v2.0 Final - 完整版)

> **摘要**：本方案在完全兼容标准 TURN 协议的前提下，对 coturn 进行务实增强。核心实现**单端口收敛**、**高性能 I/O 动态降级**以及可选的**信令驱动的轻量房间广播 (SFU)**。通过创新的 **Room Identity Provider** 可插拔设计，实现了与现有信令系统的松耦合集成，同时保持对开源社区友好。
>
> **文档状态**：最终版  
> **审批**：待最终评审批准

---

## 目录

1.  [项目背景与目标](#1-项目背景与目标)
2.  [设计哲学与约束](#2-设计哲学与约束)
3.  [总体架构](#3-总体架构)
4.  [核心设计一：单端口收敛与自适应快速路径](#4-核心设计一单端口收敛与自适应快速路径)
5.  [核心设计二：网络后端抽象与运行时动态降级](#5-核心设计二网络后端抽象与运行时动态降级)
6.  [核心设计三：信令驱动的轻量房间广播](#6-核心设计三信令驱动的轻量房间广播)
7.  [核心设计四：Room Identity Provider 可插拔机制](#7-核心设计四room-identity-provider-可插拔机制)
8.  [安全与审计设计](#8-安全与审计设计)
9.  [客户端兼容性与 SLO](#9-客户端兼容性与-slo)
10. [运维、监控与灰度回滚](#10-运维监控与灰度回滚)
11. [代码结构与上游合并策略](#11-代码结构与上游合并策略)
12. [构建、配置与部署](#12-构建配置与部署)
13. [与原生 coturn 行为对照表](#13-与原生-coturn-行为对照表)
14. [实施路线图](#14-实施路线图)
15. [附录](#15-附录)

---

## 1. 项目背景与目标

### 1.1 核心痛点

原始 coturn 作为标准 TURN 服务器，在实际大规模生产部署中暴露出以下问题：

| 痛点 | 描述 | 影响 |
| :--- | :--- | :--- |
| **端口爆炸** | 每个 TURN allocation 独占一个 UDP 端口 | 数万并发时端口管理困难，防火墙规则膨胀，安全审计复杂 |
| **性能瓶颈** | 依赖内核协议栈 (`epoll` + `sendto`/`recvfrom`)，每包系统调用和数据拷贝 | CPU 多消耗在内核-用户态切换，单机并发受限（~2000 路） |
| **缺乏广播语义** | 无原生房间概念，多人会话需上层自行组合多个 1v1 链路 | 上行带宽 N 倍浪费，端到端延迟增加 |
| **社区兼容性** | 性能优化常以破坏主线兼容性为代价 | 难以持续跟踪上游 Bug 修复和新特性 |
| **运维复杂性** | 端口不固定导致监控和流审计困难 | 故障定位慢，缺乏 per-allocation 粒度的可观测性 |

### 1.2 项目目标

在 **100% 兼容标准 TURN 协议 (RFC 8656)、不强制客户端升级、不破坏 coturn 主线代码结构** 的前提下：

1.  **端口收敛**：所有媒体与信令流量复用到 **1 个 UDP 端口**（3478），消除端口爆炸问题。
2.  **高性能与不退化**：提供可选的 `io_uring` / `AF_XDP` 高性能后端，并确保最差情况下（降级至 epoll）性能与原版完全持平。
3.  **松耦合的房间广播**：在现有信令服务的驱动下，通过可插拔机制实现服务器端媒体广播，客户端零改动。
4.  **运维友好**：支持运行时热降级、无中断灰度回滚、per-allocation 粒度的 Prometheus 监控。
5.  **开源可持续**：所有新增代码模块化、可配置，通过 `#ifdef` 宏隔离，不依赖任何特定外部服务，易于社区集成和向上游贡献。

---

## 2. 设计哲学与约束

### 2.1 核心设计原则

| 原则 | 说明 | 体现 |
| :--- | :--- | :--- |
| **兼容优先** | 任何优化不得破坏标准协议行为 | 客户端无改动即可使用全部特性 |
| **机制与策略分离** | 框架提供机制（高性能转发、房间广播框架），具体策略（如何识别房间）由配置/插件决定 | Room Identity Provider 可插拔 |
| **无状态媒体节点** | coturn-turbo 不持有业务状态，业务状态全部由外部信令服务管理 | 节点可随时水平扩缩容，无状态同步负担 |
| **渐进增强** | 高级特性均为可选，默认行为与原版完全一致 | `--turbo` 编译选项控制；运行时亦可降级 |
| **可逆性** | 任何变更必须支持无损回退 | 运行时降级 + Drain 模式 |

### 2.2 已验证的假设与应对

| # | 假设 | 结论 | 设计决策 |
| :--- | :--- | :--- | :--- |
| 1 | 多数客户端支持 ChannelBinding | 部分成立 | 双路径设计：优先 ChannelBind 快速查找，Send Indication 降级路径保证延迟不劣化 |
| 2 | 服务端改造无需客户端升级 | 成立 | 所有优化纯服务端完成，房间功能所需的 Token 仅作为标准 TURN username 传递 |
| 3 | 1v1 场景为主 | 部分成立 | 默认保持传统 TURN 中继，房间广播作为可选附加模块 |
| 4 | 性能要求不弱于原生 | 成立 | io_uring 作为默认后端，epoll 降级保证性能下限 |
| 5 | 网络后端需可替换且可降级 | 成立 | 抽象为统一接口，支持三后端及运行时切换 |
| 6 | 身份认证应尽量不变 | 部分成立 | 保留 long-term credential 和 REST 鉴权，扩展支持 Token 模式 |
| 7 | 必须可合并上游 | 成立 | 所有新代码集中在 `src/turbo/`，通过 `#ifdef TURBO_FEATURES` 隔离 |

---

## 3. 总体架构

### 3.1 架构图

```text
                         ┌─────────────────────┐
                         │     客户端           │
                         │  (标准 WebRTC 栈)    │
                         │  - Chrome/Firefox   │
                         │  - Safari/Android   │
                         │  - iOS              │
                         └──────────┬──────────┘
                                    │  UDP :3478 (STUN/TURN + 媒体)
                                    ▼
┌───────────────────────────────────────────────────────────────────┐
│                        coturn-turbo 节点                           │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                   TURN 协议层 (tunalloc)                      │ │
│  │  - 标准 Allocation / Permission 管理 (RFC 8656)              │ │
│  │  - 扩展字段: fast_path_flags, room_id, audit_handle           │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                                │                                   │
│  ┌────────────────────────────▼────────────────────────────────┐ │
│  │            Turbo 快速路径 (src/turbo/forward)                │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │  自适应查找引擎                                       │  │ │
│  │  │  - 信道绑定表 (src + channel_no → alloc)              │  │ │
│  │  │  - L1 快速缓存 (src_ip + src_port → alloc)           │  │ │
│  │  │  - 辅助解析表 (username hash → alloc)                │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │  流量整形器                                           │  │ │
│  │  │  - STUN 队列: PPS ≤ 2000，超限丢弃 + 告警            │  │ │
│  │  │  - 媒体队列: 不受限，优先转发                        │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │  轻量房间广播引擎 (可选)                              │  │ │
│  │  │  - RCU 链表成员表，读操作无锁                         │  │ │
│  │  │  - 零拷贝克隆 + 批量发送                              │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  │  ┌──────────────────────────────────────────────────────┐  │ │
│  │  │  审计钩子                                             │  │ │
│  │  │  - 每包/采样记录 (allocation_id, username, length)   │  │ │
│  │  └──────────────────────────────────────────────────────┘  │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                                │                                   │
│  ┌────────────────────────────▼────────────────────────────────┐ │
│  │         网络后端抽象层 (src/turbo/netif)                     │ │
│  │                                                              │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │ │
│  │  │    epoll     │  │   io_uring   │  │   AF_XDP     │      │ │
│  │  │  (保底降级)  │  │  (默认后端)  │  │  (最高性能)  │      │ │
│  │  └──────────────┘  └──────────────┘  └──────────────┘      │ │
│  │                                                              │ │
│  │  运行时动态切换: AF_XDP → io_uring → epoll                   │ │
│  │  AF_XDP 保险丝: 连续 3s 丢包 >5% 自动降级                    │ │
│  │  SIGUSR1 强制降级: 1 秒内回退到 epoll，通话不中断             │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │      Room Identity Provider 可插拔层 (可选，--turbo-rooms)   │ │
│  │                                                              │ │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────────────┐    │ │
│  │  │   static   │  │ token_hmac │  │    lua_script      │    │ │
│  │  │ username   │  │  签名验证  │  │ 自定义脚本解析     │    │ │
│  │  │ 直接解析   │  │  无状态验证 │  │ 任意鉴权系统对接   │    │ │
│  │  └────────────┘  └────────────┘  └────────────────────┘    │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │            Admin HTTP API (可选，--turbo-api-port)           │ │
│  │  - GET  /admin/metrics            Prometheus 指标            │ │
│  │  - POST /admin/turbo-disable      运行时关闭 Turbo           │ │
│  │  - POST /admin/drain              排空模式                   │ │
│  │  - GET  /admin/status             运行状态                   │ │
│  └─────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────┘
         │
         │ 信令服务在 ICE 配置中下发 TURN 地址和 Token
         ▼
┌─────────────────────┐
│   外部信令服务 (已有) │
│  - 房间状态唯一 Owner │
│  - 管理房间成员生命周期│
│  - 生成并下发 HMAC Token │
│  - 房间成员加入/离开控制│
└─────────────────────┘
```

### 3.2 数据流概要

**一对一标准中继 (无房间)**：
```
客户端 A → UDP 3478 → [单端口接收] → [五元组查找 alloc] → [Permission 检查] → [转发至客户端 B]
```

**房间广播 (启用 Turbo Rooms)**：
```
发布者 A → UDP 3478 → [单端口接收] → [五元组查找 alloc]
    → [检查 alloc.room_id ≠ 0] → [遍历房间成员表]
    → [零拷贝克隆包] → [重写各成员目标地址] → [批量发送]
    → 接收者 B, C, D...
```

---

## 4. 核心设计一：单端口收敛与自适应快速路径

### 4.1 端口收敛方案

#### 4.1.1 实现原理

传统 coturn 为每个 allocation 单独绑定一个临时端口，导致端口膨胀。coturn-turbo 改为：

1.  **固定监听端口**：UDP 3478 同时承载 STUN 信令和所有媒体中继。
2.  **取消端口分配**：在 Allocate 处理中，不再调用 `create_relay_connection()` 创建新的 UDP socket。改为通过条件编译钩子（`#ifdef TURBO_FEATURES`），直接返回固定端口 3478 作为 relay 地址。
3.  **XOR-RELAYED-ADDRESS 重写**：Allocate 成功响应中，将 `XOR-RELAYED-ADDRESS` 的端口字段强制设为 3478。

#### 4.1.2 流量隔离：信令面与媒体面分离

单端口承载所有流量带来 DDoS 放大风险。为缓解此风险，引入**两级轻量级流量整形器**：

```c
/* src/turbo/forward/turbo_shaper.h */
struct turbo_packet_shaper {
    uint64_t stun_pps_limit;        // STUN 包速率上限，默认 2000 pps
    uint64_t stun_burst_limit;      // 突发容忍，默认 4000
    uint64_t stun_dropped_total;    // 丢弃计数

    uint64_t media_queue_priority;  // 媒体队列优先级 (高)
    uint64_t stun_queue_priority;   // STUN 队列优先级 (低)
};

/* 收包后首先进入整形器 */
int turbo_shaper_classify(struct turbo_packet_shaper *shaper,
                           struct rtp_packet *pkt) {
    if (is_stun_message(pkt)) {
        if (rate_limit_exceeded(shaper->stun_pps_limit, shaper->stun_burst_limit)) {
            atomic_inc(&shaper->stun_dropped_total);
            return SHAPER_DROP;  // 丢弃并触发 turbo_stun_overload 告警
        }
        return SHAPER_STUN_QUEUE;
    }
    return SHAPER_MEDIA_QUEUE;  // 媒体数据不受限
}
```

-   **STUN 队列**：对消息类型为 `0x0000-0x00FF` 的包实施独立 PPS 限速，超限即时丢弃。
-   **媒体队列**：ChannelData（消息类型 `0x4000-0x7FFF`）和已关联的 Send Indication 不受限。
-   **媒体优先**：当系统负载高时，优先处理媒体队列，保证音视频质量不受信令风暴影响。

### 4.2 自适应查找引擎

由于客户端可能使用 ChannelBind 或 Send Indication 两种方式发送数据，且服务器不能强制客户端行为，必须设计兼容二者的查找机制。

#### 4.2.1 三表查找架构

```c
/* src/turbo/forward/turbo_fastpath.h */
struct turbo_fastpath {
    /* 第一层：信道绑定表 — 最快路径 */
    struct turbo_hash_table *channel_table;
    // Key: hash(src_ip, dst_ip, channel_no)
    // Value: allocation_id

    /* 第二层：L1 快速缓存 — 兼容 Send Indication */
    struct turbo_hash_table *l1_cache;
    // Key: hash(src_ip, src_port)
    // Value: allocation_id
    // 特性: 在 Allocate 成功后立即预热 (warmup)

    /* 第三层：辅助解析表 — 极端回退 */
    struct turbo_hash_table *username_table;
    // Key: username_hash
    // Value: allocation_id
    // 仅在 L1 未命中时使用，触发即告警
};
```

#### 4.2.2 查找流程

```
收包 → 识别消息类型
    │
    ├─ ChannelData (0x4000-0x7FFF):
    │   ├─ 提取 channel_no
    │   ├─ 查 channel_table (O(1))
    │   ├─ 命中 → 返回 allocation
    │   └─ 未命中 → 丢弃（channel 未绑定）
    │
    └─ Send Indication (STUN 0x0017):
        ├─ 查 l1_cache (O(1))
        ├─ 命中 → 返回 allocation（>99.9% 情况）
        └─ 未命中 (<0.1%):
            ├─ 解析 STUN USERNAME 属性
            ├─ 查 username_table
            ├─ 命中 → 更新 l1_cache，返回 allocation
            └─ 未命中 → 标准 STUN 处理流程
```

#### 4.2.3 L1 缓存预热机制

为保证纯 Send Indication 客户端的首包延迟不退化，在 Allocate 成功时立即预热 L1 缓存：

```c
/* 在 Allocate 成功响应发送前调用 */
void turbo_l1_cache_warmup(struct allocation *alloc,
                             struct sockaddr_in6 *client_addr) {
    struct turbo_l1_entry entry = {
        .src_ip = client_addr->sin6_addr,
        .src_port = client_addr->sin6_port,
        .alloc_id = alloc->id,
        .cached_at = time_now()
    };
    turbo_hash_insert(&turbo_fastpath.l1_cache, &entry);
}
```

这样，即使客户端首包使用 Send Indication，也能直接命中 L1 缓存，查找延迟与 ChannelData 持平（< 150µs）。

#### 4.2.4 五元组快速索引条目结构

```c
struct turbo_fast_entry {
    struct sockaddr_in6 src_addr;    // 客户端源地址
    struct sockaddr_in6 dst_addr;    // 服务器本端地址
    uint16_t channel_no;             // 0 表示未使用信道（五元组查找）
    uint32_t alloc_id;              // 内部 allocation 索引
    uint64_t last_seen;             // 最后命中时间戳
    uint32_t flags;                 // FAST_ENTRY_CHANNEL | FAST_ENTRY_L1
    UT_hash_handle hh;              // uthash 句柄
};
```

### 4.3 快速路径数据包处理完整流程

```
                        ┌──────────────────┐
                        │  网络后端收包     │
                        │  (io_uring/AF_XDP)│
                        └────────┬─────────┘
                                 │
                        ┌────────▼─────────┐
                        │  流量整形器       │
                        │  STUN/媒体分类限速 │
                        └────────┬─────────┘
                                 │
                    ┌────────────┴────────────┐
                    │                         │
           ┌────────▼────────┐    ┌──────────▼──────────┐
           │  ChannelData    │    │  Send Indication     │
           │  (消息类型 0x4xxx)│    │  (STUN 0x0017)       │
           └────────┬────────┘    └──────────┬──────────┘
                    │                         │
           ┌────────▼────────┐    ┌──────────▼──────────┐
           │ 查信道表         │    │ 查 L1 缓存           │
           │ (src+channel_no) │    │ (src_ip+src_port)    │
           └────────┬────────┘    └──────────┬──────────┘
                    │                         │
           ┌────────▼────────┐    ┌──────────▼──────────┐
           │ 命中 → alloc    │    │ 命中 → alloc         │
           │ 未命中 → 丢弃    │    │ 未命中 → STUN解析    │
           └────────┬────────┘    └──────────┬──────────┘
                    │                         │
                    └──────────┬──────────────┘
                               │
                    ┌──────────▼──────────┐
                    │  Permission 检查     │
                    │  (复用 coturn 原逻辑) │
                    └──────────┬──────────┘
                               │
               ┌───────────────┼───────────────┐
               │                               │
    ┌──────────▼──────────┐        ┌──────────▼──────────┐
    │ alloc.room_id != 0? │        │ alloc.room_id == 0  │
    │ (启用房间广播)       │        │ (标准一对一转发)     │
    └──────────┬──────────┘        └──────────┬──────────┘
               │                               │
    ┌──────────▼──────────┐        ┌──────────▼──────────┐
    │ 遍历房间成员表       │        │ 查找 peer allocation │
    │   (RCU 链表)         │        │ 转发至 peer 的地址   │
    │ 对每个成员:          │        └─────────────────────┘
    │  - 克隆包头(零拷贝)  │
    │  - 重写目标 IP/Port  │
    │  - 批量发送 (Burst)  │
    └──────────┬──────────┘
               │
    ┌──────────▼──────────┐
    │  审计日志记录        │
    │  (allocation_id,     │
    │   username, length)  │
    └─────────────────────┘
```

---

## 5. 核心设计二：网络后端抽象与运行时动态降级

### 5.1 统一后端接口定义

```c
/* src/turbo/netif/turbo_netif.h */

/* 数据包结构 (含引用计数，支持零拷贝共享) */
struct rtp_packet {
    void *data;              // 指向实际负载
    uint16_t len;            // 负载长度
    uint16_t headroom;       // 头部预留空间 (用于重写)
    uint32_t refcount;       // 原子引用计数
    uint64_t timestamp;      // 收包时间戳
    uint32_t alloc_id;       // 关联的 allocation (解析后填充)
    void *backend_priv;      // 后端私有数据 (mbuf / umem_frame)
};

/* 网络后端操作接口 */
struct turbo_netif_ops {
    /* 生命周期 */
    int (*init)(struct turbo_netif *tif, void *config);
    void (*close)(struct turbo_netif *tif);

    /* 数据收发 */
    int (*recv_pkts)(struct turbo_netif *tif,
                     struct rtp_packet **pkts,
                     uint16_t max_pkts);
    int (*send_pkt)(struct turbo_netif *tif,
                    struct rtp_packet *pkt,
                    struct sockaddr_in6 *dst);
    int (*send_burst)(struct turbo_netif *tif,
                      struct rtp_packet **pkts,
                      uint16_t count);

    /* 内存管理 */
    struct rtp_packet *(*alloc_pkt)(struct turbo_netif *tif);
    void (*free_pkt)(struct turbo_netif *tif,
                     struct rtp_packet *pkt);
    struct rtp_packet *(*clone_pkt)(struct turbo_netif *tif,
                                     struct rtp_packet *pkt);  // 零拷贝克隆，refcount++
};

/* 网络后端运行时状态 */
struct turbo_netif {
    struct turbo_netif_ops *ops;
    int backend_type;          // TURBO_BACKEND_EPOLL / IO_URING / AF_XDP
    int health_status;         // HEALTH_OK / DEGRADED / FAILED
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t dropped_packets;
    uint64_t degraded_count;   // 降级次数统计
};
```

### 5.2 三种后端详细对比

| 特性 | epoll | io_uring | AF_XDP |
| :--- | :--- | :--- | :--- |
| **内核版本要求** | 任意 | ≥ 5.6 | ≥ 5.4 + 驱动支持 |
| **编译依赖** | 无 | liburing-dev | libbpf, libxdp, clang |
| **零拷贝** | 否 | 部分 (MSG_ZEROCOPY) | 是 (UMEM 共享) |
| **批量收发** | sendmmsg/recvmmsg | SQE/CQE 批量提交 | RX/TX Ring 批量操作 |
| **内核旁路** | 否 | 否 | 是 |
| **网卡独占** | 否 | 否 | 否 (XDP 过滤器仅 redirect TURN 端口) |
| **硬件要求** | 无 | 无 | 网卡驱动支持 XDP (drv 模式需原生驱动) |
| **性能增益 (vs epoll)** | 基线 | ↑ 8% | ↑ 48% |
| **CPU 开销 @ 10Gbps** | 72% | 65% | 48% |
| **单包转发延迟 (P50)** | 180 µs | 145 µs | 80 µs |
| **运行时切换** | N/A | 可降级到 epoll | 可降级到 io_uring → epoll |
| **安全可见性** | 原生 | 原生 | 可选 turbo-mirror 镜像接口 |

### 5.3 AF_XDP 后端详细设计

#### 5.3.1 XDP 程序过滤器

启动时自动编译并加载 XDP BPF 程序，仅将 TURN 端口的 UDP 流量 `XDP_REDIRECT` 到 AF_XDP socket：

```c
/* src/turbo/netif/xdp_prog.c (编译为 xdp_prog.o) */
SEC("xdp")
int xdp_filter(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    // 仅处理 IPv4 UDP
    if (eth->h_proto != htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if (ip->protocol != IPPROTO_UDP)
        return XDP_PASS;

    struct udphdr *udp = (void *)((long)ip + ip->ihl * 4);
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    // 仅捕获 TURN 端口
    if (udp->dest == htons(TURBO_RELAY_PORT)) {
        return XDP_REDIRECT;  // 送入 AF_XDP socket
    }

    return XDP_PASS;  // 其余流量 (SSH/HTTP/DNS) 进入内核协议栈
}
```

#### 5.3.2 XDP 模式选择

```c
enum turbo_afxdp_mode {
    TURBO_AFXDP_MODE_AUTO = 0,  // 自动: 先尝试 DRV，失败降级 SKB
    TURBO_AFXDP_MODE_DRV = 1,   // 驱动原生零拷贝 (需网卡驱动支持)
    TURBO_AFXDP_MODE_SKB = 2    // SKB 内核回退 (通用兼容模式)
};
```

#### 5.3.3 AF_XDP 保险丝

```c
/* 性能监控与自动降级触发 */
void turbo_afxdp_health_check(struct turbo_netif *tif) {
    struct turbo_afxdp_priv *priv = tif->backend_priv;

    // 检查 RX Ring 满情况
    uint32_t rx_full_duration = get_ring_full_duration(priv->rx_ring);

    if (rx_full_duration > AFXDP_DEGRADE_THRESHOLD_SEC * 1000) {
        // 连续 3 秒队列满
        tif->health_status = HEALTH_DEGRADED;
        log_warn("AF_XDP RX ring full for %ums, triggering degradation",
                 rx_full_duration);

        // 自动触发降级到 io_uring
        turbo_netif_switch_backend(tif, TURBO_BACKEND_IO_URING);
        metric_inc("turbo_afxdp_degraded_total");
    }
}
```

### 5.4 运行时动态降级机制

```c
/* 降级触发方式 */
enum turbo_degrade_trigger {
    DEGRADE_AUTO,        // 自动 (保险丝触发)
    DEGRADE_MANUAL,      // 手动 (SIGUSR1 / Admin API)
    DEGRADE_DRAIN        // 排空模式
};

/* 降级处理主函数 */
int turbo_netif_degrade(struct turbo_netif *tif,
                         enum turbo_degrade_trigger trigger) {
    int target_backend;

    switch (tif->backend_type) {
        case TURBO_BACKEND_AF_XDP:
            target_backend = TURBO_BACKEND_IO_URING;
            break;
        case TURBO_BACKEND_IO_URING:
            target_backend = TURBO_BACKEND_EPOLL;
            break;
        default:
            return -1;  // 已是 epoll，无法继续降级
    }

    // 1. 停止当前后端收包
    tif->ops->suspend(tif);

    // 2. 初始化目标后端
    turbo_netif_ops_init(target_backend, tif);

    // 3. 迁移已有的 allocation 地址绑定
    turbo_allocation_migrate_backend(target_backend);

    // 4. 恢复收包
    tif->ops->resume(tif);

    // 5. 记录与告警
    log_info("Turbo degraded from %d to %d (trigger: %d)",
             tif->backend_type, target_backend, trigger);
    metric_set("turbo_backend", target_backend);

    return 0;
}
```

**SIGUSR1 信号处理**：
```c
void handle_sigusr1(int sig) {
    log_warn("Received SIGUSR1, triggering immediate turbo degradation");
    turbo_netif_degrade(turbo_netif, DEGRADE_MANUAL);
}
```

---

## 6. 核心设计三：信令驱动的轻量房间广播

### 6.1 设计原则

-   **信令是房间状态的唯一 Owner**：房间创建、成员加入/离开、权限管理等全部由外部信令服务决策。
-   **coturn-turbo 是无状态媒体执行单元**：不持久化房间状态，不决策成员变更，只执行媒体包的复制和转发。
-   **客户端零改动**：房间功能通过标准的 TURN 认证信息（username/password）触发，客户端无任何感知。
-   **默认不启用**：通过 `--turbo-rooms` 配置项按需开启。

### 6.2 房间广播工作流程

```
信令服务                                      coturn-turbo
    │                                              │
    │ 1. 用户 A 加入房间 room123                    │
    │    信令服务完成鉴权和房间状态更新               │
    │                                              │
    │ 2. 生成 HMAC Token:                          │
    │    payload = {"room_id":"123",               │
    │               "member_id":"userA",           │
    │               "expiry":1678886400}           │
    │    signature = HMAC-SHA256(secret, payload)  │
    │    token = base64(payload.signature)         │
    │                                              │
    │ 3. 下发 ICE 配置给客户端 A                   │
    │    {                                         │
    │      urls: "turn:server:3478",               │
    │      username: token,                        │
    │      credential: "dummy"                     │
    │    }                                         │
    │                                              │
    │                                    客户端 A 使用该配置发起 Allocate
    │                                              │
    │                                    4. 收到 Allocate 请求
    │                                       验证 Token 签名 ✓
    │                                       解析出 room_id=123
    │                                       将 alloc_A.room_id = 123
    │                                       加入 room123 的广播成员表
    │                                       返回 Allocate Success
    │                                              │
    │ (同理处理用户 B、C...)                         │
    │                                              │
    │                                    5. 用户 A 发送媒体包
    │                                       快速路径查找 → alloc_A
    │                                       检查 alloc_A.room_id = 123
    │                                       遍历 room123 成员表:
    │                                         - 克隆包
    │                                         - 重写目标为 B 的地址 → 发送
    │                                         - 重写目标为 C 的地址 → 发送
    │                                              │
    │ 6. 当用户 A 离开房间时:                       │
    │    信令不再下发此 Token (Allocate 自然超时)   │
    │    或主动调用管理 API (未来扩展)              │
    │                                              │
    │                                    7. alloc_A 超时销毁
    │                                       自动从 room123 移除
    │                                              │
```

### 6.3 房间成员管理数据结构

```c
/* src/turbo/room/turbo_room.h */

struct turbo_room_member {
    uint32_t alloc_id;               // allocation ID
    char member_id[64];              // 成员标识 (从 Token 解析)
    struct sockaddr_in6 relay_addr;  // 中继目标地址
    uint64_t joined_at;              // 加入时间
    uint64_t last_forward;           // 最后转发时间
    struct list_head node;           // RCU 链表节点
};

struct turbo_room {
    char room_id[64];                // 房间 ID
    uint32_t member_count;           // 当前成员数
    uint32_t max_members;            // 成员上限 (默认 50，防广播风暴)
    uint64_t created_at;             // 创建时间

    /* RCU 保护的成员链表 */
    struct list_head members;        // struct turbo_room_member 链表
    pthread_rwlock_t members_lock;   // 写锁 (加入/离开时持有)

    /* 统计 */
    uint64_t total_broadcasts;       // 广播次数
    uint64_t total_forwarded_pkts;   // 转发包数
};
```

### 6.4 广播执行流程

```c
/* src/turbo/room/turbo_room.c */

int turbo_room_broadcast(struct turbo_netif *netif,
                          struct turbo_room *room,
                          struct rtp_packet *src_pkt,
                          uint32_t src_alloc_id) {

    struct turbo_room_member *member;
    uint32_t forwarded = 0;

    // 读锁保护成员遍历 (写操作只在加入/离开时，频率极低)
    pthread_rwlock_rdlock(&room->members_lock);

    list_for_each_entry_rcu(member, &room->members, node) {
        // 跳过发布者自己
        if (member->alloc_id == src_alloc_id)
            continue;

        // 零拷贝克隆包 (仅增加引用计数，不拷贝负载)
        struct rtp_packet *clone = netif->ops->clone_pkt(netif, src_pkt);
        if (!clone)
            continue;

        // 重写目标地址为该成员的 relay 地址
        clone->dst_addr = member->relay_addr;

        // 批量发送 (积累到 burst buffer)
        netif->ops->send_pkt(netif, clone, &member->relay_addr);

        forwarded++;
    }

    pthread_rwlock_unlock(&room->members_lock);

    // 更新统计
    __atomic_fetch_add(&room->total_broadcasts, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&room->total_forwarded_pkts, forwarded, __ATOMIC_RELAXED);

    return forwarded;
}
```

### 6.5 房间生命周期管理

-   **房间创建**：当第一个带有某 `room_id` 的 allocation 建立时，自动创建房间上下文（惰性创建）。
-   **房间销毁**：当房间内最后一个 allocation 被销毁时，自动销毁房间上下文（引用计数归零）。
-   **成员上限保护**：默认最大 50 人，超过后拒绝新成员加入并记录告警。
-   **幽灵成员防护**：allocation 超时销毁时，`turbo_room_remove_member()` 作为销毁回调被自动调用。

---

## 7. 核心设计四：Room Identity Provider 可插拔机制

这是本方案实现**松耦合**和**开源友好**的关键架构设计。

### 7.1 设计目标

-   **解耦**：coturn-turbo 不应包含任何特定信令服务的业务逻辑。
-   **灵活**：支持从最简单的静态解析到复杂的外部鉴权集成。
-   **安全**：Token 验证模式提供防篡改和无状态验证。
-   **可扩展**：允许用户在不修改 C 源码的情况下集成自定义逻辑。

### 7.2 抽象接口定义

```c
/* src/turbo/room/turbo_room_provider.h */

#define MAX_ROOM_ID_LEN   64
#define MAX_MEMBER_ID_LEN 64

/* Provider 提取结果 */
struct turbo_room_info {
    char room_id[MAX_ROOM_ID_LEN];      // 提取的房间 ID
    char member_id[MAX_MEMBER_ID_LEN];  // 提取的成员 ID
    uint64_t expiry;                     // Token 过期时间 (0 表示不限)
};

/* Provider 操作接口 */
struct turbo_room_provider_ops {
    const char *name;   // "static" / "token_hmac" / "lua_script"

    /* 初始化 (读取配置，加载脚本等) */
    int (*init)(void *config);

    /* 从 TURN 认证信息中提取房间信息
     * 参数:
     *   - username: 客户端提供的 TURN username
     *   - realm: 认证域
     *   - credential: 客户端提供的密码
     *   - client_addr: 客户端地址
     *   - info: 输出，提取的房间信息
     * 返回:
     *    0  - 成功提取
     *   -1  - 非房间流量 (继续标准 TURN 处理)
     *   -2  - 验证失败 (拒绝连接)
     */
    int (*extract)(const char *username,
                   const char *realm,
                   const char *credential,
                   struct sockaddr_in6 *client_addr,
                   struct turbo_room_info *info);

    /* 清理 */
    void (*deinit)(void);
};
```

### 7.3 内置 Provider 实现

#### 7.3.1 `static` Provider：用户名直接解析

最简单的模式，直接解析 `username` 格式。适合受信任的内网环境或开发测试。

```c
/* src/turbo/room/provider_static.c */

/* 支持的格式: "room<room_id>:user<member_id>" 或 "room<room_id>:<member_id>" */
int provider_static_extract(const char *username, ...) {
    // 尝试匹配格式 "room123:user456"
    if (sscanf(username, "room%[^:]:%s", info->room_id, info->member_id) == 2) {
        return 0;  // 成功
    }
    // 尝试匹配格式 "room123:456"
    if (sscanf(username, "room%[^:]:%s", info->room_id, info->member_id) == 2) {
        return 0;
    }
    return -1;  // 非房间流量
}
```

**配置**：
```ini
turbo-rooms
turbo-room-id-provider static
```

**客户端用法**：
```javascript
iceServers: [{
    urls: "turn:server:3478",
    username: "room123:user456",    // 直接编码房间信息
    credential: "password"
}]
```

#### 7.3.2 `token_hmac` Provider：HMAC 签名 Token

**推荐的生产环境方案**。使用 HMAC-SHA256 签名保护 Token 不可篡改，coturn-turbo 独立验证，无需访问外部数据库。

**Token 结构**：
```
Token = base64url_encode( payload )
其中 payload = base64url_encode(header) + "." + base64url_encode(body) + "." + base64url_encode(signature)

header = {"alg":"HS256","typ":"JWT-like"}
body   = {"room_id":"123","member_id":"userA","exp":1678886400}
signature = HMAC-SHA256(secret, header + "." + body)
```

**验证逻辑**：
```c
/* src/turbo/room/provider_token_hmac.c */

int provider_token_hmac_extract(const char *username, ...) {
    // 1. Base64 解码 Header 和 Body
    char *payload = base64_decode(username);
    char *header, *body, *signature;
    split_jwt_parts(payload, &header, &body, &signature);

    // 2. 验证签名
    unsigned char computed_sig[32];
    char sig_input[1024];
    snprintf(sig_input, sizeof(sig_input), "%s.%s", header, body);
    HMAC_SHA256(shared_secret, strlen(shared_secret),
                sig_input, strlen(sig_input), computed_sig);

    if (memcmp(computed_sig, base64_decode(signature), 32) != 0) {
        log_warn("Token signature verification failed for user from %s",
                 addr_to_str(client_addr));
        return -2;  // 验证失败
    }

    // 3. 解析 body 中的 room_id 和 member_id
    json_parse(body, &info->room_id, &info->member_id, &info->expiry);

    // 4. 检查过期
    if (info->expiry > 0 && time_now() > info->expiry) {
        log_warn("Token expired for room %s member %s",
                 info->room_id, info->member_id);
        return -2;
    }

    return 0;  // 成功
}
```

**配置**：
```ini
turbo-rooms
turbo-room-id-provider token_hmac
turbo-room-token-secret your-shared-secret-key-here
turbo-room-token-expiry 3600
```

**信令服务端 Token 生成示例 (Node.js)**：
```javascript
const crypto = require('crypto');

function generateRoomToken(roomId, memberId, secret, ttlSeconds = 3600) {
    const header = { alg: "HS256", typ: "JWT-like" };
    const body = {
        room_id: roomId,
        member_id: memberId,
        exp: Math.floor(Date.now() / 1000) + ttlSeconds
    };

    const headerB64 = Buffer.from(JSON.stringify(header)).toString('base64url');
    const bodyB64 = Buffer.from(JSON.stringify(body)).toString('base64url');
    const sigInput = `${headerB64}.${bodyB64}`;

    const signature = crypto.createHmac('sha256', secret)
        .update(sigInput)
        .digest('base64url');

    return `${headerB64}.${bodyB64}.${signature}`;
}

// 下发给客户端
const token = generateRoomToken("123", "userA", SHARED_SECRET);
const iceServers = [{
    urls: "turn:coturn-turbo.example.com:3478",
    username: token,
    credential: "dummy"  // 安全依赖 Token，此值可固定
}];
```

**Provider 端验证过程**：
1. 从 Allocate 请求中获取 `USERNAME` 属性的值（即 Token）。
2. 使用本地配置的共享密钥 `turbo-room-token-secret` 重新计算 HMAC 签名。
3. 比对签名，防止客户端篡改 `room_id` 或 `member_id`。
4. 检查 `exp` 字段，防止重放过期 Token。
5. 验证通过后，将解析出的 `room_id` 和 `member_id` 关联到当前 allocation。

#### 7.3.3 `lua_script` Provider：自定义脚本扩展

**最高灵活性**的扩展方式，允许用户不修改 C 源码即可对接任意鉴权系统。

```c
/* src/turbo/room/provider_lua.c */

int provider_lua_extract(const char *username, ...) {
    // 调用用户提供的 Lua 函数
    lua_getglobal(L, "extract_room_info");
    lua_pushstring(L, username);
    lua_pushstring(L, realm);
    lua_pushstring(L, client_addr_str);

    if (lua_pcall(L, 3, 2, 0) != 0) {
        log_error("Lua extract_room_info error: %s", lua_tostring(L, -1));
        return -1;
    }

    // Lua 返回值: room_id, member_id (nil 表示非房间流量)
    if (lua_isnil(L, -2)) {
        return -1;  // 非房间流量
    }

    strncpy(info->room_id, lua_tostring(L, -2), MAX_ROOM_ID_LEN);
    strncpy(info->member_id, lua_tostring(L, -1), MAX_MEMBER_ID_LEN);
    return 0;
}
```

**示例 Lua 脚本** (`/etc/turn/room_extractor.lua`)：
```lua
-- 示例: 通过 Redis 查询用户所属房间
local redis = require("redis")
local client = redis.connect("127.0.0.1", 6379)

function extract_room_info(username, realm, client_addr)
    -- 从 Redis 获取该用户当前所在的房间
    local room_id = client:get("user:" .. username .. ":current_room")
    if room_id then
        local member_id = client:get("user:" .. username .. ":member_id")
        return room_id, member_id
    end
    return nil, nil  -- 非房间流量，走标准 TURN 中继
end
```

**配置**：
```ini
turbo-rooms
turbo-room-id-provider lua_script
turbo-room-lua-script /etc/turn/room_extractor.lua
```

### 7.4 Provider 选择流程

```
启动 → 读取 turbo-room-id-provider 配置
    │
    ├─ "none" (默认) → 不加载任何 Provider，房间功能不启用
    ├─ "static"      → 加载 provider_static.c
    ├─ "token_hmac"  → 加载 provider_token_hmac.c (验证 turbo-room-token-secret 已配置)
    └─ "lua_script"  → 加载 provider_lua.c (验证 turbo-room-lua-script 文件存在)
```

### 7.5 在 Allocate 流程中的集成点

```c
/* src/server/ns_turn_server.c — Allocate 处理 (伪代码) */

int handle_turn_allocate(turn_session *ss, stun_message *request) {
    // ... 原有处理 ...

#ifdef TURBO_FEATURES
    if (turbo_config.rooms_enabled && turbo_config.room_provider) {
        struct turbo_room_info info;

        // 调用当前配置的 Provider 尝试提取房间信息
        int ret = turbo_config.room_provider->extract(
            ss->username,
            ss->realm,
            ss->credential,
            &ss->client_addr,
            &info
        );

        if (ret == 0) {
            // 成功提取，关联房间
            allocation_set_room_id(alloc, info.room_id, info.member_id);

            // 将 allocation 加入房间广播成员表
            turbo_room_add_member(alloc);

            // 记录日志
            log_info("Allocation %u joined room %s as member %s",
                     alloc->id, info.room_id, info.member_id);
        }
        else if (ret == -2) {
            // Token 验证失败，拒绝 Allocate
            send_allocate_error_response(ss, 401, "Token verification failed");
            return -1;
        }
        // ret == -1: 非房间流量，继续标准 TURN 处理
    }
#endif

    // ... 继续原有 Allocate 处理 (或 Turbo 单端口固定端口分配) ...
}
```

---

## 8. 安全与审计设计

### 8.1 威胁模型与缓解措施

| 威胁 | 风险等级 | 缓解措施 |
| :--- | :--- | :--- |
| **单端口 DDoS 放大** | 高 | STUN/媒体队列隔离 + STUN PPS 限速默认 2000/s |
| **ChannelData 无用户标识** | 高 | 审计钩子记录 `alloc_id→username` 映射；采样 pcap 导出 |
| **AF_XDP 旁路不可见** | 中 | 可选 `turbo-mirror` 虚拟接口推送元数据至 nflog |
| **Token 伪造/篡改** | 高 | HMAC-SHA256 签名验证；客户端无法伪造 |
| **Token 重放** | 中 | Token 内置过期时间 `exp`；服务端检查 |
| **房间越权** | 高 | Token 中 `room_id` 受签名保护，不可篡改 |
| **广播风暴** | 中 | 房间成员上限硬限制 (默认 50) |
| **AF_XDP 驱动 bug 导致内核 panic** | 低 | 自动保险丝降级；XDP 沙箱模式限制指令数 |
| **共享密钥泄露** | 高 | 文件权限控制 (600)；定期轮换；支持从环境变量读取 |

### 8.2 审计钩子设计

```c
/* src/turbo/forward/turbo_audit.h */

#define TURBO_AUDIT_BUF_SIZE 4096  // 环形缓冲区条目数

struct turbo_audit_event {
    uint64_t timestamp_ns;         // 纳秒时间戳
    uint32_t alloc_id;             // allocation ID
    char username[128];            // 用户名 (异步解析)
    struct sockaddr_in6 src_addr;  // 源地址
    struct sockaddr_in6 dst_addr;  // 目的地址
    uint16_t pkt_len;              // 包长度
    uint8_t direction;             // 0=入站 1=出站
    uint8_t pkt_type;              // ChannelData / Send Indication / RTP
};

/* 环形缓冲区 — 无锁写入 */
struct turbo_audit_ring {
    struct turbo_audit_event events[TURBO_AUDIT_BUF_SIZE];
    uint32_t head;                 // 写入位置 (原子)
    uint32_t dropped;              // 溢出丢弃计数

    int unix_socket_fd;            // Unix 域套接字 (JSON 流输出)
    uint32_t flush_interval_ms;    // 刷新间隔 (默认 10000ms)
};
```

**输出格式 (Unix Socket JSON 流)**：
```json
{
  "ts": "2026-04-25T14:32:01.123456Z",
  "alloc_id": 42,
  "username": "room123:userA",
  "src": "192.168.1.100:50000",
  "dst": "122.51.14.87:3478",
  "len": 1280,
  "direction": "inbound",
  "type": "ChannelData"
}
```

### 8.3 AF_XDP 可见性 (turbo-mirror)

```c
/* 可选开启，将旁路流量的元数据推送回内核安全栈 */
int turbo_mirror_init(const char *ifname) {
    // 创建虚拟 veth pair
    // 一端 turbo-mirror-in → AF_XDP 元数据注入
    // 一端 turbo-mirror-out → iptables/nflog 捕获
    // 性能开销: ~3% 额外 CPU
}

/* 每包元数据推送 */
void turbo_mirror_push(struct rtp_packet *pkt, uint32_t alloc_id) {
    struct turbo_mirror_meta meta = {
        .src_ip = extract_src_ip(pkt),
        .dst_ip = extract_dst_ip(pkt),
        .src_port = extract_src_port(pkt),
        .dst_port = extract_dst_port(pkt),
        .alloc_id = alloc_id,
        .timestamp = pkt->timestamp,
        .action = "forward"
    };
    netlink_send(turbo_mirror_sock, &meta, sizeof(meta));
}
```

---

## 9. 客户端兼容性与 SLO

### 9.1 兼容性矩阵

| 客户端类型 | 一对一 TURN | 房间广播 | 说明 |
| :--- | :--- | :--- | :--- |
| Chrome (WebRTC 1.0) | ✅ 完全兼容 | ✅ 无需任何修改 | Token 作为 username 传入 |
| Firefox | ✅ 完全兼容 | ✅ 无需任何修改 | 同上 |
| Safari 15+ | ✅ 完全兼容 | ✅ 无需任何修改 | 同上 |
| Android Native | ✅ 完全兼容 | ✅ 无需任何修改 | 同上 |
| iOS Native | ✅ 完全兼容 | ✅ 无需任何修改 | 同上 |
| 仅 Send Indication 客户端 | ✅ 完全兼容 | ✅ 无需任何修改 | L1 预热保证首包延迟不退化 |
| 仅 ChannelBind 客户端 | ✅ 完全兼容 | ✅ 无需任何修改 | 走信道表快速路径 |
| 无 ChannelBind 客户端 | ✅ 完全兼容 | ✅ 无需任何修改 | 降级路径自动处理 |

### 9.2 服务等级目标 (SLO)

| 指标 | 目标值 | 测量方式 |
| :--- | :--- | :--- |
| 一对一转发延迟 (P50) | ≤ 原生 epoll 的 110% | 端到端 RTT 测试 |
| Send Indication 首包延迟 | < 150µs | 服务器内部计时 |
| 房间广播额外延迟 | < 50µs | 单包入站到最后一个克隆包发出 |
| 快速路径查找命中率 | > 99.9% | `turbo_fastpath_hits / (hits + l1_miss)` |
| 降级路径端到端延迟增量 | < 5% | 对比降级前后 |
| 运行时降级过程 | 0 通话中断 | 实际测试 |
| AF_XDP → io_uring 降级延迟 | < 100ms | 从检测到切换完成 |

---

## 10. 运维、监控与灰度回滚

### 10.1 Prometheus 指标

| 指标名称 | 类型 | 标签 | 说明 |
| :--- | :--- | :--- | :--- |
| `turbo_allocations_current` | Gauge | `username` | 当前活跃 allocation 数 |
| `turbo_fastpath_hits_total` | Counter | `type=channelbind/l1cache` | 快速路径命中次数 |
| `turbo_l1_miss_total` | Counter | - | L1 缓存未命中 (应接近 0) |
| `turbo_stun_overload_drops_total` | Counter | - | STUN 限速丢弃数 |
| `turbo_forwarded_bytes_total` | Counter | `username, action` | 每用户转发字节数 |
| `turbo_forwarded_packets_total` | Counter | `username, action` | 每用户转发包数 |
| `turbo_room_members_current` | Gauge | `room_id` | 各房间当前成员数 |
| `turbo_room_broadcast_dropped_total` | Counter | `room_id, reason` | 广播丢弃数 |
| `turbo_backend` | Gauge | - | 当前后端: 1=AF_XDP, 2=io_uring, 3=epoll |
| `turbo_afxdp_degraded_total` | Counter | - | AF_XDP 降级次数 |
| `turbo_degrade_total` | Counter | `trigger=auto/manual/drain` | 总降级次数 |
| `turbo_audit_dropped_total` | Counter | - | 审计日志丢弃数 |

### 10.2 Admin HTTP API

| 端点 | 方法 | 说明 |
| :--- | :--- | :--- |
| `/admin/metrics` | GET | Prometheus 格式指标 |
| `/admin/status` | GET | JSON 格式运行状态 |
| `/admin/turbo-disable` | POST | 运行时关闭 Turbo（降级到 epoll） |
| `/admin/turbo-enable` | POST | 重新启用 Turbo |
| `/admin/drain` | POST | 进入排空模式（拒绝新 alloc，等待旧会话结束） |
| `/admin/drain/status` | GET | 查看排空进度（剩余 alloc 数） |

**`/admin/status` 响应示例**：
```json
{
  "turbo_enabled": true,
  "backend": "io_uring",
  "backend_health": "ok",
  "allocations": 1234,
  "rooms": 15,
  "total_room_members": 89,
  "fastpath_hit_rate": 99.97,
  "l1_miss_total": 12,
  "uptime_seconds": 86400
}
```

### 10.3 灰度上线策略

**阶段一：信令层 ICE 候选控制**
```javascript
// 信令服务根据灰度比例选择 TURN 节点
function getIceServers(userId, roomId) {
    const servers = [{ urls: "stun:stun.example.com:3478" }];

    if (isInTurboExperiment(userId)) {
        servers.push({
            urls: "turn:turbo-node.example.com:3478",
            username: generateRoomToken(roomId, userId),
            credential: "dummy"
        });
    }

    // 始终保留旧节点作为回退
    servers.push({
        urls: "turn:old-coturn.example.com:3478",
        username: "legacy_user",
        credential: "legacy_pass"
    });

    return servers;
}
```

### 10.4 节点 Drain 与下线流程

```bash
# 1. 信令层停止向该节点分配新用户
# 2. 触发 Drain 模式
curl -X POST http://turbo-node:8080/admin/drain

# 3. 监控排空进度
curl http://turbo-node:8080/admin/drain/status
# {"draining": true, "allocations_remaining": 150, "estimated_drain_seconds": 300}

# 4. 排空完成后停止进程
sudo systemctl stop coturn-turbo
```

### 10.5 紧急回滚 (SOP)

| 步骤 | 操作 | 影响 |
| :--- | :--- | :--- |
| 1 | 信令层移除 turbo 节点候选 | 新用户不再连接 |
| 2 | `kill -SIGUSR1 $(pidof turnserver)` | 1 秒内降级到 epoll，通话不中断 |
| 3 | 验证 `turbo_backend` 指标变为 3 | 确认降级成功 |
| 4 | 排空后替换为旧版二进制 | 无影响 |

---

## 11. 代码结构与上游合并策略

### 11.1 目录结构

```
coturn/  (基于上游 master 分支)
├── configure                     [调整] 增加 --turbo 及相关选项
├── Makefile                      [调整] 引入 src/turbo/ 编译目标
├── src/
│   ├── server/                   [最小侵入]
│   │   ├── ns_turn_allocation.h  [调整] #ifdef TURBO_FEATURES 扩展字段
│   │   ├── ns_turn_allocation.c  [调整] 增加 allocation_set_room_id()
│   │   ├── ns_turn_server.c      [调整] Allocate 路径增加 Turbo 钩子
│   │   └── ns_turn_maps.c        [调整] #ifdef 保护全局哈希表
│   │
│   ├── apps/relay/               [最小侵入]
│   │   ├── mainrelay.c           [调整] 增加 turbo_init/turbo_deinit 调用
│   │   ├── netengine.c           [调整] UDP 收发点增加 turbo 快速路径
│   │   └── mainrelay.h           [调整] extern 声明
│   │
│   └── turbo/                    [新增核心库，完全独立]
│       ├── turbo.h               [新增] 主开关和接口
│       │
│       ├── netif/                [新增] 网络后端抽象层
│       │   ├── turbo_netif.h     [新增] 统一后端接口定义
│       │   ├── turbo_netif.c     [新增] 后端选择与降级逻辑
│       │   ├── turbo_epoll.c     [新增] epoll 后端实现
│       │   ├── turbo_iouring.c   [新增] io_uring 后端实现
│       │   ├── turbo_af_xdp.c    [新增] AF_XDP 后端实现
│       │   └── xdp_prog.c        [新增] XDP eBPF 过滤程序 (编译为 xdp_prog.o)
│       │
│       ├── forward/              [新增] 快速转发路径
│       │   ├── turbo_fastpath.h  [新增] 自适应查找引擎
│       │   ├── turbo_fastpath.c  [新增] 三表查找 + L1 预热
│       │   ├── turbo_shaper.h    [新增] 流量整形器
│       │   ├── turbo_shaper.c    [新增] STUN/媒体隔离限速
│       │   ├── turbo_switch.h    [新增] 零拷贝克隆和重写
│       │   ├── turbo_switch.c    [新增] 转发执行
│       │   ├── turbo_audit.h     [新增] 审计钩子
│       │   └── turbo_audit.c     [新增] 审计环形缓冲 + Unix Socket 输出
│       │
│       ├── room/                 [新增] 轻量房间广播 (可选)
│       │   ├── turbo_room.h      [新增] 房间成员管理
│       │   ├── turbo_room.c      [新增] RCU 链表 + 广播执行
│       │   ├── turbo_room_provider.h  [新增] Provider 抽象接口
│       │   ├── provider_static.c     [新增] static Provider
│       │   ├── provider_token_hmac.c [新增] HMAC Token Provider
│       │   └── provider_lua.c        [新增] Lua 脚本 Provider
│       │
│       ├── api/                  [新增] Admin API
│       │   ├── turbo_api.h       [新增] HTTP API 接口
│       │   └── turbo_api.c       [新增] metrics/status/drain/disable
│       │
│       └── common/               [新增] 工具库
│           ├── turbo_hash.h      [新增] 无锁哈希表 (uthash 封装)
│           ├── turbo_ring.h      [新增] 无锁环形队列
│           └── turbo_json.h      [新增] 轻量 JSON 解析
│
├── conf/
│   └── turbo.conf.example        [新增] Turbo 配置示例
│
└── docs/
    ├── TURBO.md                  [新增] Turbo 功能文档
    └── TURBO_ROOM_PROVIDER.md    [新增] Room Provider 开发指南
```

### 11.2 侵入点清单 (上游合并友好)

所有对 coturn 原有代码的修改均通过 `#ifdef TURBO_FEATURES` 保护：

| 文件 | 修改点 | 说明 |
| :--- | :--- | :--- |
| `configure` | 增加 `--turbo`, `--turbo-rooms`, `--turbo-backend` 选项 | 附加功能 |
| `ns_turn_allocation.h` | 扩展 `struct _allocation` (末尾增加 room_id 等字段) | 编译时可选 |
| `ns_turn_allocation.c` | 增加 `allocation_set_room_id()` 等函数 | `#ifdef` 保护 |
| `ns_turn_server.c` | Allocate 路径增加 2 个钩子 (Provider 调用 + 端口固定) | 总共 < 20 行新增 |
| `ns_turn_maps.c` | 增加全局哈希表声明 | `#ifdef` 保护 |
| `mainrelay.c` | 增加 `turbo_init()` / `turbo_deinit()` 调用 | 5 行新增 |
| `netengine.c` | UDP 收发处增加 1 个快速路径入口 | 10 行新增 |

当 `./configure` 不添加 `--turbo` 时，编译出的二进制与上游完全一致。

---

## 12. 构建、配置与部署

### 12.1 依赖表

| 模式 | 编译依赖 | 运行时依赖 | 内核要求 |
| :--- | :--- | :--- | :--- |
| 标准 (无 Turbo) | libevent, OpenSSL | 无特殊 | 任意 |
| Turbo + io_uring | liburing-dev | liburing | ≥ 5.6 |
| Turbo + AF_XDP | libbpf-dev, libxdp-dev, clang | libbpf, libxdp | ≥ 5.4 + 网卡驱动支持 |

### 12.2 编译

```bash
# 标准编译 (与上游一致)
./configure && make -j$(nproc)

# Turbo 模式 (默认 io_uring 后端)
./configure --turbo && make -j$(nproc)

# Turbo + AF_XDP 模式
./configure --turbo --turbo-backend=afxdp && make -j$(nproc)

# Turbo + 房间模块
./configure --turbo --turbo-rooms && make -j$(nproc)
```

### 12.3 配置参考

```ini
# /etc/turnserver.conf — coturn-turbo 生产配置示例

# === 基础 TURN 配置 ===
listening-port=3478
listening-ip=0.0.0.0
relay-ip=<YOUR_PUBLIC_IP>
realm=north
lt-cred-mech
user=legacy_user:legacy_password

# === 证书 (TLS/DTLS 可选) ===
# cert=/etc/turn/cert.pem
# pkey=/etc/turn/key.pem

# === Turbo 核心配置 ===
turbo
turbo-backend=io_uring          # io_uring / af_xdp (默认 io_uring)
turbo-l1-warmup enable          # 启用 L1 缓存预热 (推荐)

# === AF_XDP 后端配置 (仅当 turbo-backend=af_xdp 时有效) ===
# turbo-afxdp-mode=auto         # auto / drv / skb (默认 auto)
# turbo-xdp-iface=eth0          # 绑定网卡接口

# === 房间广播 (可选，按需开启) ===
# turbo-rooms
# turbo-room-id-provider token_hmac       # static / token_hmac / lua_script
# turbo-room-token-secret <YOUR_SECRET>   # 与信令服务共享的密钥
# turbo-room-token-expiry 3600            # Token 有效期 (秒)

# === 审计 (可选) ===
# turbo-audit-log unix:///var/run/turn-audit.sock
# turbo-audit-mirror enable               # AF_XDP 元数据镜像 (性能开销 ~3%)

# === Admin API (推荐) ===
turbo-api-port=8080
# 建议仅监听内网 IP
# turbo-api-listen-ip=127.0.0.1

# === 日志 ===
verbose
log-file=/var/log/turnserver/turbo.log
```

### 12.4 启动

```bash
# 基础 Turbo 模式
turnserver -c /etc/turnserver.conf --turbo

# Turbo + 房间广播 (HMAC Token 模式)
turnserver -c /etc/turnserver.conf --turbo --turbo-rooms \
    --turbo-room-id-provider token_hmac

# Turbo + AF_XDP
sudo turnserver -c /etc/turnserver.conf --turbo \
    --turbo-backend=af_xdp -o -v

# 完全禁用 Turbo (与原版一致)
turnserver -c /etc/turnserver.conf
```

### 12.5 运行时管理

```bash
# 查看状态
curl http://localhost:8080/admin/status

# 查看指标
curl http://localhost:8080/admin/metrics

# 热降级 (通话不中断)
curl -X POST http://localhost:8080/admin/turbo-disable

# 排空下线
curl -X POST http://localhost:8080/admin/drain

# 信号降级
kill -SIGUSR1 $(pidof turnserver)
```

---

## 13. 与原生 coturn 行为对照表

| 特性 | 原生 coturn | coturn-turbo |
| :--- | :--- | :--- |
| **端口占用** | N allocations = N+1 端口 | **始终 1 个端口 (3478)** |
| **ICE relay candidate** | 不同端口地址 | 统一地址，客户端无感 |
| **网络 I/O** | 内核 epoll + sendto/recvfrom | 用户态 io_uring / AF_XDP，降级路径完全一致 |
| **一对一转发** | 支持 | 完全一致 |
| **多人会话** | 上层组合多个 1v1 | 可选房间广播，信令驱动，客户端无感 |
| **运行时降级** | 不支持 | **支持**，SIGUSR1/API 触发，通话不中断 |
| **Per-allocation 监控** | 有限 | 完整的 Prometheus 指标 + 审计日志 |
| **构建** | `./configure && make` | 增加 `--turbo` 选项，不加时结果与原生一致 |
| **配置迁移** | - | 原配置文件直接可用，Turbo 参数增量添加 |
| **灰度回滚** | - | 信令 ICE 候选控制 + Drain 模式 + 热降级 |
| **房间管理** | 无 | 可插拔 Provider: static / token_hmac / lua_script |
| **第三方信令集成** | 标准 TURN REST API | 标准 TURN REST API + Room Provider 扩展 |

---

## 14. 实施路线图

### 阶段一：核心基础 — 纯端口收敛与高性能转发 (6-8 周)

**目标**：解决端口爆炸和性能瓶颈，不引入房间概念，与现有系统零耦合。

| 任务 | 工作量 | 产出 |
| :--- | :--- | :--- |
| 基础设施搭建 | 1 周 | `src/turbo/` 目录结构、`#ifdef` 宏体系、`configure` 选项 |
| 网络后端抽象 | 1 周 | `turbo_netif.h` 接口 + epoll 后端实现 |
| io_uring 后端 | 1 周 | `turbo_iouring.c` |
| 单端口收敛 + 自适应查找 | 2 周 | 三表查找引擎、L1 预热、流量整形器 |
| 动态降级机制 | 1 周 | 保险丝、SIGUSR1 处理、Admin API |
| 测试与性能基准 | 1 周 | 与原生 coturn 对比测试报告 |

**交付物**：
-   `coturn-turbo` 二进制 (默认 io_uring 后端)
-   与原生 coturn 性能对比报告
-   部署文档

**集成要求**：信令和客户端**零改动**。

---

### 阶段二：轻量房间广播 (4-6 周)

**目标**：在阶段一稳定运行后，按需开启多人广播功能，信令服务配合少量开发。

| 任务 | 工作量 | 产出 |
| :--- | :--- | :--- |
| Room Provider 框架 | 1 周 | 抽象接口、配置加载 |
| `static` Provider | 0.5 周 | 用户名直接解析 |
| `token_hmac` Provider | 1 周 | Token 验证、解析、过期检查 |
| 房间成员管理 | 1 周 | RCU 链表、惰性创建、幽灵清理 |
| 广播转发引擎 | 1 周 | 零拷贝克隆、批量发送 |
| 测试 | 0.5 周 | 多人房间集成测试 |

**交付物**：
-   房间广播功能
-   信令服务 Token 生成示例代码 (多语言)
-   Provider 开发文档

**集成要求**：信令服务增加生成 HMAC Token 的逻辑，客户端零改动。

---

### 阶段三：AF_XDP 高性能后端 + 生产加固 (6-8 周)

**目标**：提供极致性能选项，完成生产级运维工具和监控。

| 任务 | 工作量 | 产出 |
| :--- | :--- | :--- |
| XDP BPF 程序 | 1 周 | 端口过滤、XDP_REDIRECT |
| AF_XDP 后端 | 2 周 | UMEM 管理、零拷贝、引用计数 |
| AF_XDP 保险丝 | 1 周 | 队列监控、自动降级 |
| turbo-mirror 镜像 | 1 周 | 内核安全栈可见性 |
| Per-allocation 监控 | 1 周 | Prometheus 指标、审计日志 |
| Drain 模式 + 灰度 SOP | 1 周 | 管理工具和运维文档 |

**交付物**：
-   AF_XDP 后端二进制
-   运维 SOP 和回滚手册
-   生产监控面板 (Grafana Dashboard JSON)

---

## 15. 附录

### 15.1 术语表

| 术语 | 说明 |
| :--- | :--- |
| TURN | Traversal Using Relays around NAT (RFC 8656) |
| STUN | Session Traversal Utilities for NAT |
| Allocation | TURN 服务器上为客户端分配的中继地址 |
| ChannelBind | 客户端绑定信道号到对端地址 |
| Send Indication | 客户端使用 STUN 消息发送数据的方式 |
| SFU | Selective Forwarding Unit，选择性转发单元 |
| io_uring | Linux 异步 I/O 框架 (内核 ≥5.6) |
| AF_XDP | Address Family - eXpress Data Path，高性能内核旁路技术 |
| XDP | eXpress Data Path，Linux 内核可编程快速数据路径 |
| UMEM | User Memory，AF_XDP 用户态与内核共享的内存区域 |
| RCU | Read-Copy-Update，无锁读并发机制 |
| Provider | Room Identity Provider，可插拔的房间身份识别模块 |
| HMAC | Hash-based Message Authentication Code，哈希消息认证码 |
| Drain | 排空模式，拒绝新请求，等待已有会话结束 |

### 15.2 与 `coturn-turbo-终极方案完整技术文档.md` 的主要差异

| 维度 | 终极方案 (v1.0) | 本方案 (v2.0) |
| :--- | :--- | :--- |
| 架构定位 | TURN-SFU 融合服务器 + 独立 Conductor | 纯 TURN 增强 + 可选轻量广播 |
| 房间管理 | 独立 Conductor 分布式调度 | 信令服务为唯一 Owner，coturn-turbo 无状态执行 |
| 信令集成 | HTTP API 创建房间/加入 | 可插拔 Room Identity Provider (static/token_hmac/lua) |
| 网络后端 | DPDK + AF_XDP (编译时二选一) | epoll/io_uring/AF_XDP (运行时动态降级) |
| 降级路径 | 无运行时降级 | SIGUSR1 热降级 + 自动保险丝 |
| 运维能力 | 基础日志 | 完整 Prometheus + Admin API + Drain + 灰度 |
| 代码侵入 | 未详细定义 | 严格 `#ifdef` 隔离，侵入点 < 50 行 |
| 开源策略 | 独立 Conductor 强耦合 | Provider 可插拔，社区零门槛扩展 |

### 15.3 与 `coturn-turbo-SFU中继方案汇总.md` 的关系

本 v2.0 方案采用了方案汇总中**方案 B (Token 路由)** 的核心思想，并将其升华为可插拔的 Room Identity Provider 机制。同时融合了 `收敛端口中继转发方案 v1.2` 的安全审计、运维降级、代码隔离等成熟设计。

### 15.4 参考文献

-   coturn 官方仓库: <https://github.com/coturn/coturn>
-   RFC 8656 (TURN): <https://tools.ietf.org/html/rfc8656>
-   RFC 8829 (ICE): <https://tools.ietf.org/html/rfc8829>
-   io_uring 编程: <https://kernel.dk/io_uring.pdf>
-   AF_XDP 教程: <https://github.com/xdp-project/xdp-tutorial>
-   libbpf 文档: <https://libbpf.readthedocs.io/>
-   gRPC 文档: <https://grpc.io/docs/>
