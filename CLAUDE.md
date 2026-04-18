# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在处理本仓库代码时提供项目背景、构建流程、代码规范及架构指引。

## 项目概述

coturn-turbo 是基于 **coturn 4.10.0** 扩展的高性能 TURN-SFU 融合服务器，在保持完全兼容标准 TURN 协议（RFC 5766、RFC 5389）的前提下，通过集成 DPDK/AF_XDP 内核旁路技术、单端口复用及房间广播机制，将传统的一对一中继升级为面向多人会议的选择性转发（SFU），单机并发能力提升至 10,000+ 路媒体流。

**核心能力**：
- 标准 TURN/STUN 协议支持（全功能兼容原生 coturn）
- DPDK / AF_XDP 高性能网络后端（零拷贝、用户态协议栈）
- 内置房间管理（SFU 广播转发，一人上传、多人接收）
- 分布式调度层（Conductor）实现多节点集群与负载均衡
- 支持多种数据库后端（SQLite、MySQL、PostgreSQL、Redis、MongoDB）

## 技术栈

- **语言**：C11（turbo 部分使用 GNU99 扩展）
- **构建系统**：主构建系统为 `configure` + `make`（兼容原生 coturn 的 CMake 备选方案）
- **核心依赖**：libevent2、OpenSSL (≥1.1.1)
- **Turbo 扩展依赖**：DPDK (≥22.07)、libbpf、libxdp (仅 AF_XDP 模式需要)

## 构建说明

### 标准模式构建（无 Turbo 扩展，完全兼容原生 coturn）

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

或使用传统 autotools 流程：

```bash
./configure
make -j$(nproc)
sudo make install
```

### Debug 构建

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Fuzzer 构建（需 Clang 或 AppleClang）

```bash
CC=clang CXX=clang++ cmake -S . -B build -DFUZZER=ON
cmake --build build -j$(nproc)
```

### Turbo DPDK 模式构建

```bash
./configure --turbo --use-dpdk
make -j$(nproc)
```

### Turbo AF_XDP 模式构建

```bash
./configure --turbo --use-afxdp
make -j$(nproc)
```

**主要 CMake 选项**：
- `-DFUZZER=ON` — 构建 OSS-Fuzz 目标（需要 Clang）
- `-DCMAKE_BUILD_TYPE=Debug|Release`
- `-DWITH_MYSQL=ON/OFF`、`-DWITH_PGSQL=ON/OFF`、`-DWITH_MONGO=ON/OFF`、`-DWITH_REDIS=ON/OFF`

**Turbo 专用 configure 选项**：
- `--turbo` — 启用 turbo 扩展功能
- `--use-dpdk` — 使用 DPDK 网络后端（需预先安装 DPDK 开发库）
- `--use-afxdp` — 使用 AF_XDP 网络后端（需 Linux 内核 ≥5.4 及 libbpf/libxdp）

构建产物包含：
- `turnserver` — TURN/STUN 主服务进程
- `turnadmin` — 用户数据库管理工具
- `turnutils` — 测试工具集（uclient、peer、stunclient）
- `conductor` — 分布式调度服务（仅 Turbo 构建）
- `turboserver` — 高性能数据平面进程（与 turnserver 集成，通过 `--turbo` 启动参数激活）

卸载命令：`cmake --build build --target uninstall` 或 `make uninstall`

## 代码风格

### 基础 C 代码（原生 coturn 部分）

所有 C 源码必须使用项目根目录下的 [.clang-format](.clang-format) 进行格式化：

```bash
find src -name '*.c' -o -name '*.h' | xargs clang-format -i
```

关键规则（基于 LLVM 风格）：
- 缩进：2 空格，**不使用制表符**
- 列宽限制：120
- 指针对齐：右对齐（`int *p`）
- 大括号风格：K&R（附着式）
- 栈缓冲区声明时立即零初始化：`uint8_t buf[N] = {0}` 或 `SomeStruct s = {0}`

### Turbo 扩展部分

Turbo 新增模块遵循以下补充规范（确保与原生代码风格可区分）：
- 缩进：**制表符**（Tab），宽度等效为 4 空格
- 函数命名：`snake_case`（如 `turbo_room_broadcast`）
- 结构体命名：`snake_case`（如 `turbo_room_mgr`）
- 宏命名：`UPPER_SNAKE_CASE`（如 `TURBO_MAX_ROOM_MEMBERS`）
- 头文件保护：使用 `#ifndef _TURBO_XXX_H_` 模式

### Turbo 拓展部分实现方案技术文档

Turbo 拓展部分实现方案参见这份技术文档：./coturn-turbo-终极方案完整技术文档.md

### 常见编码模式

- **端口类型**：所有端口字段及参数使用 `uint16_t`（而非 `int`）；端口 0 表示由操作系统分配临时端口。
- **栈缓冲区初始化**：声明时即零初始化（`= {0}`），不可仅在使用前才初始化。
- **HMAC 输出缓冲区**：声明为 `uint8_t buf[MAXSHASIZE] = {0}` — 缓冲区在 HMAC 计算前已被写入报文，未初始化的字节将短暂存在于数据包中。
- **未初始化结构体**：栈上分配的地址结构使用 `= {0}`（例如 `ioa_addr`）。
- **计数器溢出处理**：`turn_ports.c` 中的 `_turnports` 使用 `uint32_t` 计数器；比较时必须防止溢出（应使用差值比较，而非 `>=`）。
- **端口边界检查**：验证 `int` 型端口值时使用 `<= USHRT_MAX`（而非 `< USHRT_MAX`）——端口 65535 是合法的。
- **错误处理**：必须检查所有 OpenSSL / libevent 调用的返回值；HMAC 操作前调用 `ERR_clear_error()`。
- **日志记录**：使用 `TURN_LOG_FUNC` 系列宏，禁止直接使用 `fprintf` 或 `perror`。

## 测试

### 协议一致性测试（RFC 5769 测试向量）

```bash
cd examples && ./scripts/rfc5769.sh
```

### 基本 TURN 中继测试（先启动服务器）

```bash
cd examples && ./scripts/basic/relay.sh
cd examples && ./scripts/basic/udp_c2c_client.sh
```

### 完整测试套件

```bash
cd examples && ./run_tests.sh
```

数据库配置及扩展测试场景请参考 [docs/Testing.md](docs/Testing.md)。

### Turbo 模块单元测试

```bash
# 构建并运行 turbo 核心单元测试
make test_turbo
./test_turbo
```

## 源代码布局

```
coturn-turbo/
├── configure                     # 自动配置脚本（已扩展 DPDK/AF_XDP 检测）
├── CMakeLists.txt                # CMake 构建入口
├── .clang-format                 # 代码格式化规则
├── src/                          # 核心源代码
│   ├── client/                   # TURN 客户端库（C）
│   ├── client++/                 # TURN 客户端库（C++）
│   ├── server/                   # TURN/STUN 协议核心逻辑
│   │   ├── ns_turn_server.h      # 主服务器 API，会话管理
│   │   ├── ns_turn_maps.h        # 信道与权限映射
│   │   ├── ns_turn_allocation.h  # 中继分配管理（已扩展 room_id 字段）
│   │   └── ns_turn_ioalib.h      # I/O 抽象层
│   ├── apps/
│   │   ├── relay/                # turnserver 主进程
│   │   │   ├── mainrelay.c       # 入口、配置解析、多线程（已集成 turbo 初始化）
│   │   │   ├── netengine.c       # 网络引擎（每线程一个 libevent 循环，已添加 turbo 快速路径）
│   │   │   ├── turn_ports.c      # 端口分配与计数器逻辑
│   │   │   ├── tls_listener.c    # TCP/TLS 监听器
│   │   │   ├── dtls_listener.c   # DTLS/UDP 监听器
│   │   │   ├── userdb.c          # 用户数据库后端抽象
│   │   │   ├── http_server.c     # Prometheus 指标与管理接口
│   │   │   ├── turn_admin_server.c # Telnet 管理控制台
│   │   │   ├── acme.c            # ACME 证书管理
│   │   │   ├── turbo_core.c      # 【新增】turbo 引擎入口
│   │   │   ├── turbo_room.c      # 【新增】房间管理实现
│   │   │   ├── turbo_forward.c   # 【新增】转发逻辑实现
│   │   │   └── turbo_api.c       # 【新增】HTTP API 服务
│   │   ├── uclient/              # 命令行测试客户端
│   │   └── conductor/            # 【新增】分布式信令调度服务
│   │       ├── main.c            # conductor 主程序入口
│   │       ├── room_manager.c    # 全局房间状态管理
│   │       ├── signal_handler.c  # WebSocket 信令处理
│   │       └── api_server.c      # RESTful API 服务
│   ├── turbo/                    # 【新增】高性能网络与转发核心库
│   │   ├── network/              # 网络抽象层
│   │   │   ├── turbo_netif.h     # 后端抽象接口
│   │   │   ├── turbo_dpdk.c      # DPDK 后端实现
│   │   │   ├── turbo_af_xdp.c    # AF_XDP 后端实现
│   │   │   ├── turbo_port.c      # 单端口复用实现
│   │   │   └── xdp_prog.c        # eBPF/XDP 程序
│   │   ├── forward/              # 转发核心
│   │   │   ├── turbo_switch.c    # 快速转发与零拷贝克隆
│   │   │   └── turbo_fec.c       # FEC 前向纠错（可选）
│   │   └── utils/                # 工具函数
│   │       ├── turbo_hash.c      # 无锁哈希表
│   │       └── turbo_json.c      # JSON 解析
│   └── include/turn/             # 公共头文件
├── fuzzing/                      # OSS-Fuzz 目标（FuzzStun、FuzzOAuthToken 等）
├── examples/                     # 测试脚本与示例配置
├── turndb/                       # 数据库模式与安装脚本
├── conf/                         # 配置文件
│   └── turbo.conf.example        # 【新增】turbo 配置示例
└── scripts/                      # 部署与辅助脚本
```

## 架构概述

### 原生 coturn 架构

- **入口点**：`mainrelay.c` — 初始化全局状态、解析配置/命令行参数、启动工作线程。
- **线程模型**：一个主线程 + 可配置数量的工作线程（`-n` 参数）；每个工作线程运行独立的 libevent 事件循环。
- **I/O 抽象**：`ns_ioalib_engine_impl.c` 提供基于 libevent 的网络 I/O 层，该抽象便于接入定制化网络栈。
- **会话生命周期**：每个客户端连接创建一个带有唯一 ID 的会话；会话流程为：认证 → 分配 → 中继。
- **消息处理**：入向 STUN/TURN 报文在 `turnserver.c` 中解析，通过 HMAC-SHA1 认证，随后根据消息类型和信道状态进行分发。
- **数据库**：`userdb.c` 抽象多种后端，通过 CMake 选项在构建时选择；默认支持 SQLite，同时可选 MySQL、PostgreSQL、Redis、MongoDB。

### coturn-turbo 扩展架构

- **数据平面加速**：在 `netengine.c` 的 UDP 接收路径插入 Turbo 快速路径。若收到的数据包属于某个已关联房间的分配（allocation），则绕过原有逐包中继逻辑，直接进入 SFU 广播流水线。
- **网络后端抽象**：定义 `turbo_netif_ops` 统一接口，运行时通过配置选择 DPDK 或 AF_XDP 后端，实现内核旁路、批量收发及零拷贝。
- **单端口复用**：所有客户端媒体流收敛至 UDP 3478 单一端口，通过五元组（源 IP、源端口、目的 IP、目的端口、协议）进行 O(1) 会话查找，替代原生 coturn 的多端口分配模式。
- **房间与广播**：`turbo_room_mgr` 维护无锁成员表（基于 DPDK `rte_hash` 或自研哈希表），广播时对每个目标成员克隆数据包头部并重写地址，负载数据共享（零拷贝）。
- **分布式调度**：Conductor 服务通过一致性哈希将房间映射至特定的 Turboserver 节点，并通过 Redis 同步全局状态，实现集群的水平扩展。

## 常见开发模式与注意事项

### 原生部分

- **端口类型**：始终使用 `uint16_t`，边界检查包含 65535。
- **栈缓冲区**：声明时即零初始化（`= {0}`），避免未初始化数据进入报文。
- **计数器溢出**：`turn_ports.c` 中的计数器比较应使用 `(a - b) < threshold` 方式避免回绕问题。
- **日志**：使用 `TURN_LOG_FUNC` 宏，包含适当的日志级别。

### Turbo 扩展部分

- **五元组查找**：所有快速路径入口必须先从数据包提取五元组，在全局 `turbo_port_map` 中查找对应的 `turbo_allocation`，查找失败则回退到原生 STUN/TURN 处理流程。
- **房间成员迭代安全**：广播遍历房间成员时，采用 RCU 风格的延迟删除机制——删除成员时先标记删除，待所有正在进行的广播迭代完成后再释放资源，避免使用锁。
- **数据包克隆**：DPDK 后端使用 `rte_pktmbuf_attach()` 克隆 mbuf，AF_XDP 后端使用预分配的 UMEM 缓冲区配合 `memcpy` 或引用计数实现共享。克隆后必须单独重写每个目标的 IP/UDP 头部。
- **大页内存要求**：DPDK 模式必须预先配置大页；AF_XDP 模式建议配置大页以获得最佳性能。部署脚本应自动检查 `/proc/meminfo` 中的 `HugePages_Total`。
- **API 设计**：新增的 HTTP API（`/v1/room/*`）遵循 RESTful 风格，请求/响应体使用 JSON 格式，错误码遵循 HTTP 标准。

## 参考资料
- coturn-turbo 扩展架构实现实现方案：./coturn-turbo-终极方案完整技术文档.md
- coturn 官方仓库：https://github.com/coturn/coturn
- coturn 开发指南：https://deepwiki.com/coturn/coturn/7.2-development-guide-and-code-structure
- DPDK 文档：https://doc.dpdk.org/
- AF_XDP 内核文档：https://www.kernel.org/doc/html/latest/networking/af_xdp.html
- libbpf 文档：https://libbpf.readthedocs.io/