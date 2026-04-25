# coturn-turbo Fast 方案任务计划

> **版本**：v1.0
> **基于设计文档**：coturn-turbo-fast-final-design.md (v2.1)
> **当前代码基线**：feature/turbo-fast 分支（已实现终极方案 v1.0）
> **目标**：将现有代码重构为 fast 方案（v2.1），删除 DPDK/Conductor，建立 io_uring/epoll 后端和 Provider 机制

---

## 整体阶段规划

```
Phase 1: 基础结构重整（清理 + 迁移）          ← 阻塞后续所有阶段
Phase 2: 网络后端层（epoll / io_uring / AF_XDP）← 阻塞 Phase 3
Phase 3: 快速路径层（fastpath / shaper / switch）← 阻塞 Phase 4
Phase 4: 房间广播与 Provider 机制             ← 依赖 Phase 3
Phase 5: 运维与监控（API / 审计 / 配置）       ← 可与 Phase 4 并行
```

---

## Phase 1：基础结构重整

> **目标**：清理终极方案遗留代码，建立新目录结构，更新编译系统和宏定义。完成后代码应能正常编译（turbo 功能临时禁用）。

### T1.1 删除 Conductor 组件

**文件操作**：删除 `src/apps/conductor/` 整个目录（含 `main.c`、`room_manager.c`、`signal_handler.c`、`api_server.c`、`redis_sync.c`、`CMakeLists.txt`）

**同步清理**：
- `CMakeLists.txt` 根文件中删除 `add_subdirectory(src/apps/conductor)` 相关行
- 检查 `src/apps/relay/mainrelay.c` 中是否有对 conductor 头文件的 `#include`，有则删除

**验收**：`find . -path "*/conductor*" | wc -l` 返回 0

---

### T1.2 删除 DPDK 相关代码

**文件操作**：删除以下文件：
- `src/turbo/network/turbo_dpdk.c`
- `src/turbo/network/turbo_dpdk.h`

**同步清理**：
- `src/turbo/network/CMakeLists.txt`：删除 turbo_dpdk.c 编译条目
- `src/apps/relay/mainrelay.c`：删除 `#ifdef TURN_USE_DPDK` 代码块
- `src/turbo/forward/turbo_room.h`：删除 `#ifdef TURN_USE_DPDK` 的 `rte_hash`/`rte_rcu_qsbr` 引用
- `src/apps/relay/turbo_api.c`：删除 `#ifdef TURN_USE_DPDK` 的 `rte_hash.h` 引用
- 全局搜索 `TURN_USE_DPDK` 并删除相关代码块

**验收**：`grep -r "DPDK\|rte_\|TURN_USE_DPDK" src/` 返回空

---

### T1.3 删除旧 turbo_forward / turbo_core 文件

这两个文件的功能将被拆分到 Phase 3 的新模块中：

**文件操作**：删除：
- `src/apps/relay/turbo_forward.c`
- `src/apps/relay/turbo_forward.h`
- `src/apps/relay/turbo_core.c`
- `src/apps/relay/turbo_core.h`

**同步清理**：
- `src/apps/relay/mainrelay.c`：删除对上述头文件的 `#include`，以及 `turbo_core_init()`/`turbo_core_start()` 等调用（这些调用会在 Phase 2 由 `turbo_init()` 重新引入）
- 对应 `Makefile` 或 `CMakeLists.txt` 中删除相关编译目标

---

### T1.4 重组 src/turbo/ 目录结构

按设计文档 §12.1 建立新目录结构：

```bash
# 新建目录
mkdir -p src/turbo/netif
mkdir -p src/turbo/forward
mkdir -p src/turbo/room
mkdir -p src/turbo/api
mkdir -p src/turbo/common

# 迁移工具库（保持文件内容不变，只移动位置）
mv src/turbo/utils/turbo_hash.h   src/turbo/common/turbo_hash.h
mv src/turbo/utils/turbo_hash.c   src/turbo/common/turbo_hash.c
mv src/turbo/utils/turbo_json.h   src/turbo/common/turbo_json.h
mv src/turbo/utils/turbo_json.c   src/turbo/common/turbo_json.c
mv src/turbo/utils/turbo_rcu.h    src/turbo/common/turbo_rcu.h
mv src/turbo/utils/turbo_rcu.c    src/turbo/common/turbo_rcu.c
rmdir src/turbo/utils

# 迁移网络后端（现有文件保留，后续 T2.x 再改造内容）
mv src/turbo/network/turbo_netif.h    src/turbo/netif/turbo_netif.h
mv src/turbo/network/turbo_netif.c    src/turbo/netif/turbo_netif.c
mv src/turbo/network/turbo_af_xdp.h  src/turbo/netif/turbo_af_xdp.h
mv src/turbo/network/turbo_af_xdp.c  src/turbo/netif/turbo_af_xdp.c
mv src/turbo/network/turbo_mempool.h  src/turbo/netif/turbo_mempool.h
mv src/turbo/network/turbo_mempool.c  src/turbo/netif/turbo_mempool.c
mv src/turbo/network/turbo_port.h     src/turbo/netif/turbo_port.h
mv src/turbo/network/turbo_port.c     src/turbo/netif/turbo_port.c
mv src/turbo/network/xdp_prog.c       src/turbo/netif/xdp_prog.c
rmdir src/turbo/network

# 迁移转发模块
mv src/turbo/forward/turbo_switch.h  src/turbo/forward/turbo_switch.h
mv src/turbo/forward/turbo_switch.c  src/turbo/forward/turbo_switch.c
# turbo_room.h/c 留在 forward/ 下暂时不动，Phase 4 重写后移到 room/
# turbo_fec.h/c 保持不变（默认禁用）

# 迁移 API
mv src/apps/relay/turbo_api.h  src/turbo/api/turbo_api.h
mv src/apps/relay/turbo_api.c  src/turbo/api/turbo_api.c
```

更新所有文件中的 `#include` 路径以匹配新目录结构。

---

### T1.5 更新编译宏：TURN_TURBO → TURBO_FEATURES

**全局搜索替换**（仅在 `#ifdef`/`#if defined()` 上下文中替换，不替换注释或字符串）：

```bash
# 在所有 .c/.h 文件中
sed -i 's/defined(TURN_TURBO)/defined(TURBO_FEATURES)/g' src/**/*.{c,h}
sed -i 's/TURN_USE_AFXDP/TURBO_AFXDP/g' src/**/*.{c,h}
```

**手动验证**：检查替换结果不引入语法错误，特别是 `mainrelay.c` 和 `ns_turn_server.c`。

---

### T1.6 重写 configure.ac turbo 相关部分

**删除以下 configure.ac 内容**：
- `--with-dpdk` / `--use-dpdk` 检测块
- `--use-afxdp` 检测块（保留 libbpf/libxdp 检测，但改为 `--turbo-backend=af_xdp` 触发）
- `AC_DEFINE([TURN_USE_DPDK], ...)` 和 `AC_DEFINE([TURN_USE_AFXDP], ...)`

**新增以下 configure.ac 内容**：

```m4
# --turbo: 启用 Turbo 扩展
AC_ARG_ENABLE([turbo],
    [AS_HELP_STRING([--turbo], [Enable Turbo high-performance extensions])],
    [enable_turbo=yes], [enable_turbo=no])

# --turbo-rooms: 启用房间广播（依赖 --turbo）
AC_ARG_ENABLE([turbo-rooms],
    [AS_HELP_STRING([--turbo-rooms], [Enable Turbo room broadcasting])],
    [enable_turbo_rooms=yes], [enable_turbo_rooms=no])

# --turbo-backend=io_uring|af_xdp: 选择高性能后端（默认 io_uring）
AC_ARG_WITH([turbo-backend],
    [AS_HELP_STRING([--turbo-backend=BACKEND],
        [Turbo network backend: io_uring (default) or af_xdp])],
    [turbo_backend=$withval], [turbo_backend=io_uring])

AS_IF([test "x$enable_turbo" = "xyes"], [
    AC_DEFINE([TURBO_FEATURES], [1], [Enable Turbo extensions])

    AS_IF([test "x$turbo_backend" = "xio_uring"], [
        PKG_CHECK_MODULES([LIBURING], [liburing >= 2.0])
        AC_DEFINE([TURBO_IOURING], [1], [Use io_uring backend])
    ])

    AS_IF([test "x$turbo_backend" = "xaf_xdp"], [
        PKG_CHECK_MODULES([LIBBPF], [libbpf >= 0.6])
        PKG_CHECK_MODULES([LIBXDP],  [libxdp >= 1.2])
        AC_DEFINE([TURBO_AFXDP], [1], [Use AF_XDP backend])
        AC_CHECK_PROG([CLANG], [clang], [clang])
    ])

    AS_IF([test "x$enable_turbo_rooms" = "xyes"], [
        AC_DEFINE([TURBO_ROOMS], [1], [Enable Turbo room broadcasting])
    ])
])
```

**验收**：`./configure --turbo` 和 `./configure --turbo --turbo-backend=af_xdp` 均能正常完成，生成正确的 `config.h`。

---

### T1.7 新建 src/turbo/turbo.h 主头文件

```c
/* src/turbo/turbo.h */
#ifndef TURBO_H
#define TURBO_H

#ifdef TURBO_FEATURES

#include "netif/turbo_netif.h"
#include "forward/turbo_fastpath.h"
#include "forward/turbo_shaper.h"
#include "common/turbo_ring.h"

/* 全局实例（在 turbo_init() 中初始化） */
extern struct turbo_netif      g_turbo_netif;
extern struct turbo_fastpath   g_turbo_fastpath;
extern struct turbo_pkt_shaper g_turbo_shaper;

/* 共享 relay socket（所有 allocation 共用）*/
extern ioa_socket_handle       turbo_shared_relay_socket;
#define TURBO_RELAY_PORT       3478

/* 主接口 */
int  turbo_init(void);
void turbo_deinit(void);

ioa_socket_handle turbo_get_shared_relay_socket(void);
void turbo_l1_cache_warmup(ts_ur_super_session *ss,
                           const ioa_addr *client_addr);
void turbo_override_relay_port(ts_ur_super_session *ss, uint16_t port);

#endif /* TURBO_FEATURES */
#endif /* TURBO_H */
```

---

### T1.8 更新 Makefile / CMakeLists.txt

- 在 `src/turbo/` 的 `CMakeLists.txt` 中注册新子目录（`netif/`、`forward/`、`room/`、`api/`、`common/`）。
- 删除旧的 `network/`、`utils/` 子目录条目。
- 在 autotools `Makefile.am` 中同步更新 `TURBO_SOURCES` 变量。

---

### Phase 1 验收标准

- [ ] `grep -r "conductor\|DPDK\|rte_\|TURN_TURBO\|TURN_USE_DPDK" src/` 返回空
- [ ] `find src/turbo -type d` 输出 `netif/ forward/ room/ api/ common/`（旧 `network/` `utils/` 不存在）
- [ ] `./configure && make -j$(nproc)` 编译通过（不带 `--turbo`，与上游一致）
- [ ] `./configure --turbo && make -j$(nproc)` 编译通过（允许临时链接错误，因 Phase 2 模块尚未完成）

---

## Phase 2：网络后端层

> **目标**：实现 epoll / io_uring / AF_XDP 三种后端及运行时降级机制。完成后，Turbo Worker 线程能独立接收和发送 UDP 包。

### T2.1 重新设计 turbo_netif.h 接口

将现有 `src/turbo/netif/turbo_netif.h`（迁移自 network/）**完全替换**为设计文档 §5.1 中定义的新接口：

关键变更：
- 数据包结构从 `turbo_packet` 改为 `rtp_packet`（含 `headroom`、`refcount`、`backend_priv`）
- 增加 `suspend()`/`resume()` 方法（降级流程需要）
- `backend_type` 和 `health_status` 字段
- `degraded_count` 统计

同步更新所有引用 `turbo_packet` 的文件（`turbo_switch.c/h`、`turbo_room.c/h`）。

---

### T2.2 实现 turbo_epoll.c（保底降级路径）

新建 `src/turbo/netif/turbo_epoll.c`：

**核心机制**：
- 使用 `recvmmsg()`（批量接收，单次系统调用最多 64 包）
- 使用 `sendmmsg()`（批量发送）
- epoll 监听 socket 可读事件
- `suspend()`：从 epoll 中删除 socket fd
- `resume()`：重新加入 epoll 监听

**接口实现**：
```c
static struct turbo_netif_ops epoll_ops = {
    .init       = turbo_epoll_init,
    .close      = turbo_epoll_close,
    .suspend    = turbo_epoll_suspend,
    .resume     = turbo_epoll_resume,
    .recv_pkts  = turbo_epoll_recv_pkts,
    .send_burst = turbo_epoll_send_burst,
    .alloc_pkt  = turbo_epoll_alloc_pkt,
    .free_pkt   = turbo_epoll_free_pkt,
    .clone_pkt  = turbo_epoll_clone_pkt,
};
```

`clone_pkt` 实现：分配新 buffer，`memcpy` 数据，增加引用计数（epoll 无零拷贝，但接口语义一致）。

---

### T2.3 实现 turbo_iouring.c（默认高性能后端）

新建 `src/turbo/netif/turbo_iouring.c`：

**核心机制**（基于 liburing）：
- 使用 `io_uring_prep_recvmsg()` 批量提交接收 SQE
- 使用 `io_uring_prep_sendmsg()` 批量提交发送 SQE
- `io_uring_submit_and_wait()` 等待 CQE
- ring size 建议：SQ=256，CQ=512

**关键设计点**：
- 固定 buffer 模式（`io_uring_register_buffers`）减少 per-包映射开销
- 每个 `recv` SQE 预分配一个 `rtp_packet`，CQE 返回时直接读取 `res` 字段获得包长
- `suspend()`：停止提交新的 `recv` SQE，等待当前在途 CQE 完成
- `resume()`：重新批量提交 `recv` SQE

```c
static struct turbo_netif_ops iouring_ops = {
    .init       = turbo_iouring_init,
    .close      = turbo_iouring_close,
    .suspend    = turbo_iouring_suspend,
    .resume     = turbo_iouring_resume,
    .recv_pkts  = turbo_iouring_recv_pkts,
    .send_burst = turbo_iouring_send_burst,
    .alloc_pkt  = turbo_iouring_alloc_pkt,
    .free_pkt   = turbo_iouring_free_pkt,
    .clone_pkt  = turbo_iouring_clone_pkt,
};
```

---

### T2.4 改造 turbo_af_xdp.c（加入保险丝和新接口）

在现有 AF_XDP 实现基础上：

1. **适配新 `rtp_packet` 结构**：`backend_priv` 存储 UMEM frame 地址，`refcount` 实现零拷贝 clone（`refcount++` 而非 memcpy）。
2. **新增 `suspend()`/`resume()`**：`suspend()` 停止从 RX ring 取包；`resume()` 恢复轮询。
3. **实现保险丝**（`turbo_afxdp_health_check()`，见设计文档 §5.3.4）：
   - 周期性（每 1 秒）检查 RX ring fill 率
   - 连续 3 秒 >90% 满则触发 `turbo_netif_degrade()`
4. **XDP 模式选择**：保留 `auto/drv/skb` 三种模式

---

### T2.5 实现 turbo_netif.c（后端选择 + 降级逻辑）

新建/重写 `src/turbo/netif/turbo_netif.c`：

**主要函数**：

```c
/* 根据配置初始化后端 */
int turbo_netif_init(struct turbo_netif *tif, int backend_type);

/* 运行时降级（见设计文档 §5.3.2）*/
int turbo_netif_degrade(struct turbo_netif *tif,
                        enum turbo_degrade_trigger trigger);

/* SIGUSR1 异步标志处理（在 worker 线程 poll 周期调用）*/
void turbo_netif_check_degrade_request(struct turbo_netif *tif);
```

**Worker 线程主循环**：
```c
void *turbo_worker_loop(void *arg) {
    struct turbo_netif *tif = arg;
    struct rtp_packet *pkts[TURBO_RX_BURST];

    while (atomic_load(&tif->running)) {
        turbo_netif_check_degrade_request(tif);
        turbo_afxdp_health_check(tif);  /* 仅 AF_XDP 后端 */

        int n = tif->ops->recv_pkts(tif, pkts, TURBO_RX_BURST);
        for (int i = 0; i < n; i++)
            turbo_process_packet(tif, pkts[i]);
    }
    return NULL;
}
```

---

### T2.6 实现 turbo_ring.h（Worker↔libevent 消息队列）

新建 `src/turbo/common/turbo_ring.h`：

单生产者/单消费者无锁环形队列，用于 Turbo Worker 将需要 STUN 处理的包投递给 libevent 线程：

```c
/* 基于 power-of-2 掩码的无锁 SPSC ring */
#define TURBO_RING_SIZE 512

struct turbo_ring {
    void       *entries[TURBO_RING_SIZE];
    _Atomic uint32_t head;  /* 生产者写 */
    _Atomic uint32_t tail;  /* 消费者读 */
};

int  turbo_ring_push(struct turbo_ring *r, void *entry);
void *turbo_ring_pop(struct turbo_ring *r);
```

---

### T2.7 实现 turbo_seqlock.h（Allocation 表并发保护）

新建 `src/turbo/common/turbo_seqlock.h`，按设计文档 §6.2 实现 seqlock。

此 seqlock 仅保护 fastpath 需要读取的 allocation 字段（`room_id`、`relay_addr`、`expiry`），不影响 coturn 原有的 allocation 管理逻辑。

---

### Phase 2 验收标准

- [ ] `./configure --turbo && make -j$(nproc)` 编译无错误
- [ ] turbo worker 线程能启动并接收 UDP 包（用 `tcpdump` + `turnutils_uclient` 验证）
- [ ] `kill -SIGUSR1 $(pidof turnserver)` 能触发后端降级，日志输出降级信息
- [ ] AF_XDP 保险丝：模拟 RX ring 满后，3 秒内自动降级到 io_uring（集成测试）

---

## Phase 3：快速路径层

> **目标**：实现三表查找引擎和流量整形器，使媒体包完全在 Turbo Worker 线程中完成转发，不经过 libevent。

### T3.1 实现 turbo_fastpath.h/c（三表查找引擎）

新建 `src/turbo/forward/turbo_fastpath.h` 和 `turbo_fastpath.c`：

**数据结构**：按设计文档 §4.3 实现三个 hash 表：
- `channel_table`：key = `hash(src_ip, src_port, channel_no)`
- `l1_cache`：key = `hash(src_ip, src_port)`
- `username_table`：key = `username_hash`

**L1 缓存写锁**：
- 写入路径（libevent 线程）：`pthread_mutex_lock(&fastpath.l1_write_lock)`
- 读取路径（Turbo Worker）：直接读，无锁（hash 表实现保证读端原子性）

**查找函数**：
```c
/* 返回 alloc_id，未找到返回 0 */
uint32_t turbo_fastpath_lookup(struct turbo_fastpath *fp,
                               const uint8_t *buf, size_t len,
                               const struct sockaddr_in6 *src);

/* Allocate 成功时由 libevent 线程调用 */
void turbo_fastpath_warmup_l1(struct turbo_fastpath *fp,
                              uint32_t alloc_id,
                              const struct sockaddr_in6 *client_addr);
```

---

### T3.2 实现 turbo_shaper.h/c（流量整形器）

新建 `src/turbo/forward/turbo_shaper.h` 和 `turbo_shaper.c`：

**包分类逻辑**：
```c
/* 按设计文档 §4.2 判断包类型 */
static inline int classify_pkt(const uint8_t *buf, size_t len) {
    if (len < 2) return PKT_INVALID;
    uint16_t type = (buf[0] << 8) | buf[1];
    if (type >= 0x4000 && type <= 0x7FFF) return PKT_CHANNEL_DATA;
    if ((type & 0xC000) == 0x0000)        return PKT_STUN;
    return PKT_UNKNOWN;
}
```

**令牌桶限速**（STUN 队列）：
- `stun_pps_limit`：默认 2000 pps，可配置
- 使用 `clock_gettime(CLOCK_MONOTONIC)` 计算时间窗口，不引入锁

---

### T3.3 改造 turbo_switch.c/h（零拷贝克隆适配新接口）

现有 `turbo_switch.c/h` 使用旧的 `turbo_packet` 结构，需适配新的 `rtp_packet`：

**主要改动**：
- 将 `turbo_packet *` 替换为 `rtp_packet *`
- `clone_pkt` 调用改为 `tif->ops->clone_pkt(tif, pkt)`
- `send_burst` 调用改为 `tif->ops->send_burst(tif, pkts, count)`
- 保留目标地址重写逻辑（修改 IP/UDP header）

---

### T3.4 在 netengine.c 中添加快速路径入口

在 `src/apps/relay/netengine.c` 的 UDP 收包处理函数中，增加快速路径转交逻辑：

```c
/* netengine.c — UDP 收包点（#ifdef TURBO_FEATURES 保护） */
#ifdef TURBO_FEATURES
    /* 媒体包直接投递给 turbo worker，不进入 libevent 处理 */
    if (turbo_config.enabled) {
        /* 通过共享 relay socket 接收的包已由 worker 线程处理
         * 此处仅处理 libevent 原有 socket 上的控制流量 */
        /* 如包来自 turbo_shared_relay_socket 则跳过 libevent 处理 */
        if (fd == turbo_get_shared_relay_fd()) return;
    }
#endif
```

同时在 `mainrelay.c` 的 `turbo_init()` 中，确保 `turbo_shared_relay_socket` 的 fd 不被加入 libevent 的监听集合。

---

### T3.5 实现完整的 turbo_process_packet()

在 `src/turbo/turbo.h` 或单独的 `src/turbo/turbo_core.c` 中实现：

```c
/* Turbo Worker 线程中调用 */
void turbo_process_packet(struct turbo_netif *tif, struct rtp_packet *pkt) {
    /* 1. 流量整形分类 */
    int cls = turbo_shaper_classify(&g_turbo_shaper, pkt->data, pkt->len);
    if (cls == SHAPER_DROP) {
        tif->ops->free_pkt(tif, pkt);
        return;
    }
    if (cls == SHAPER_STUN_QUEUE) {
        /* 投入无锁 ring，由 libevent 线程处理 */
        turbo_ring_push(&g_ctrl_ring, pkt);
        return;
    }

    /* 2. 三表查找 */
    uint32_t alloc_id = turbo_fastpath_lookup(&g_turbo_fastpath,
                                              pkt->data, pkt->len,
                                              &pkt->src_addr);
    if (!alloc_id) {
        /* fastpath miss，投给 libevent 处理 */
        turbo_ring_push(&g_ctrl_ring, pkt);
        return;
    }

    /* 3. 获取 allocation（seqlock 保护只读访问）*/
    struct turbo_alloc_snapshot snap;
    turbo_alloc_read(&snap, alloc_id);

    /* 4. Permission 检查（复用 coturn 逻辑的轻量版本）*/
    if (!turbo_check_permission(&snap, &pkt->src_addr)) {
        tif->ops->free_pkt(tif, pkt);
        return;
    }

    /* 5. 转发 */
    if (snap.room_id[0] != '\0') {
        struct turbo_room *room = turbo_room_get(snap.room_id);
        if (room) turbo_room_broadcast(tif, room, pkt, alloc_id);
    } else {
        turbo_switch_forward(tif, pkt, &snap.peer_addr);
    }

    /* 6. 审计 */
    turbo_audit_record(&g_turbo_audit, alloc_id, pkt);

    tif->ops->free_pkt(tif, pkt);
}
```

---

### Phase 3 验收标准

- [ ] ChannelData 包能被 Turbo Worker 直接转发，不经过 libevent（通过日志/metrics 验证）
- [ ] `turbo_fastpath_hits_total{type="channelbind"}` 计数器正常递增
- [ ] STUN 限速：发送 >2000 pps STUN 包时，`turbo_stun_overload_drops_total` 计数器递增
- [ ] 端到端测试：`turnutils_uclient` 能正常建立 allocation 并传输数据
- [ ] 与原生 coturn 的 P50 延迟对比：turbo（io_uring）延迟 ≤ 原生 110%

---

## Phase 4：房间广播与 Provider 机制

> **目标**：实现 Room Identity Provider 框架和房间广播引擎。

### T4.1 实现 turbo_room_provider.h（Provider 接口）

新建 `src/turbo/room/turbo_room_provider.h`，按设计文档 §8.1 定义接口。

**全局 Provider 注册表**：
```c
/* 内置三种 provider */
extern struct turbo_room_provider_ops provider_static_ops;
extern struct turbo_room_provider_ops provider_token_hmac_ops;
#ifdef TURBO_LUA
extern struct turbo_room_provider_ops provider_lua_ops;
#endif

/* 根据配置字符串获取 provider */
struct turbo_room_provider_ops *turbo_provider_get(const char *name);
```

---

### T4.2 实现 provider_static.c

新建 `src/turbo/room/provider_static.c`：

- 解析格式：`room<ID>:<member_id>`（例如 `room123:userA`）
- `sscanf(username, "room%63[^:]:%63s", room_id, member_id)`
- 无需任何配置项

---

### T4.3 实现 provider_token_hmac.c

新建 `src/turbo/room/provider_token_hmac.c`：

**实现要点**：
1. JWT-like 格式解析（header.body.signature，均为 base64url 编码）
2. 使用 OpenSSL `HMAC()` + `CRYPTO_memcmp()` 验证签名
3. 过期时间检查（`time(NULL) > exp`）
4. 共享密钥从配置加载（支持从环境变量 `TURBO_ROOM_SECRET` 读取）

**配置项**：
- `turbo-room-token-secret`：共享密钥字符串
- `turbo-room-token-expiry`：允许的最大有效期（防过长 Token）

---

### T4.4 实现 provider_lua.c（可选）

新建 `src/turbo/room/provider_lua.c`：

- 依赖 LuaJIT（`--turbo-lua` configure 选项触发）
- 实现 50ms 超时保护（`setitimer(ITIMER_REAL, ...)` + `SIGALRM` 处理）
- Lua 脚本路径通过 `turbo-room-lua-script` 配置
- `lua_pcall()` 调用用户定义的 `extract_room_info(username, realm, addr)` 函数

---

### T4.5 重写 turbo_room.h/c（RCU 链表 + 惰性创建）

**重写要点**（参考设计文档 §7.3-7.5）：

1. **成员链表**：从 hash 表改为 `list_head` 双向链表 + `pthread_rwlock_t`
2. **房间表**：使用 uthash（`UT_hash_handle hh`）索引 `room_id` 字符串 → `turbo_room*`
3. **惰性创建**：`turbo_room_add_member()` 检查房间不存在则自动创建（持全局写锁）
4. **引用计数销毁**：`turbo_room_remove_member()` 后若 `member_count == 0` 则自动销毁房间
5. **幽灵成员防护**：在 `ns_turn_allocation.c` 的 allocation 销毁回调中调用 `turbo_room_remove_member()`
6. **成员上限**：超过 `max_members`（默认 50）时拒绝加入并记录 `turbo_room_member_overflow` 告警

**文件位置**：将重写后的文件放在 `src/turbo/room/turbo_room.h/c`（旧文件在 `forward/`，Phase 1 暂不移动，此处统一移动）

---

### T4.6 更新 ns_turn_server.c 的 Allocate 钩子

在 `src/server/ns_turn_server.c` 的 `create_relay_connection()` 成功返回后，按设计文档 §4.1.1 和 §8.3 添加：

```c
#ifdef TURBO_FEATURES
    /* Provider 调用（仅在 turbo-rooms 启用时）*/
    if (turbo_config.rooms_enabled && turbo_config.room_provider) {
        /* ... Provider extract + room_add_member ... */
    }
    /* 单端口收敛：覆写 relay 地址端口为 3478 */
    turbo_override_relay_port(ss, TURBO_RELAY_PORT);
    /* L1 缓存预热（跨线程写，mutex 保护）*/
    turbo_l1_cache_warmup(ss, &ss->client_addr);
#endif
```

**注意**：`turbo_override_relay_port()` 必须在 `create_relay_connection()` 成功后调用，且在发送 Allocate 成功响应之前完成。

---

### T4.7 更新 ns_turn_allocation.h/c（扩展 allocation 结构体）

在 `src/server/ns_turn_allocation.h` 中（`#ifdef TURBO_FEATURES` 保护）：

```c
/* 末尾新增（不影响非 Turbo 编译）*/
#ifdef TURBO_FEATURES
    char     turbo_room_id[64];
    char     turbo_member_id[64];
#endif
```

在 `src/server/ns_turn_allocation.c` 中新增：
```c
#ifdef TURBO_FEATURES
void allocation_set_room_id(allocation *a,
                             const char *room_id,
                             const char *member_id);
#endif
```

---

### Phase 4 验收标准

- [ ] `static` Provider：`username=room123:userA` 的 Allocate 成功后，`turbo_room_members_current{room_id="123"}` 为 1
- [ ] `token_hmac` Provider：合法 Token 验证通过，篡改后的 Token 返回 401
- [ ] 房间广播：3 个客户端加入同一房间，A 发送数据后 B 和 C 均能收到（零拷贝克隆）
- [ ] 幽灵成员清理：allocation 超时后，房间成员计数自动减少
- [ ] 房间上限：第 51 个客户端加入时收到错误，`turbo_room_member_overflow` 告警触发
- [ ] Lua Provider（若实现）：50ms 超时测试，超时后返回 -1（fallback 标准 TURN）

---

## Phase 5：运维与监控

> **目标**：实现新版 Admin API、审计日志、配置文件和文档。可与 Phase 4 并行推进。

### T5.1 重写 turbo_api.h/c（新端点）

**删除**：`/v1/room/create`、`/v1/room/join`、`/v1/room/leave`、`/v1/room/info`、`/v1/room/list`

**新增**（保留 libevent HTTP 框架基础设施）：

| 端点 | 实现要点 |
|------|---------|
| `GET /admin/status` | 返回设计文档 §11.2 的 JSON 结构 |
| `GET /admin/metrics` | Prometheus text 格式，输出 §11.1 所有指标 |
| `POST /admin/turbo-disable` | 调用 `turbo_netif_degrade()` 到 epoll |
| `POST /admin/turbo-enable` | 重置 degrade 标志（需要 worker 重启才真正切换后端）|
| `POST /admin/drain` | 设置 `drain_mode=1`，`turbo_config.accepting_new_allocs=0` |
| `GET /admin/drain/status` | 返回当前 allocation 数 + 预估清空时间 |

---

### T5.2 实现 turbo_audit.h/c（审计钩子）

新建 `src/turbo/forward/turbo_audit.h` 和 `turbo_audit.c`：

按设计文档 §9.2 实现：
- 无锁环形缓冲区（4096 条目）
- 独立 flush 线程，每 10 秒将积累的事件写入 Unix Socket
- 输出格式：JSON 流（每行一个事件）
- 溢出时丢弃最旧条目，`turbo_audit_dropped_total` 计数

---

### T5.3 更新 conf/turbo.conf.example

按设计文档 §13.3 重写配置模板，删除所有 DPDK/Conductor/v1 room API 相关注释和选项，新增：
- `turbo-backend=io_uring`
- `turbo-l1-warmup=enable`
- `turbo-room-id-provider=token_hmac`
- `turbo-room-token-secret`
- `turbo-audit-log`

---

### T5.4 更新 mainrelay.c 配置解析

在 `src/apps/relay/mainrelay.c` 中：

**删除**：
- `TURBO_AFXDP_MODE_OPT`（改为 `TURBO_BACKEND_OPT`）
- `turbo_afxdp_mode` 字段（改为 `turbo_backend` 字符串）

**新增**：
- `--turbo-rooms` 选项解析 → `turbo_config.rooms_enabled`
- `--turbo-backend=` 选项解析 → `turbo_config.backend_type`
- `--turbo-room-id-provider=` → `turbo_config.room_provider_name`
- `--turbo-room-token-secret=` → `turbo_config.room_token_secret`
- `--turbo-room-lua-script=` → `turbo_config.room_lua_script`

---

### T5.5 更新 docs/TURBO.md 和 docs/TURBO_ROOM_PROVIDER.md

- `TURBO.md`：删除 DPDK 部分，新增 io_uring 部分；更新降级操作说明；更新配置选项表
- `TURBO_ROOM_PROVIDER.md`：更新 Provider 接口定义（`turbo_room_provider_ops`）；补充 `token_hmac` 生成示例（多语言：Node.js / Python / Go）；补充 Lua Provider 约束说明（50ms 超时、不允许阻塞 I/O）

---

### T5.6 更新 CLAUDE.md 目录结构部分

将 CLAUDE.md 中的 `src/turbo/` 目录结构更新为新的 `netif/`、`forward/`、`room/`、`api/`、`common/` 结构，删除对 Conductor 和 DPDK 的描述。

---

### Phase 5 验收标准

- [ ] `curl http://localhost:8080/admin/status` 返回合法 JSON，包含 `turbo_enabled`、`backend`、`allocations` 等字段
- [ ] `curl http://localhost:8080/admin/metrics` 返回 Prometheus 格式，包含所有 §11.1 指标
- [ ] `curl -X POST http://localhost:8080/admin/turbo-disable` 触发降级到 epoll
- [ ] 审计日志：建立 allocation 后传输数据，Unix Socket 上能收到对应的 JSON 审计事件
- [ ] `./configure --turbo --turbo-rooms && make -j$(nproc)` 编译无警告

---

## 全量验收标准

### 功能验收

- [ ] 标准 TURN（无 `--turbo`）：与上游 coturn 行为完全一致
- [ ] 单端口收敛：所有 Allocate 响应中 `XOR-RELAYED-ADDRESS` 端口为 3478
- [ ] 快速路径命中率：稳定运行 5 分钟后，`turbo_fastpath_hits_total / total_pkts > 99.9%`
- [ ] 运行时降级：`kill -SIGUSR1` 在 <100ms 内完成后端切换，无新 allocation 失败
- [ ] 房间广播：N 人房间，1 人发送，N-1 人收到（验证包含 3 人以上场景）

### 性能验收

- [ ] io_uring 后端 P50 延迟 ≤ 原生 epoll 的 110%（使用 `turnutils_uclient` 端到端测试）
- [ ] STUN 限速：2001 pps STUN 时，超额部分被丢弃，媒体包不受影响

### 安全验收

- [ ] 篡改 Token 的 `room_id` 字段后，Allocate 返回 401
- [ ] 使用过期 Token（`exp` 已过）时，Allocate 返回 401
- [ ] HMAC 验证使用常数时间比对（代码审查确认使用 `CRYPTO_memcmp`）

### 代码质量验收

- [ ] `grep -r "TURN_TURBO\|TURN_USE_DPDK\|conductor\|rte_" src/` 返回空
- [ ] `grep -r "#ifdef TURBO_FEATURES" src/server/ src/apps/relay/` 新增行 < 60 行
- [ ] 所有新增文件有 BSD-3-Clause 许可证头

---

## 依赖关系图

```
T1.1──┐
T1.2──┤
T1.3──┤
T1.4──┼──► Phase 1 完成 ──► T2.1──► T2.2 ──┐
T1.5──┤                              T2.3 ──┤──► Phase 2 完成 ──► T3.1──► T3.5──► Phase 3 完成
T1.6──┤                              T2.4 ──┤                     T3.2──┘
T1.7──┤                              T2.5 ──┤                     T3.3
T1.8──┘                              T2.6 ──┘                     T3.4
                                     T2.7

Phase 3 完成 ──► T4.1──► T4.2──┐
                 T4.3──┘        │
                 T4.4(可选)     ├──► T4.5──► T4.6──► T4.7──► Phase 4 完成
                                │
Phase 3 完成 ──► T5.1──┐        │
                 T5.2──┤        │
                 T5.3──┤(可并行)│
                 T5.4──┤        │
                 T5.5──┘        │
                                └──► 全量验收
```

---

## 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| io_uring 与 libevent 共存引发 fd 竞争 | 中 | 高 | Phase 2 早期单独验证 worker 线程独占 relay socket |
| 单端口收敛后 allocation 内部状态不一致 | 中 | 高 | T4.6 后立即回归 `turnutils_uclient` 完整功能测试 |
| AF_XDP 降级窗口导致超预期丢包 | 低 | 中 | 在测试环境中实测降级耗时，更新 SLO 数值 |
| token_hmac HMAC 实现存在时序漏洞 | 低 | 高 | T4.3 代码审查：确认使用 `CRYPTO_memcmp` 而非 `memcmp` |
| Lua Provider 阻塞 Allocate 线程 | 中 | 中 | T4.4 集成测试：模拟 Lua 脚本 sleep(1s)，验证超时保护生效 |
