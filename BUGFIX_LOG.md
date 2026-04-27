# coturn-turbo Bug 修复记录

按修复时间倒序排列，每条记录包含：现象、根因分析、修复方案、配置要求。

---

## [BUG-001] Turbo 模式在云服务器上 EADDRNOTAVAIL (errno=99) 循环报错

**日期**：2026-04-26  
**分支**：`feature/turbo-fast`  
**修复文件**：`src/apps/relay/ns_ioalib_engine_impl.c`

### 现象

在腾讯云等公网 NAT 云服务器上启动 turbo io_uring 模式，H5 音视频客户端接入时日志持续刷出：

```
WARNING: Trying to bind fd 39 to <122.51.14.87:65331>: errno=99
WARNING: Trying to bind fd 39 to <122.51.14.87:64892>: errno=99
...（每次 Allocate 请求触发数十条）
```

`errno=99` 即 `EADDRNOTAVAIL`——无法将请求的地址分配给套接字。

### 根因分析

**两个问题叠加：**

#### 1. 云服务器 NAT 架构问题（配置层面）

云服务器的公网 IP（如 `122.51.14.87`）由云厂商通过 NAT/弹性公网 IP 映射，**不直接绑定在任何本地网络接口**。服务器本地网卡只有私网 IP（如 `10.x.x.x`）。

当 `relay-ip` 被配置或自动探测为公网 IP 时，`create_relay_ioa_sockets()` 尝试：

```c
bind(fd, {AF_INET, 122.51.14.87, port})  // 失败：内核找不到该 IP 对应的本地接口
```

内层 `for (i = 0; i < 0xFFFF; i++)` 循环对每个端口都重试，每次都 `EADDRNOTAVAIL`，产生大量 WARNING 日志，最终分配失败返回 508。

#### 2. Turbo 单端口模式未跳过无用的 Relay Socket 绑定（代码 Bug）

Turbo 单端口收敛的核心设计：所有数据转发走共享 socket（`0.0.0.0:3478`），`create_relay_connection()` 创建的 relay socket 仅作为**行政结构**（记录 lifetime、permission 状态），**不在数据路径上**。

但代码仍然尝试将这些无用的 relay socket 绑定到 `relay_addr`（公网 IP），在云服务器上必然失败。

**调用链：**
```
handle_allocate_request()
  └─ create_relay_connection()
       └─ create_relay_ioa_sockets()          ← 问题所在
            └─ bind_ioa_socket(fd, relay_addr:port)  ← EADDRNOTAVAIL
```

### 修复方案

在 `create_relay_ioa_sockets()`（`src/apps/relay/ns_ioalib_engine_impl.c`）中，turbo 模式下 bind 调用使用 `INADDR_ANY` 替代 relay IP，端口池管理（`turnipports_release`）仍保留原始 relay IP 地址，避免端口泄漏：

```c
#if defined(TURBO_FEATURES)
    const int turbo_bind_any = turn_params.turbo_enabled;
#endif
```

在两处 `bind_ioa_socket` 调用处（RTP socket、RTCP socket）各引入临时变量：

```c
#if defined(TURBO_FEATURES)
    ioa_addr _rtp_bind_addr;
    addr_cpy(&_rtp_bind_addr, &local_addr);   // 含正确 port
    if (turbo_bind_any) {
        // IP 改为 INADDR_ANY，让 bind 在云服务器上成功
        ((struct sockaddr_in *)&_rtp_bind_addr)->sin_addr.s_addr = htonl(INADDR_ANY);
    }
    const ioa_addr *_rtp_bind = &_rtp_bind_addr;
#else
    const ioa_addr *_rtp_bind = &local_addr;
#endif
    if (bind_ioa_socket(*rtp_s, _rtp_bind, ...) >= 0) { break; }
```

`local_addr` / `rtcp_local_addr` 保留真实 relay IP，`turnipports_release()` 错误路径下依然能正确归还端口到对应的 IP 端口池。

### 必须配合的配置修改

代码修复解决了绑定崩溃，但还需要在 `turnserver.conf` 中正确配置，否则 Allocate 响应里给客户端的 relay IP 会是 `0.0.0.0`：

```ini
# 私网 IP（实际绑定在网卡上的地址）
relay-ip=10.x.x.x

# 公网 IP / 私网 IP 映射，让 TURN 响应对外报告正确的公网地址
external-ip=122.51.14.87/10.x.x.x
```

Turbo 在 Allocate 响应中会把端口覆盖为 3478，最终客户端看到的 relay 地址为 `122.51.14.87:3478`。

### 影响范围

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 云服务器 + turbo + 公网 relay-ip | 分配失败 508，日志刷满 WARNING | 绑定 INADDR_ANY，一次成功 |
| 自建机房 + turbo + 本地 relay-ip | 正常（无影响） | 行为不变 |
| 非 turbo 模式 | 正常（无影响） | 行为不变（`#ifdef` 保护） |

---

## [BUG-002] io_uring CQ 推进错误导致双重释放 → heap corruption → 崩溃

**日期**：2026-04-27  
**分支**：`feature/turbo-fast`  
**文件**：`src/turbo/netif/turbo_iouring.c`  
**症状**：WebRTC 音视频通话在 session 关闭时进程 abort，错误为 `malloc(): mismatching next->prev_size (unsorted)`（glibc 堆损坏检测）。崩溃 backtrace 顶部为 `decrement_global_allocation_count → TURN_LOG_FUNC → fprintf` 等 malloc 内部调用，这是 glibc 在 malloc 遍历 unsorted bin 时发现 chunk 元数据被覆盖后的 abort。  
**触发时机**：BUG-001 修复后才出现；BUG-001 修复之前，所有 Allocate 均以 EADDRNOTAVAIL 失败，io_uring worker 从未处理过真实媒体流量，此隐性 bug 因无流量而未触发。

### 根因分析

`iouring_recv_pkts` 中存在三个互相关联的错误：

**错误 A：CQ 推进值只计成功 recv，未计 error/send CQE**

```c
// 旧代码
io_uring_cq_advance(&priv->ring, n + (priv->inflight_rx < 0 ? 0 : 0));
//                               ^^^— 只计成功 recv 数
//                                    (... < 0 ? 0 : 0) 永远 = 0，是 typo
```

`io_uring_for_each_cqe` 只迭代，不推进 ring head；推进必须通过 `io_uring_cq_advance(ring, count)` 完成。若 `count` 偏小，未被推进的 CQE 在下次循环时会被重复处理。

**错误 B：error recv CQE 双重释放**

当 recv CQE 的 `res ≤ 0`（错误或零长）且 `user_data = pkt（非 NULL）` 时：
1. 第一次：`iouring_free_pkt(tif, p)` 释放包。
2. CQE **未被推进**（advance 值不含此项）。
3. 下次 `recv_pkts`：同一 CQE 再次出现，`p != NULL` → `iouring_free_pkt` 再次调用 → **double-free** → 堆元数据损坏。

**错误 C：send CQE 被错误计入 `inflight_rx`**

`iouring_send_burst` 以 `io_uring_sqe_set_data(sqe, NULL)` 提交 send SQE，但 io_uring 仍会生成对应的 CQE（`user_data=NULL, res=bytes_sent`）。这些 send CQE 在循环中走 `p == NULL` 分支，但旧代码仍执行了 `priv->inflight_rx--`。`inflight_rx` 只在提交 recv SQE 时递增，被 send CQE 反复递减，最终变为大幅负值。关闭时：

```c
// 旧代码：inflight_rx 为负 → 转换为 unsigned 后约等于 4×10^9 → CQ 灾难性推进
io_uring_cq_advance(&priv->ring, priv->inflight_rx);
```

### 修复方案

**`iouring_recv_pkts`**：引入 `total_consumed`（所有已迭代 CQE 数）和 `recv_consumed`（仅 recv CQE 数），以 `total_consumed` 推进 CQ，以 `recv_consumed` 控制 SQE 补充。send CQE（`p == NULL`）只计入 `total_consumed`，跳过 `inflight_rx--`。

```c
int total_consumed = 0;
int recv_consumed  = 0;
io_uring_for_each_cqe(&priv->ring, head, cqe) {
    if (n >= max) break;
    total_consumed++;
    struct rtp_packet *p = io_uring_cqe_get_data(cqe);
    if (p == NULL) { continue; }   /* send CQE — 仅消费，不改 inflight_rx */
    recv_consumed++;
    priv->inflight_rx--;
    if (cqe->res > 0) { p->len = cqe->res; pkts[n++] = p; }
    else              { iouring_free_pkt(tif, p); }   /* error，已计入 total_consumed，CQE 会被推进 */
}
io_uring_cq_advance(&priv->ring, total_consumed);
/* 对每个消费的 recv CQE（成功+error）都补充一个新 recv SQE，保持 inflight_rx 稳定 */
for (int i = 0; i < recv_consumed; i++) { ... submit_recv_sqe ... }
```

**`iouring_close`**：用实际迭代计数 `drained` 替换 `priv->inflight_rx` 作为 advance 参数。

### 影响范围

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| WebRTC 通话 session 关闭 | 进程 abort，堆损坏 | 正常退出 |
| 高并发（大量 send CQE 堆积） | inflight_rx 无限负漂，关闭时灾难性 advance | inflight_rx 值稳定 |
| recv error（对端 RST 等） | 包被双重释放 | 包被正确单次释放 |

---

## [BUG-003] INADDR_ANY 绑定导致 close 时端口无法归还（端口泄漏）

**日期**：2026-04-27  
**分支**：`feature/turbo-fast`  
**文件**：`src/apps/relay/ns_ioalib_engine_impl.c`  
**症状**：长时间运行后可用端口数逐渐减少；重启服务可恢复。端口泄漏在 BUG-001 修复引入 `turbo_bind_any` 路径后出现。  

### 根因分析

BUG-001 修复将 relay socket 绑定地址替换为 `INADDR_ANY:port`，并将该地址传给 `bind_ioa_socket`。`bind_ioa_socket` 内部：

```c
addr_cpy(&(s->local_addr), local_addr);   // 存储 0.0.0.0:port
```

关闭 socket 时，`close_ioa_socket` 调用：

```c
turnipports_release(tp, transport, &s->local_addr);   // 用 0.0.0.0 查 IP 端口池
```

而端口池以 relay IP（如 `172.17.123.27`）为 key。`ur_addr_map_get` 查不到 `0.0.0.0` 对应的条目，静默失败，端口永久泄漏。

### 修复方案

在 `bind_ioa_socket` 成功返回后（turbo 路径），立即将 `s->local_addr` 恢复为包含真实 relay IP 的 `local_addr`：

```c
if (bind_ioa_socket(*rtp_s, _rtp_bind, ...) >= 0) {
#if defined(TURBO_FEATURES)
    if (turbo_bind_any)
        addr_cpy(&((*rtp_s)->local_addr), &local_addr);  /* 恢复 relay IP */
#endif
    break;
}
```

`local_addr` 始终保持 relay IP + 实际分配端口，只有传给 `addr_bind()` 的参数（`_rtp_bind`）才被替换为 INADDR_ANY。

### 影响范围

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| turbo 模式，session 正常关闭 | 端口泄漏，长时间后耗尽 | 端口正确归还端口池 |
| turbo 模式，绑定失败错误路径 | 不受影响（local_addr 未进入 bind_ioa_socket） | 行为不变 |
| 非 turbo 模式 | 不受影响（`#ifdef` 保护） | 行为不变 |

---

*更多修复记录将追加到本文件末尾。*
