# coturn-turbo Fast 方案最终技术设计文档 (v2.1)

> **文档状态**：最终版（已完善原 v2.0 方案中的技术缺口）
> **基于**：coturn-turbo-fast实施方案.md (v2.0)
> **变更摘要**：补充 io_uring/libevent 共存机制、单端口收敛的 allocation 一致性保障、降级窗口丢包说明、configure.ac 迁移规则、Provider 超时保护

---

## 目录

1. [项目背景与目标](#1-项目背景与目标)
2. [设计哲学与约束](#2-设计哲学与约束)
3. [总体架构](#3-总体架构)
4. [核心设计一：单端口收敛与自适应快速路径](#4-核心设计一单端口收敛与自适应快速路径)
5. [核心设计二：网络后端抽象与运行时动态降级](#5-核心设计二网络后端抽象与运行时动态降级)
6. [核心设计三：io_uring 与 libevent 共存模型](#6-核心设计三ioring-与-libevent-共存模型)
7. [核心设计四：信令驱动的轻量房间广播](#7-核心设计四信令驱动的轻量房间广播)
8. [核心设计五：Room Identity Provider 可插拔机制](#8-核心设计五room-identity-provider-可插拔机制)
9. [安全与审计设计](#9-安全与审计设计)
10. [客户端兼容性与 SLO](#10-客户端兼容性与-slo)
11. [运维、监控与灰度回滚](#11-运维监控与灰度回滚)
12. [代码结构与上游合并策略](#12-代码结构与上游合并策略)
13. [构建、配置与部署](#13-构建配置与部署)
14. [与原生 coturn 行为对照表](#14-与原生-coturn-行为对照表)
15. [从终极方案迁移说明](#15-从终极方案迁移说明)
16. [附录](#16-附录)

---

## 1. 项目背景与目标

### 1.1 核心痛点

| 痛点 | 描述 | 影响 |
|------|------|------|
| **端口爆炸** | 每个 TURN allocation 独占一个 UDP 端口 | 数万并发时端口管理困难，防火墙规则膨胀 |
| **性能瓶颈** | 依赖内核协议栈逐包系统调用 | CPU 多消耗在内核/用户态切换，单机并发受限 |
| **缺乏广播语义** | 无原生房间概念，多人会话需上层组合多个 1v1 | 上行带宽 N 倍浪费 |
| **运维复杂性** | 端口不固定，原方案依赖独立 Conductor 服务 | 部署依赖重（DPDK/Redis），故障定位困难 |

### 1.2 项目目标

在 **100% 兼容标准 TURN 协议 (RFC 8656)、不强制客户端升级、不破坏 coturn 主线代码结构** 的前提下：

1. **端口收敛**：所有媒体与信令流量复用到 **1 个 UDP 端口**（3478）。
2. **高性能与不退化**：提供 io_uring（默认）/ AF_XDP（可选高性能）后端，最差情况下降级至 epoll 性能与原版完全持平。
3. **松耦合的房间广播**：通过可插拔 Room Identity Provider 机制实现服务器端媒体广播，信令服务**无需改造核心架构**，只需生成带签名的 Token。
4. **运维友好**：支持运行时热降级、无中断灰度回滚、per-allocation 粒度的 Prometheus 监控。
5. **开源可持续**：丢弃 DPDK/Conductor 等高依赖组件，所有新增代码通过 `#ifdef TURBO_FEATURES` 隔离。

---

## 2. 设计哲学与约束

### 2.1 核心设计原则

| 原则 | 说明 |
|------|------|
| **兼容优先** | 任何优化不得破坏标准协议行为，客户端无改动即可使用全部特性 |
| **机制与策略分离** | 框架提供转发机制，具体房间识别策略由可插拔 Provider 决定 |
| **无状态媒体节点** | coturn-turbo 不持有业务状态，所有房间决策权归信令服务 |
| **渐进增强** | 高级特性均为可选，默认行为与原版完全一致 |
| **可逆性** | 任何变更必须支持无损回退 |
| **轻依赖** | 不引入 DPDK、Redis、独立 Conductor 等重型依赖 |

### 2.2 与 v1.0（终极方案）的核心差异

| 维度 | 终极方案 (v1.0) | 本方案 (v2.1) |
|------|----------------|--------------|
| 后端 | DPDK + AF_XDP（编译时二选一） | epoll / io_uring / AF_XDP（运行时动态降级） |
| 降级路径 | 无 | SIGUSR1 热降级 + 自动保险丝 |
| 房间管理 | 独立 Conductor + Redis + HTTP API | 可插拔 Provider（static/token_hmac/lua），信令服务零改动 |
| 主宏 | `TURN_TURBO` / `TURN_USE_DPDK` / `TURN_USE_AFXDP` | `TURBO_FEATURES` |
| configure 选项 | `--use-dpdk` / `--use-afxdp` | `--turbo` / `--turbo-rooms` / `--turbo-backend=` |
| 部署依赖 | DPDK ≥22.07 / Redis / Conductor 服务 | 仅 liburing（可选）/ libbpf+libxdp（可选）|

---

## 3. 总体架构

### 3.1 架构图

```
                         ┌─────────────────────┐
                         │     客户端           │
                         │  (标准 WebRTC 栈)    │
                         └──────────┬──────────┘
                                    │  UDP :3478 (STUN/TURN + 媒体)
                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                        coturn-turbo 节点                         │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │         libevent 线程池 (原 coturn 主路径，不变)             │  │
│  │  - STUN/TURN 信令处理 (Allocate/Refresh/Permission)        │  │
│  │  - TCP/TLS 控制通道                                         │  │
│  │  - Allocation 表写入 (Allocate 成功时)                      │  │
│  │  - Provider 调用 (extract room_id/member_id)               │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │         Turbo Worker 线程 (新增，独立于 libevent)            │  │
│  │  - UDP 3478 快速路径收发 (io_uring / AF_XDP / epoll)        │  │
│  │  - 流量整形器 (STUN/媒体分类)                               │  │
│  │  - 自适应三表查找引擎 (只读 Allocation 表)                   │  │
│  │  - 房间广播 (零拷贝克隆 + 批量发送)                          │  │
│  │  - 审计日志写入                                             │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │         网络后端抽象层 (src/turbo/netif/)                   │  │
│  │  ┌──────────┐  ┌────────────┐  ┌────────────┐             │  │
│  │  │  epoll   │  │  io_uring  │  │   AF_XDP   │             │  │
│  │  │ (保底)   │  │  (默认)    │  │  (最高性能) │             │  │
│  │  └──────────┘  └────────────┘  └────────────┘             │  │
│  │  运行时动态切换: AF_XDP → io_uring → epoll                  │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │         Admin HTTP API (src/turbo/api/, 可选)              │  │
│  │  GET  /admin/status   POST /admin/turbo-disable           │  │
│  │  GET  /admin/metrics  POST /admin/drain                   │  │
│  └────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
         │  信令服务下发带 Token 的 ICE 配置
         ▼
┌─────────────────────────┐
│   外部信令服务 (已有)    │
│  - 生成并下发 HMAC Token │
│  - 房间状态唯一 Owner    │
└─────────────────────────┘
```

### 3.2 数据流

**一对一标准中继（无房间）**：
```
客户端 A → UDP 3478 → [Turbo Worker 收包]
  → [流量整形: 媒体队列]
  → [三表查找 alloc]
  → [Permission 检查]
  → [转发至客户端 B 的 relay addr]
```

**房间广播（启用 Turbo Rooms）**：
```
发布者 A → UDP 3478 → [Turbo Worker 收包]
  → [三表查找 alloc_A]
  → [检查 alloc_A.room_id ≠ 0]
  → [遍历房间成员 RCU 链表]
  → [零拷贝克隆包 × N]
  → [重写各成员目标地址]
  → [批量发送]
  → 接收者 B, C, D...
```

---

## 4. 核心设计一：单端口收敛与自适应快速路径

### 4.1 端口收敛方案

#### 4.1.1 实现原理

传统 coturn 在 `create_relay_connection()` 中为每个 allocation 调用 `create_relay_ioa_sockets()` 创建并绑定新的 UDP socket（随机端口）。Turbo 模式下的改造策略：

**不跳过 `create_relay_connection()`**，而是在其内部 `create_relay_ioa_sockets()` 返回成功后，通过 `#ifdef TURBO_FEATURES` 钩子强制覆写 relay 地址的端口字段为 3478，并将该 allocation 关联的 relay socket 替换为已存在的共享监听 socket（`ioa_socket` ref-counted）：

```c
/* src/server/ns_turn_server.c — create_relay_connection() 末尾 */
#ifdef TURBO_FEATURES
    if (turbo_config.enabled && newelem->s) {
        /* 覆写 relay 端口为 3478，客户端看到固定地址 */
        ioa_addr *relay_addr = get_local_addr_from_ioa_socket(newelem->s);
        addr_set_port(relay_addr, TURBO_RELAY_PORT);  /* 3478 */

        /* 关闭刚创建的独立 relay socket，改用共享监听 socket */
        IOA_CLOSE_SOCKET(newelem->s);
        newelem->s = turbo_get_shared_relay_socket();  /* ref++ */

        /* 在 fastpath 表中预热 L1 缓存 */
        turbo_l1_cache_warmup(ss, relay_addr);
    }
#endif
```

这样做的好处：allocation 结构体完整初始化，coturn 内部的 Permission 检查、ChannelBind、Refresh 等逻辑全部照常工作，只有对外返回的 `XOR-RELAYED-ADDRESS` 端口变为 3478。

#### 4.1.2 共享 relay socket 管理

```c
/* src/turbo/turbo.h */
/*
 * 全局共享 relay socket（ref-counted ioa_socket）
 * 在 turbo_init() 时创建，绑定到 TURBO_RELAY_PORT (3478)
 * 所有 allocation 共享同一个 socket；turbo worker 线程负责收包
 */
extern ioa_socket_handle turbo_shared_relay_socket;

ioa_socket_handle turbo_get_shared_relay_socket(void);
```

**注意**：共享 socket 只用于接收（turbo worker 线程通过 io_uring/epoll 驱动）；发送时每个 allocation 通过 `sendto(fd, ..., &dst_addr)` 指定不同目标地址，天然隔离。

### 4.2 流量整形器

单端口承载所有流量带来 DDoS 放大风险，引入两级流量整形：

```c
/* src/turbo/forward/turbo_shaper.h */
struct turbo_packet_shaper {
    uint64_t stun_pps_limit;     /* STUN 包速率上限，默认 2000 pps */
    uint64_t stun_burst_limit;   /* 突发容忍，默认 4000 */
    _Atomic uint64_t stun_dropped_total;
    _Atomic uint64_t media_passed_total;
};

/* 收包后首先进入整形器，在 turbo worker 线程中调用 */
int turbo_shaper_classify(struct turbo_packet_shaper *shaper,
                          const uint8_t *buf, size_t len);
/* 返回:
 *   SHAPER_DROP         - STUN 限速丢弃
 *   SHAPER_STUN_QUEUE   - 进入 STUN 处理队列（速率受限）
 *   SHAPER_MEDIA_QUEUE  - 进入媒体快速路径（不受限）
 */
```

- **媒体优先**：ChannelData（`0x4000–0x7FFF`）和 Send Indication 数据部分直接进媒体队列，不受 STUN 限速影响。
- **STUN 限速**：消息类型 `0x0000–0x00FF` 的包独立计数，超限丢弃并触发 `turbo_stun_overload` 告警。

### 4.3 自适应三表查找引擎

```c
/* src/turbo/forward/turbo_fastpath.h */
struct turbo_fastpath {
    /* 层 1：信道绑定表 — ChannelData 最快路径 */
    struct turbo_hash_table *channel_table;
    /* Key: hash(src_ip, src_port, channel_no)  Value: alloc_id */

    /* 层 2：L1 快速缓存 — Send Indication 兼容路径 */
    struct turbo_hash_table *l1_cache;
    /* Key: hash(src_ip, src_port)  Value: alloc_id */
    /* 在 Allocate 成功时由 libevent 线程预热，turbo worker 只读 */

    /* 层 3：辅助解析表 — 极端回退 */
    struct turbo_hash_table *username_table;
    /* Key: username_hash  Value: alloc_id */
    /* 命中即触发告警，并回填 L1 缓存 */
};
```

**查找流程**：
```
收包 → 识别消息类型
    │
    ├─ ChannelData (0x4000-0x7FFF):
    │   查 channel_table → 命中返回 alloc，未命中丢弃
    │
    └─ Send Indication (0x0017) / 其他 STUN:
        查 l1_cache → 命中返回 alloc（>99.9%）
        未命中 → 解析 USERNAME → 查 username_table
              → 命中: 回填 l1_cache，返回 alloc，触发告警
              → 未命中: 交还 libevent 标准处理流程
```

#### L1 缓存预热（跨线程写入）

L1 预热发生在 libevent 线程的 Allocate 成功路径上，turbo worker 线程只读该表。写入通过 `pthread_mutex_t` 保护（写入频率极低，不影响媒体转发吞吐）：

```c
/* libevent 线程调用，在 create_relay_connection() 钩子之后 */
void turbo_l1_cache_warmup(ts_ur_super_session *ss,
                           const ioa_addr *client_addr) {
    struct turbo_l1_entry e = {
        .src_ip   = client_addr->sin6.sin6_addr,
        .src_port = client_addr->sin6.sin6_port,
        .alloc_id = ss->id,
    };
    pthread_mutex_lock(&fastpath.l1_write_lock);
    turbo_hash_insert(&fastpath.l1_cache, &e);
    pthread_mutex_unlock(&fastpath.l1_write_lock);
}
```

---

## 5. 核心设计二：网络后端抽象与运行时动态降级

### 5.1 统一后端接口

```c
/* src/turbo/netif/turbo_netif.h */

struct rtp_packet {
    void     *data;
    uint16_t  len;
    uint16_t  headroom;
    _Atomic uint32_t refcount;
    uint64_t  timestamp;
    uint32_t  alloc_id;
    void     *backend_priv;
};

struct turbo_netif_ops {
    int      (*init)       (struct turbo_netif *tif, void *cfg);
    void     (*close)      (struct turbo_netif *tif);
    void     (*suspend)    (struct turbo_netif *tif);  /* 暂停收包 */
    void     (*resume)     (struct turbo_netif *tif);  /* 恢复收包 */
    int      (*recv_pkts)  (struct turbo_netif *tif,
                            struct rtp_packet **pkts, uint16_t max);
    int      (*send_burst) (struct turbo_netif *tif,
                            struct rtp_packet **pkts, uint16_t count);
    struct rtp_packet *(*alloc_pkt)(struct turbo_netif *tif);
    void     (*free_pkt)   (struct turbo_netif *tif, struct rtp_packet *pkt);
    struct rtp_packet *(*clone_pkt)(struct turbo_netif *tif,
                                    struct rtp_packet *pkt);
};

struct turbo_netif {
    struct turbo_netif_ops *ops;
    int      backend_type;   /* TURBO_BACKEND_EPOLL / IO_URING / AF_XDP */
    int      health_status;  /* HEALTH_OK / DEGRADED / FAILED */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t dropped_packets;
    uint64_t degraded_count;
    void    *priv;
};
```

### 5.2 三种后端对比

| 特性 | epoll | io_uring | AF_XDP |
|------|-------|----------|--------|
| 内核版本要求 | 任意 | ≥ 5.6 | ≥ 5.4 + 驱动支持 |
| 编译依赖 | 无额外 | liburing-dev | libbpf, libxdp, clang |
| 零拷贝 | 否 | 部分（MSG_ZEROCOPY） | 是（UMEM 共享） |
| 批量收发 | recvmmsg/sendmmsg | SQE/CQE 批量提交 | RX/TX Ring 批量 |
| 性能增益（vs epoll） | 基线 | ↑ ~8% | ↑ ~48% |
| 运行时切换 | 降级终点 | 可降级到 epoll | 可降级到 io_uring |

### 5.3 运行时动态降级

#### 5.3.1 降级路径

```
AF_XDP → io_uring → epoll（单向，不可逆升级，需重启恢复）
```

#### 5.3.2 降级过程（技术细节）

```c
int turbo_netif_degrade(struct turbo_netif *tif,
                        enum turbo_degrade_trigger trigger) {
    int target;
    switch (tif->backend_type) {
    case TURBO_BACKEND_AF_XDP:   target = TURBO_BACKEND_IO_URING; break;
    case TURBO_BACKEND_IO_URING: target = TURBO_BACKEND_EPOLL;    break;
    default: return -1;  /* 已是 epoll，无法继续降级 */
    }

    /* 1. 暂停当前后端收包（已在 RX ring 中的包可能丢失，见 §5.3.3） */
    tif->ops->suspend(tif);

    /* 2. 释放当前后端资源（AF_XDP: 卸载 XDP 程序 + 释放 UMEM） */
    tif->ops->close(tif);

    /* 3. 初始化目标后端（复用同一个 UDP socket fd） */
    turbo_netif_ops_init(target, tif);
    tif->ops->init(tif, NULL);

    /* 4. 恢复收包 */
    tif->ops->resume(tif);

    /* 5. 更新指标，不需要迁移 allocation 表（查找表不变） */
    log_info("Turbo degraded %d→%d (trigger=%d)",
             tif->backend_type, target, trigger);
    tif->backend_type = target;
    tif->degraded_count++;

    return 0;
}
```

#### 5.3.3 降级窗口丢包说明

> **重要**：降级过程中存在一个 **<100ms 的短暂切换窗口**，在此窗口内：
> - 已进入 AF_XDP RX ring 但未被 worker 线程取出的 in-flight 包**会丢失**。
> - 估计影响量：≤ 1 个 RTT 内的包（通常 1–5 个 RTP 包）。
> - 这是**可接受的**：标准 WebRTC 的抖动缓冲区（jitter buffer）可以吸收此类短暂丢失，不会造成主观可感知的音视频质量下降。
> - 文档和监控中**不应**将此描述为"零丢包"，而应描述为"通话不中断（P99.9）"。

#### 5.3.4 AF_XDP 保险丝

```c
void turbo_afxdp_health_check(struct turbo_netif *tif) {
    struct turbo_afxdp_priv *priv = tif->priv;
    uint32_t ring_full_ms = get_ring_full_duration_ms(priv->rx_ring);

    if (ring_full_ms > 3000) {
        log_warn("AF_XDP RX ring full for %ums, auto-degrading", ring_full_ms);
        turbo_netif_degrade(tif, DEGRADE_AUTO);
        metric_inc("turbo_afxdp_degraded_total");
    }
}
```

#### 5.3.5 SIGUSR1 手动降级

```c
/* src/apps/relay/mainrelay.c */
static void handle_sigusr1(int sig) {
    (void)sig;
    /* 异步安全：只设置标志，由 worker 线程在下一个 poll 周期执行实际降级 */
    atomic_store(&turbo_degrade_requested, 1);
}
```

---

## 6. 核心设计三：io_uring 与 libevent 共存模型

> **这是 v2.0 方案的重要补充，原文档未明确定义此机制。**

### 6.1 线程模型

coturn 原有代码深度依赖 libevent 事件循环处理所有 TCP/UDP 控制信令。Turbo 的快速路径**不替换** libevent，而是在独立的 Turbo Worker 线程中运行，两者通过明确定义的边界协作。

```
┌─────────────────────────────┐    ┌─────────────────────────────┐
│  libevent 线程池（原有）      │    │  Turbo Worker 线程（新增）   │
│                             │    │                             │
│  - TCP/TLS 信令处理          │    │  - UDP 3478 快速路径        │
│  - STUN Allocate/Refresh    │    │  - io_uring / AF_XDP 驱动  │
│  - Allocation 表写入         │────▶│  - Allocation 表只读       │
│  - Provider 调用             │    │  - 房间广播                 │
│  - ChannelBind 管理          │    │  - 审计写入                 │
└─────────────────────────────┘    └─────────────────────────────┘
           │                                     │
           │        共享 Allocation 表           │
           │   (seqlock 保护，写少读多)            │
           └─────────────────────────────────────┘
```

### 6.2 共享 Allocation 表的并发安全

Allocation 表由 libevent 线程独占写入（Allocate/Refresh/Delete 路径），Turbo Worker 只读。使用 **seqlock** 保证读端无锁、写端低频加锁：

```c
/* src/turbo/common/turbo_seqlock.h */
typedef struct {
    _Atomic uint32_t seq;  /* 奇数=写中，偶数=无写 */
} turbo_seqlock_t;

/* 写端（libevent 线程）*/
static inline void turbo_seqlock_write_begin(turbo_seqlock_t *sl) {
    atomic_fetch_add_explicit(&sl->seq, 1, memory_order_release);
}
static inline void turbo_seqlock_write_end(turbo_seqlock_t *sl) {
    atomic_fetch_add_explicit(&sl->seq, 1, memory_order_release);
}

/* 读端（Turbo Worker 线程，重试直到读到一致快照）*/
static inline uint32_t turbo_seqlock_read_begin(turbo_seqlock_t *sl) {
    uint32_t seq;
    do {
        seq = atomic_load_explicit(&sl->seq, memory_order_acquire);
    } while (seq & 1);
    return seq;
}
static inline int turbo_seqlock_read_retry(turbo_seqlock_t *sl, uint32_t seq) {
    return atomic_load_explicit(&sl->seq, memory_order_acquire) != seq;
}
```

### 6.3 Turbo Worker 与 UDP socket 所有权

- Turbo Worker 线程**接管** UDP 3478 socket 的收包权（通过 io_uring 或 AF_XDP）。
- libevent 原有的 UDP 监听器（`udp_services`）在 turbo 模式下**不监听** 3478 端口，改为由 turbo 共享 relay socket 接收，Worker 处理后将需要 STUN 处理的包通过 **无锁环形队列**（`turbo_ring`）投递给 libevent 线程处理。
- 媒体包（ChannelData / Send Indication 数据）**不经过 libevent 线程**，直接在 Worker 线程中完成查找和转发。

```
UDP :3478 入包
    │
    ├─ 媒体包 (ChannelData / Send Indication)
    │   → Turbo Worker 直接转发，不进 libevent
    │
    └─ STUN 控制包 (Allocate/Refresh/ChannelBind 等)
        → 投入 turbo_ring (无锁)
        → libevent 线程从 ring 取出处理
```

### 6.4 启动与关闭序列

```c
/* src/apps/relay/mainrelay.c — turbo_init() */
int turbo_init(void) {
    /* 1. 初始化共享 relay socket（绑定 3478，SO_REUSEPORT） */
    turbo_shared_relay_socket = turbo_create_relay_socket(3478);

    /* 2. 初始化 fastpath 表、房间管理、审计、整形器 */
    turbo_fastpath_init(&g_fastpath);
    turbo_room_mgr_init(&g_room_mgr);
    turbo_audit_init(&g_audit);

    /* 3. 初始化网络后端（根据配置选择 io_uring/AF_XDP/epoll） */
    turbo_netif_init(&g_netif, turbo_config.backend);

    /* 4. 启动 Worker 线程 */
    pthread_create(&g_worker_thread, NULL, turbo_worker_loop, &g_netif);

    /* 5. 注册 SIGUSR1 信号处理 */
    signal(SIGUSR1, handle_sigusr1);

    return 0;
}
```

---

## 7. 核心设计四：信令驱动的轻量房间广播

### 7.1 设计原则

- 信令服务是房间状态的唯一 Owner，coturn-turbo 是无状态媒体执行单元。
- 客户端**零改动**：Token 作为标准 TURN `username` 字段传递，标准 WebRTC 栈无需任何修改。
- 默认不启用：通过 `--turbo-rooms` 配置项按需开启。

### 7.2 房间广播工作流程

```
信令服务                               coturn-turbo
    │                                       │
    │ 1. 生成 HMAC Token                    │
    │    {room_id,member_id,exp}            │
    │                                       │
    │ 2. 下发 ICE 配置给客户端 A            │
    │    username: token                    │
    │                                       │
    │                           客户端 A 发起 Allocate
    │                                       │
    │                           3. 收到 Allocate 请求
    │                              Provider 验证 Token ✓
    │                              解析 room_id=123
    │                              alloc_A.room_id = 123
    │                              加入 room123 广播成员表
    │                              返回 Allocate Success
    │                              (relay addr: server_ip:3478)
    │                                       │
    │              (同理处理 B、C...)         │
    │                                       │
    │                           4. A 发送媒体包
    │                              Turbo Worker 三表查找 → alloc_A
    │                              alloc_A.room_id = 123 ≠ 0
    │                              遍历 room123 RCU 成员链表
    │                              零拷贝克隆 × (N-1)
    │                              重写目标地址 → 批量发送
```

### 7.3 房间成员管理数据结构

```c
/* src/turbo/room/turbo_room.h */

struct turbo_room_member {
    uint32_t             alloc_id;
    char                 member_id[64];
    struct sockaddr_in6  relay_addr;
    uint64_t             joined_at;
    uint64_t             last_forward;
    struct list_head     node;           /* RCU 链表节点 */
};

struct turbo_room {
    char                 room_id[64];
    _Atomic uint32_t     member_count;
    uint32_t             max_members;    /* 默认 50，防广播风暴 */
    uint64_t             created_at;
    struct list_head     members;
    pthread_rwlock_t     members_lock;   /* 仅加入/离开时持写锁 */
    _Atomic uint64_t     total_broadcasts;
    _Atomic uint64_t     total_forwarded_pkts;
    UT_hash_handle       hh;
};
```

### 7.4 广播执行

```c
int turbo_room_broadcast(struct turbo_netif *netif,
                         struct turbo_room *room,
                         struct rtp_packet *src_pkt,
                         uint32_t src_alloc_id) {
    uint32_t forwarded = 0;

    pthread_rwlock_rdlock(&room->members_lock);
    list_for_each_entry_rcu(member, &room->members, node) {
        if (member->alloc_id == src_alloc_id)
            continue;

        struct rtp_packet *clone = netif->ops->clone_pkt(netif, src_pkt);
        if (!clone) continue;

        clone->dst_addr = member->relay_addr;
        netif->ops->send_burst(netif, &clone, 1);
        forwarded++;
    }
    pthread_rwlock_unlock(&room->members_lock);

    atomic_fetch_add(&room->total_broadcasts, 1, __ATOMIC_RELAXED);
    atomic_fetch_add(&room->total_forwarded_pkts, forwarded, __ATOMIC_RELAXED);
    return forwarded;
}
```

### 7.5 房间生命周期

- **惰性创建**：第一个携带某 `room_id` 的 allocation 建立时自动创建房间（引用计数=1）。
- **自动销毁**：最后一个 allocation 销毁时（引用计数归零），自动销毁房间上下文。
- **幽灵成员防护**：allocation 超时销毁时，`turbo_room_remove_member()` 作为销毁回调被 libevent 线程自动调用。
- **成员上限**：默认 50 人，超过后拒绝新成员并记录 `turbo_room_member_overflow` 告警。

---

## 8. 核心设计五：Room Identity Provider 可插拔机制

### 8.1 抽象接口

```c
/* src/turbo/room/turbo_room_provider.h */

struct turbo_room_info {
    char     room_id[64];
    char     member_id[64];
    uint64_t expiry;           /* 0 = 不限期 */
};

struct turbo_room_provider_ops {
    const char *name;

    int  (*init)   (void *config);

    /* 返回值:
     *   0  - 成功提取房间信息
     *  -1  - 非房间流量（继续标准 TURN 处理）
     *  -2  - 验证失败（拒绝 Allocate）
     */
    int  (*extract)(const char *username,
                    const char *realm,
                    const char *credential,
                    const struct sockaddr_in6 *client_addr,
                    struct turbo_room_info *info);

    void (*deinit) (void);
};
```

### 8.2 内置 Provider 实现

#### 8.2.1 `static` Provider

用于开发测试或受信任内网环境，直接解析 `username` 格式 `room<ID>:user<ID>`：

```c
/* 支持格式: "room123:userA" 或 "room123:456" */
int provider_static_extract(const char *username, ...) {
    if (sscanf(username, "room%63[^:]:%63s",
               info->room_id, info->member_id) == 2)
        return 0;
    return -1;
}
```

#### 8.2.2 `token_hmac` Provider（推荐生产环境）

类 JWT 格式的 HMAC-SHA256 签名 Token，无需访问外部数据库：

**Token 结构**：
```
{base64url(header)}.{base64url(body)}.{base64url(HMAC-SHA256(header.body, secret))}

header = {"alg":"HS256","typ":"JWT-like"}
body   = {"room_id":"123","member_id":"userA","exp":1678886400}
```

**验证逻辑关键点**：
1. 使用 `CRYPTO_memcmp()`（OpenSSL）进行**常数时间 HMAC 比对**，防止时序攻击。
2. 检查 `exp` 字段防止重放。
3. 签名验证失败返回 -2（触发 401）；Token 格式错误返回 -1（继续标准 TURN 处理）。

```c
int provider_token_hmac_extract(const char *username, ...) {
    char header[256], body[512], sig_recv[256];
    if (split_jwt(username, header, body, sig_recv) != 0)
        return -1;  /* 格式不符，当作非房间流量 */

    /* 重新计算签名 */
    unsigned char sig_computed[32];
    char sig_input[800];
    snprintf(sig_input, sizeof(sig_input), "%s.%s", header, body);
    HMAC_SHA256(hmac_secret, sig_input, sig_computed);

    /* 常数时间比对 */
    unsigned char sig_decoded[32];
    base64url_decode(sig_recv, sig_decoded, 32);
    if (CRYPTO_memcmp(sig_computed, sig_decoded, 32) != 0) {
        log_warn("HMAC verification failed from %s", addr_to_str(client_addr));
        return -2;
    }

    /* 解析 body */
    json_parse(body, &info->room_id, &info->member_id, &info->expiry);
    if (info->expiry > 0 && time(NULL) > (time_t)info->expiry)
        return -2;

    return 0;
}
```

**信令侧 Token 生成示例（Node.js）**：
```javascript
const crypto = require('crypto');

function generateRoomToken(roomId, memberId, secret, ttl = 3600) {
    const header = Buffer.from(JSON.stringify({alg:'HS256',typ:'JWT-like'}))
                          .toString('base64url');
    const body   = Buffer.from(JSON.stringify({
        room_id: roomId, member_id: memberId,
        exp: Math.floor(Date.now()/1000) + ttl
    })).toString('base64url');
    const sig = crypto.createHmac('sha256', secret)
                      .update(`${header}.${body}`)
                      .digest('base64url');
    return `${header}.${body}.${sig}`;
}

// 下发给客户端
const iceServers = [{
    urls: "turn:server:3478",
    username: generateRoomToken("123", "userA", SHARED_SECRET),
    credential: "dummy"
}];
```

#### 8.2.3 `lua_script` Provider（可选，高灵活性）

允许不修改 C 源码即可对接任意鉴权系统：

```c
int provider_lua_extract(const char *username, ...) {
    /* 超时保护：Provider 必须在 50ms 内返回
     * 超时则返回 -1（回退标准 TURN 处理），不阻塞 Allocate */
    struct sigaction sa_old;
    turbo_lua_set_timeout(50);  /* 50ms alarm */

    lua_getglobal(L, "extract_room_info");
    lua_pushstring(L, username);
    lua_pushstring(L, realm);
    lua_pushstring(L, addr_to_str(client_addr));

    int ret = lua_pcall(L, 3, 2, 0);
    turbo_lua_clear_timeout();

    if (ret != LUA_OK || lua_isnil(L, -2))
        return -1;

    strncpy(info->room_id,   lua_tostring(L, -2), 63);
    strncpy(info->member_id, lua_tostring(L, -1), 63);
    return 0;
}
```

> **Lua Provider 约束**：调用发生在 libevent relay 线程（Allocate 处理路径），**必须同步快速完成**，不允许阻塞性 I/O 操作。若需访问 Redis/HTTP API，应在信令侧完成后将结果编码进 Token 传入，而不是在 Provider 内部实时查询。超时上限 50ms，超时后降级为 -1（非房间流量）。

### 8.3 在 Allocate 流程中的集成点

```c
/* src/server/ns_turn_server.c — Allocate 处理 */
#ifdef TURBO_FEATURES
    if (turbo_config.rooms_enabled && turbo_config.room_provider) {
        struct turbo_room_info info;
        int ret = turbo_config.room_provider->extract(
            ss->username, ss->realm, ss->credential,
            &ss->client_addr, &info);

        if (ret == 0) {
            allocation_set_room_id(alloc, info.room_id, info.member_id);
            turbo_room_add_member(alloc);
            log_info("alloc %u joined room %s as %s",
                     alloc->id, info.room_id, info.member_id);
        } else if (ret == -2) {
            send_allocate_error(ss, 401, "Token verification failed");
            return -1;
        }
        /* ret == -1: 非房间流量，继续标准 TURN 处理 */
    }
    /* 单端口收敛：覆写 relay 端口为 3478 */
    turbo_override_relay_port(ss, TURBO_RELAY_PORT);
    turbo_l1_cache_warmup(ss, &ss->client_addr);
#endif
```

---

## 9. 安全与审计设计

### 9.1 威胁模型

| 威胁 | 等级 | 缓解措施 |
|------|------|---------|
| 单端口 DDoS 放大 | 高 | STUN/媒体队列隔离 + PPS 限速默认 2000/s |
| Token 伪造/篡改 | 高 | HMAC-SHA256 + 常数时间比对 |
| Token 重放 | 中 | Token 内置 `exp`，服务端检查 |
| 房间越权 | 高 | `room_id` 受签名保护，不可篡改 |
| 广播风暴 | 中 | 成员上限硬限制（默认 50） |
| ChannelData 无用户标识 | 高 | 审计钩子记录 alloc_id→username 映射 |
| Lua 脚本阻塞 | 中 | 50ms 超时保护 |
| 共享密钥泄露 | 高 | 文件权限 600；支持从环境变量读取 |
| AF_XDP 降级窗口丢包 | 低 | 文档化，WebRTC jitter buffer 可吸收 |

### 9.2 审计钩子

```c
/* src/turbo/forward/turbo_audit.h */
#define TURBO_AUDIT_BUF_SIZE 4096

struct turbo_audit_event {
    uint64_t            timestamp_ns;
    uint32_t            alloc_id;
    char                username[128];
    struct sockaddr_in6 src_addr;
    struct sockaddr_in6 dst_addr;
    uint16_t            pkt_len;
    uint8_t             direction;   /* 0=inbound, 1=outbound */
    uint8_t             pkt_type;    /* ChannelData/SendIndication/RTP */
};

struct turbo_audit_ring {
    struct turbo_audit_event events[TURBO_AUDIT_BUF_SIZE];
    _Atomic uint32_t head;
    _Atomic uint32_t dropped;
    int      unix_sock_fd;
    uint32_t flush_interval_ms;   /* 默认 10000ms */
};
```

---

## 10. 客户端兼容性与 SLO

### 10.1 兼容性矩阵

| 客户端类型 | 一对一 TURN | 房间广播 | 说明 |
|------------|------------|---------|------|
| Chrome/Firefox/Safari 15+ | ✅ | ✅ | Token 作为 username 传入 |
| Android/iOS Native | ✅ | ✅ | 同上 |
| 仅 Send Indication 客户端 | ✅ | ✅ | L1 预热保证首包延迟不退化 |
| 仅 ChannelBind 客户端 | ✅ | ✅ | 信道表快速路径 |

### 10.2 服务等级目标（SLO）

| 指标 | 目标值 | 测量方式 |
|------|--------|---------|
| 一对一转发延迟（P50） | ≤ 原生 epoll 的 110% | 端到端 RTT 测试 |
| Send Indication 首包延迟 | < 150µs | 服务器内部计时 |
| 房间广播额外延迟 | < 50µs | 入站到最后一个克隆包发出 |
| 快速路径命中率 | > 99.9% | fastpath_hits / (hits + l1_miss) |
| 运行时降级（通话中断率） | < 0.1%（P99.9 不中断） | 实际测试，见§5.3.3 |
| AF_XDP → io_uring 降级耗时 | < 100ms | 从触发到恢复收包 |

---

## 11. 运维、监控与灰度回滚

### 11.1 Prometheus 指标

| 指标名称 | 类型 | 说明 |
|---------|------|------|
| `turbo_allocations_current` | Gauge | 当前活跃 allocation 数 |
| `turbo_fastpath_hits_total` | Counter | 快速路径命中次数（label: channelbind/l1cache/username） |
| `turbo_l1_miss_total` | Counter | L1 缓存未命中（应接近 0） |
| `turbo_stun_overload_drops_total` | Counter | STUN 限速丢弃数 |
| `turbo_forwarded_bytes_total` | Counter | 转发字节数（label: username） |
| `turbo_room_members_current` | Gauge | 各房间当前成员数（label: room_id） |
| `turbo_backend` | Gauge | 当前后端：3=epoll, 2=io_uring, 1=AF_XDP |
| `turbo_degraded_total` | Counter | 降级次数（label: trigger=auto/manual/drain） |
| `turbo_degrade_window_dropped_pkts` | Counter | 降级窗口估算丢包数 |
| `turbo_audit_dropped_total` | Counter | 审计日志溢出丢弃数 |

### 11.2 Admin HTTP API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/admin/metrics` | GET | Prometheus 格式指标 |
| `/admin/status` | GET | JSON 运行状态 |
| `/admin/turbo-disable` | POST | 运行时降级到 epoll |
| `/admin/turbo-enable` | POST | 重新启用（需配置中指定后端）|
| `/admin/drain` | POST | 进入排空模式 |
| `/admin/drain/status` | GET | 排空进度 |

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
  "uptime_seconds": 86400,
  "degraded_count": 0
}
```

### 11.3 紧急回滚 SOP

| 步骤 | 操作 | 影响 |
|------|------|------|
| 1 | 信令层移除 turbo 节点 ICE 候选 | 新用户不再连接 |
| 2 | `kill -SIGUSR1 $(pidof turnserver)` | <100ms 内降级到 epoll，P99.9 通话不中断 |
| 3 | 验证 `turbo_backend` 指标变为 3 | 确认降级成功 |
| 4 | 排空后替换为旧版二进制 | 无影响 |

---

## 12. 代码结构与上游合并策略

### 12.1 目录结构

```
coturn-turbo/
├── configure                      [调整] 新增 --turbo/--turbo-rooms/--turbo-backend 选项
│                                         删除 --use-dpdk/--use-afxdp
├── Makefile                       [调整] 引入 src/turbo/ 新目录结构
├── src/
│   ├── server/
│   │   ├── ns_turn_allocation.h   [调整] #ifdef TURBO_FEATURES 扩展 room_id/member_id
│   │   ├── ns_turn_allocation.c   [调整] allocation_set_room_id()
│   │   └── ns_turn_server.c       [调整] Allocate 钩子（<30 行新增）
│   ├── apps/relay/
│   │   ├── mainrelay.c            [调整] turbo_init/deinit，宏改为 TURBO_FEATURES
│   │   └── netengine.c            [调整] UDP 收发点增加快速路径入口
│   └── turbo/                     [新增核心库]
│       ├── turbo.h                [新增] 主开关和全局接口
│       ├── netif/
│       │   ├── turbo_netif.h/c    [新增] 后端抽象接口 + 降级逻辑
│       │   ├── turbo_epoll.c      [新增] epoll 后端
│       │   ├── turbo_iouring.c    [新增] io_uring 后端
│       │   ├── turbo_af_xdp.c/h   [改造] 加入保险丝，适配新接口
│       │   └── xdp_prog.c         [保留] XDP eBPF 过滤程序
│       ├── forward/
│       │   ├── turbo_fastpath.h/c [新增] 三表查找引擎 + L1 预热
│       │   ├── turbo_shaper.h/c   [新增] 流量整形器
│       │   ├── turbo_switch.h/c   [改造] 零拷贝克隆，适配新接口
│       │   └── turbo_audit.h/c    [新增] 审计钩子
│       ├── room/
│       │   ├── turbo_room.h/c     [重写] RCU 链表，惰性创建
│       │   ├── turbo_room_provider.h [新增] Provider 抽象接口
│       │   ├── provider_static.c  [新增]
│       │   ├── provider_token_hmac.c [新增]
│       │   └── provider_lua.c     [新增，可选]
│       ├── api/
│       │   └── turbo_api.h/c      [重写] 新端点，删除 /v1/room/*
│       └── common/
│           ├── turbo_hash.h       [保留/迁移]
│           ├── turbo_ring.h       [新增] 无锁环形队列（Worker↔libevent）
│           ├── turbo_seqlock.h    [新增] seqlock（allocation 表并发保护）
│           └── turbo_json.h       [保留/迁移]
│
│   [删除]
│   ├── apps/conductor/            整个目录删除
│   └── turbo/network/turbo_dpdk.c/h  删除
│
├── conf/
│   └── turbo.conf.example         [重写]
└── docs/
    ├── TURBO.md                   [更新]
    └── TURBO_ROOM_PROVIDER.md     [更新]
```

### 12.2 上游侵入点清单

所有对 coturn 原有代码的修改均通过 `#ifdef TURBO_FEATURES` 保护，合计 < 50 行：

| 文件 | 修改内容 | 行数 |
|------|---------|------|
| `configure` | 新增 turbo 选项，删除 dpdk/afxdp 选项 | ~30 |
| `ns_turn_allocation.h` | 末尾增加 `room_id`/`member_id` 字段 | 5 |
| `ns_turn_allocation.c` | `allocation_set_room_id()` 函数 | 8 |
| `ns_turn_server.c` | Allocate 路径增加 Provider 调用 + 端口覆写钩子 | 25 |
| `mainrelay.c` | `turbo_init()`/`turbo_deinit()` 调用 + 新选项解析 | 15 |
| `netengine.c` | UDP 收发处增加快速路径入口 | 10 |

---

## 13. 构建、配置与部署

### 13.1 configure 选项变更

| 旧选项（终极方案） | 新选项（本方案） |
|-----------------|--------------|
| `--use-dpdk` | 删除 |
| `--use-afxdp` | 删除 |
| — | `--turbo`（启用 Turbo，默认 io_uring 后端） |
| — | `--turbo-rooms`（启用房间广播） |
| — | `--turbo-backend=io_uring\|af_xdp`（选择高性能后端）|

**编译宏变更**：

| 旧宏 | 新宏 |
|------|------|
| `TURN_TURBO` | `TURBO_FEATURES` |
| `TURN_USE_DPDK` | 删除 |
| `TURN_USE_AFXDP` | `TURBO_AFXDP`（仅在 `--turbo-backend=af_xdp` 时定义）|

### 13.2 编译

```bash
# 恢复 autotools 环境
autoreconf -fi

# 标准 coturn（无 Turbo，二进制与上游完全一致）
./configure && make -j$(nproc)

# Turbo + io_uring（推荐默认）
./configure --turbo && make -j$(nproc)

# Turbo + AF_XDP（极致性能，需网卡驱动支持）
./configure --turbo --turbo-backend=af_xdp && make -j$(nproc)

# Turbo + io_uring + 房间广播
./configure --turbo --turbo-rooms && make -j$(nproc)
```

### 13.3 配置参考

```ini
# /etc/turnserver.conf — coturn-turbo 生产配置

# === 基础 TURN ===
listening-port=3478
listening-ip=0.0.0.0
relay-ip=<YOUR_PUBLIC_IP>
realm=north
lt-cred-mech
user=legacy_user:legacy_password

# === Turbo 核心 ===
turbo
turbo-backend=io_uring          # io_uring（默认）| af_xdp
turbo-l1-warmup=enable
turbo-api-port=8080
# turbo-api-listen-ip=127.0.0.1  # 建议仅内网

# === AF_XDP 配置（仅 turbo-backend=af_xdp 时有效）===
# turbo-afxdp-mode=auto           # auto | drv | skb
# turbo-xdp-iface=eth0

# === 房间广播（可选）===
# turbo-rooms
# turbo-room-id-provider=token_hmac   # static | token_hmac | lua_script
# turbo-room-token-secret=<SECRET>    # 与信令服务共享
# turbo-room-token-expiry=3600
# turbo-room-max-members=50

# === 审计（可选）===
# turbo-audit-log=unix:///var/run/turn-audit.sock

# === 日志 ===
verbose
log-file=/var/log/turnserver/turbo.log
```

---

## 14. 与原生 coturn 行为对照表

| 特性 | 原生 coturn | coturn-turbo |
|------|------------|--------------|
| 端口占用 | N allocations = N+1 端口 | **始终 1 个端口（3478）** |
| 网络 I/O | 内核 epoll + sendto/recvfrom | io_uring / AF_XDP / epoll 可选 |
| 多人会话 | 上层组合多个 1v1 | 可选房间广播，信令驱动 |
| 运行时降级 | 不支持 | **SIGUSR1/API 触发，P99.9 不中断** |
| per-allocation 监控 | 有限 | 完整 Prometheus 指标 + 审计日志 |
| 构建 | `./configure && make` | 增加 `--turbo` 选项，不加时与原生完全一致 |
| 房间管理 | 无 | 可插拔 Provider：static/token_hmac/lua |
| Conductor 依赖 | 无 | **无**（已从终极方案中移除） |
| Redis 依赖 | 无 | **无** |
| DPDK 依赖 | 无 | **无** |

---

## 15. 从终极方案迁移说明

### 15.1 需要删除的组件

| 组件 | 原因 |
|------|------|
| `src/apps/conductor/` 整个目录 | 信令服务接管房间状态，无需独立调度服务 |
| `src/turbo/network/turbo_dpdk.c/h` | 丢弃 DPDK 依赖 |
| `src/apps/relay/turbo_forward.c/h` | 被 `turbo_fastpath.c` + `turbo_shaper.c` 替代 |
| `src/apps/relay/turbo_core.c/h` | 功能拆分到 `turbo.h` + 各子模块 |
| Admin API `/v1/room/*` 端点 | 房间由 Provider 机制隐式管理，不再需要 HTTP 驱动 |

### 15.2 宏与选项替换对照

```bash
# configure.ac 中替换
--use-dpdk       → 删除
--use-afxdp      → 删除
                 → 新增 --turbo / --turbo-rooms / --turbo-backend=

# C 代码中替换（全局搜索替换）
TURN_TURBO       → TURBO_FEATURES
TURN_USE_DPDK    → 删除相关 #ifdef 块
TURN_USE_AFXDP   → TURBO_AFXDP
```

### 15.3 可复用的代码模块

| 模块 | 复用程度 | 说明 |
|------|---------|------|
| `turbo_af_xdp.c/h` | 改造（约 60% 保留） | 加入保险丝，适配新接口 |
| `turbo_hash.h` | 直接迁移 | 移到 `src/turbo/common/` |
| `turbo_json.h` | 直接迁移 | 移到 `src/turbo/common/` |
| `turbo_rcu.c/h` | 直接迁移 | 移到 `src/turbo/common/` |
| `turbo_mempool.c/h` | 改造 | 适配新 `rtp_packet` 结构 |
| `turbo_switch.c/h` | 改造（约 70% 保留） | 适配新后端接口 |
| `turbo_api.c/h` | 重写（保留 libevent HTTP 框架） | 端点完全替换 |
| `turbo_room.c/h` | 重写（保留 RCU 思路） | 改为 list_head + rwlock 实现 |

---

## 16. 附录

### 16.1 术语表

| 术语 | 说明 |
|------|------|
| TURBO_FEATURES | 本方案的编译控制宏，取代旧方案的 TURN_TURBO |
| Provider | Room Identity Provider，可插拔的房间身份识别模块 |
| L1 Cache | Turbo 快速路径的二级缓存（src_ip+src_port → alloc_id） |
| seqlock | 顺序锁，用于 Allocation 表的读写并发保护 |
| Turbo Worker | 独立 UDP 快速路径线程，与 libevent 线程池分离 |
| 降级窗口 | AF_XDP→io_uring 切换期间 <100ms 的短暂丢包窗口 |
| HMAC | Hash-based Message Authentication Code |
| Drain | 排空模式：拒绝新 Allocate，等待已有会话自然结束 |
| RCU | Read-Copy-Update，无锁读并发机制 |

### 16.2 参考文献

- coturn 官方仓库：https://github.com/coturn/coturn
- RFC 8656 (TURN)：https://tools.ietf.org/html/rfc8656
- io_uring 文档：https://kernel.dk/io_uring.pdf
- AF_XDP 教程：https://github.com/xdp-project/xdp-tutorial
- OpenSSL CRYPTO_memcmp：https://www.openssl.org/docs/man3.0/man3/CRYPTO_memcmp.html
