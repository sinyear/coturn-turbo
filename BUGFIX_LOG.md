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

*更多修复记录将追加到本文件末尾。*
