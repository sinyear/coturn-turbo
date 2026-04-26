# CLAUDE.md

## 项目概述

coturn-turbo 是 [coturn](https://github.com/coturn/coturn) TURN/STUN 服务器的增强分支，在**100% 兼容标准 TURN 协议 (RFC 8656)** 且**不强制客户端升级**的前提下，提供：

- **单端口收敛**：所有流量复用到 1 个 UDP 端口（3478）
- **高性能用户态网络**：可选的 `io_uring` / `AF_XDP` 后端，及运行时动态降级至 `epoll`
- **信令驱动的轻量房间广播**：外部信令服务控制，媒体层面实现广播（类似 SFU）
- **可插拔的身份识别**：Room Identity Provider 机制，支持 static / HMAC Token / Lua 脚本

所有增强特性通过 `--turbo` 编译选项控制，未启时编译产物与上游 coturn 完全一致。

## 技术栈

| 层面 | 技术 |
| :--- | :--- |
| 语言 | C11 |
| 构建系统 | Autotools (`autoconf`, `automake`) + Make |
| 核心依赖 | libevent, OpenSSL |
| Turbo 可选依赖 | liburing (≥5.6), libbpf + libxdp + clang (AF_XDP), LuaJIT |
| 协议 | STUN (RFC 8489), TURN (RFC 8656) |
| 监控 | Prometheus 指标端点 (嵌入式 HTTP) |

## 目录结构

```
coturn-turbo/
├── configure                  # 上游 build 脚本，新增 --turbo 等选项
├── Makefile                   # 调度编译，引入 src/turbo/ 目标
├── src/
│   ├── server/                # 原始 coturn 核心，最小侵入
│   │   ├── ns_turn_server.c   # TURN 分配处理，Allocate 处插入钩子
│   │   ├── ns_turn_allocation.h/.c  # 扩展分配结构体 room_id 等
│   │   └── ns_turn_maps.c     # 全局哈希表声明
│   ├── apps/relay/
│   │   ├── mainrelay.c        # 主入口，增加 turbo_init / turbo_deinit
│   │   ├── netengine.c        # UDP 收发钩子，引入快速路径
│   │   └── mainrelay.h
│   └── turbo/                 # 【核心新增库，完全独立】
│       ├── turbo.h            # 主开关和接口宏
│       ├── netif/             # 网络后端抽象层
│       │   ├── turbo_netif.h  # 统一后端接口 (epoll/io_uring/AF_XDP)
│       │   ├── turbo_epoll.c  # epoll 实现 (保底降级路径)
│       │   ├── turbo_iouring.c # io_uring 实现 (默认高性能选项)
│       │   ├── turbo_af_xdp.c # AF_XDP 实现 (极致性能)
│       │   └── xdp_prog.c     # XDP eBPF 程序 (编译为 xdp_prog.o)
│       ├── forward/           # 快速转发路径
│       │   ├── turbo_fastpath.h/c  # 自适应查找引擎 + L1 缓存
│       │   ├── turbo_shaper.h/c    # 流量整形器 (信令/媒体隔离)
│       │   ├── turbo_switch.h/c    # 零拷贝包克隆和转发
│       │   └── turbo_audit.h/c     # 审计钩子 (环形缓冲 + Unix Socket)
│       ├── room/              # 轻量房间广播 (可选)
│       │   ├── turbo_room.h/c # rwlock 保护的成员链表与广播引擎（惰性创建）
│       │   ├── turbo_room_provider.h # 可插拔身份识别接口
│       │   ├── provider_static.c    # 静态用户名解析 (room<ID>:<member>)
│       │   ├── provider_token_hmac.c # HMAC-SHA256 Token 验证
│       │   └── provider_lua.c       # Lua 脚本自定义逻辑 (可选，--turbo-lua)
│       ├── api/               # Admin HTTP API
│       │   ├── turbo_api.h/c  # /admin/metrics, /status, /turbo-disable, /drain
│       └── common/            # 共享工具
│           ├── turbo_hash.h   # 无锁哈希表 (uthash 封装)
│           ├── turbo_ring.h   # 无锁环形队列
│           └── turbo_json.h   # 轻量 JSON 解析
├── conf/
│   └── turbo.conf.example     # Turbo 配置示例
└── docs/
    ├── TURBO.md               # Turbo 功能用户文档
    └── TURBO_ROOM_PROVIDER.md # Room Provider 集成指南
```

**侵入点说明**：所有对上游代码的修改均被 `#ifdef TURBO_FEATURES` 保护，总计不足 50 行。

## 构建与运行

### 依赖安装

```bash
# 基础 (同 coturn)
sudo apt install -y libevent-dev libssl-dev build-essential autoconf automake libtool

# io_uring (推荐)
sudo apt install -y liburing-dev

# AF_XDP (可选)
sudo apt install -y libbpf-dev libxdp-dev clang llvm
```

### 编译

```bash
# 恢复 autotools 环境 (首次从仓库克隆后)
autoreconf -fi

# 标准 coturn (无 Turbo)
./configure && make -j$(nproc)

# 启用 Turbo + io_uring
./configure --turbo && make -j$(nproc)

# 启用 Turbo + AF_XDP
./configure --turbo --turbo-backend=afxdp && make -j$(nproc)

# 启用房间广播 (可选)
./configure --turbo --turbo-rooms && make -j$(nproc)
```

### 配置与启动

```bash
# 复制配置示例
cp conf/turbo.conf.example /etc/turnserver.conf

# 编辑关键配置
vim /etc/turnserver.conf
# 最低配置: listening-port=3478, realm=your.realm, lt-cred-mech, user=...
# Turbo 关键项: turbo, turbo-backend=io_uring, turbo-api-port=8080

# 启动 (二进制名为 turnserver)
turnserver -c /etc/turnserver.conf --turbo

# 运行时管理
curl http://localhost:8080/admin/status    # 查看状态
curl -X POST http://localhost:8080/admin/turbo-disable  # 动态降级到 epoll
```

## 核心设计概念

### 1. 单端口收敛与自适应查找
- 所有 Allocate 响应均返回固定端口 3478 的 XOR-RELAYED-ADDRESS。
- 数据包到达后，经过三级查找定位 allocation：
  1. **信道绑定表** (最快，ChannelData)
  2. **L1 快速缓存** (`src_ip+src_port` → alloc, 处理 Send Indication)
  3. **辅助解析表** (`username_hash` → alloc, 极少使用，触发即告警)
- L1 缓存在 Allocate 成功时预热，确保 Send Indication 客户端的首包延迟不退化。

### 2. 网络后端抽象与动态降级
- 定义统一接口 `turbo_netif_ops`，支持零拷贝克隆 (`clone_pkt`) 和批量发送 (`send_burst`)。
- 运行时可通过 Admin API 或 `kill -SIGUSR1` 从 AF_XDP → io_uring → epoll 无感降级，通话不中断。
- AF_XDP 包含保险丝：连续 3 秒 RX Ring 满则自动降级。

### 3. 房间广播 (可选)
- **机制**：信令服务在 ICE 配置的 `username` 字段下发 Token，turn server 通过 Provider 解析出 `room_id` 和 `member_id`，自动将 allocation 加入房间成员表。
- **转发**：收到媒体包后，若 allocation 关联房间，则遍历房间成员（RCU 保护），零拷贝克隆包并重写目标地址后批量发送。
- **生命周期**：房间随最后一个成员离开自动销毁；所有状态全在外部信令服务。

### 4. Room Identity Provider (可插拔)
- 抽象接口 `turbo_room_provider_ops` 定义 `extract(username, realm, credential, client_addr) → (room_id, member_id, expiry)`。
- 内置三种实现：
  - `static`：直接解析 `username` 格式 `room<ID>:user<ID>`
  - `token_hmac`：类似于 JWT 的 HMAC-SHA256 签名 Token，防止篡改
  - `lua_script`：调用用户 Lua 脚本，可对接任意鉴权系统（如 Redis、HTTP API）
- 配置项 `turbo-room-id-provider` 选择实现。

### 5. 无状态媒体节点
- coturn-turbo **不存储**房间状态、成员列表等业务数据，所有决策权归信令服务。
- 节点可水平扩展，无需状态同步；任意节点宕机不影响整体服务。

## 关键代码流程

### 处理 Allocate 请求 (ns_turn_server.c)
```c
#ifdef TURBO_FEATURES
  if (turbo_config.rooms_enabled && turbo_config.room_provider) {
      struct turbo_room_info info;
      int ret = turbo_config.room_provider->extract(ss->username,...);
      if (ret == 0) {
          allocation_set_room_id(alloc, info.room_id, info.member_id);
          turbo_room_add_member(alloc);  // 加入 RCU 链表
      } else if (ret == -2) {
          // Token 验证失败，返回 401
      }
      // ret == -1 走标准 TURN 逻辑
  }
  // 单端口收敛：固定返回 3478 作为 relay 端口
#endif
```

### 收包快速路径 (netengine.c 钩子)
```c
if (turbo_fastpath_enabled) {
    struct allocation *alloc = turbo_fastpath_lookup(pkt);
    if (alloc) {
        // permission 检查... 然后进入：
        if (alloc->room_id) {
            turbo_room_broadcast(netif, alloc->room, pkt, alloc->id);
        } else {
            turbo_switch_forward(netif, alloc->peer, pkt);
        }
        return; // 已经处理，不走原始路径
    }
}
// 降级到标准 coturn 处理
```

### 网络后端调用示例
```c
struct turbo_netif *tif = turbo_netif_current();
struct rtp_packet *pkt = tif->ops->alloc_pkt(tif);
...
int sent = tif->ops->send_burst(tif, burst_pkts, count);
```

## 配置参考

```ini
# 基础 TURN
listening-port=3478
realm=north
lt-cred-mech
user=test:test123

# Turbo 核心
turbo
turbo-backend=io_uring         # io_uring | af_xdp (默认 io_uring)
turbo-l1-warmup enable         # 预热 L1 缓存
turbo-api-port=8080            # 管理 API

# 房间广播 (可选)
# turbo-rooms
# turbo-room-id-provider token_hmac
# turbo-room-token-secret <共享密钥>
# turbo-room-token-expiry 3600

# 审计
# turbo-audit-log unix:///var/run/turn-audit.sock

# 日志
verbose
log-file=/var/log/turnserver/turbo.log
```

## 开发与贡献

### 添加新的 Room Identity Provider
1. 在 `src/turbo/room/` 创建 `provider_<name>.c`，实现 `turbo_room_provider_ops` 接口。
2. 在 `turbo_room.c` 中注册新的 provider。
3. 更新 `configure.ac` 和文档。

示例最小实现：
```c
#include "turbo_room_provider.h"

static int my_provider_init(void *config) { return 0; }
static int my_provider_extract(const char *username, ...) {
    // 自定义解析逻辑
    return 0; // 或 -1 (非房间), -2 (验证失败)
}
static void my_provider_deinit(void) {}

struct turbo_room_provider_ops my_provider_ops = {
    .name = "my_provider",
    .init = my_provider_init,
    .extract = my_provider_extract,
    .deinit = my_provider_deinit,
};
```

### 编码风格
- 遵循 coturn 原有代码风格（BSD KNF 类似）。
- 新增代码使用 C11，所有符号避免与上游冲突，使用 `turbo_` 前缀。
- 所有对上游文件的修改必须用 `#ifdef TURBO_FEATURES` … `#endif` 包裹。
- 关键数据结构使用 uthash 和 Linux 内核链表风格。

### 测试
```bash
# 基本流测试
cd test/
./test_turbo_fastpath

# 使用 turnutils_uclient 进行端到端测试
turnutils_uclient -t -W <turn_secret> -p 3478 <turn_server>
```

### 调试
- 设置 `CTURBO_DEBUG=1` 环境变量启用详细日志。
- 使用 `curl /admin/status` 查看快速路径命中率、当前分配数、后端类型等。
- AF_XDP 问题可通过 `turbo-mirror` (可选) 将流量镜像到 nflog 分析。

## 常见问题

**Q: 如何确认 Turbo 是否生效？**  
访问 `/admin/status`，`"turbo_enabled": true` 且 `"fastpath_hit_rate" > 99%` 表示正常工作。

**Q: 如何回滚到原始 coturn 行为？**  
运行时：`curl -X POST /admin/turbo-disable`  
编译时：不加 `--turbo` 重新 `./configure && make`

**Q: 客户端需要修改吗？**  
完全不需要。房间功能所需的 token 仅作为标准 TURN `username` 传递，标准 WebRTC 栈无需任何改动。

**Q: 如何集成外部信令服务？**  
使用 `token_hmac` provider，信令服务只需生成 HMAC 签名的 Token：
```javascript
// 信令侧生成 Token
const token = jwt.sign({ room_id: '123', member_id: 'userA' }, SHARED_SECRET, 
                        { expiresIn: '1h', algorithm: 'HS256' });
// 下发给客户端
iceServers: [{ urls: "turn:server:3478", username: token, credential: "dummy" }]
```
coturn-turbo 使用相同共享密钥验证签名，提取房间信息。

## 相关文档
- `docs/TURBO.md` — 完整功能说明和运维手册
- `docs/TURBO_ROOM_PROVIDER.md` — Provider 开发指南与示例
- `conf/turbo.conf.example` — 带注释的配置模板