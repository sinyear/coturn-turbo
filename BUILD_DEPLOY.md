# coturn-turbo 构建、打包、部署与测试指南

本文档描述 coturn-turbo 三种构建模式的完整生命周期：**标准 TURN 模式**、**DPDK Turbo 模式**、**AF_XDP Turbo 模式**。

---

## 目录

- [一、环境准备](#一环境准备)
- [二、三种模式构建](#二三种模式构建)
  - [2.1 标准 TURN 模式](#21-标准-turn-模式)
  - [2.2 DPDK Turbo 模式](#22-dpdk-turbo-模式)
  - [2.3 AF_XDP Turbo 模式](#23-af_xdp-turbo-模式)
- [三、打包](#三打包)
- [四、部署](#四部署)
  - [4.1 标准模式部署](#41-标准模式部署)
  - [4.2 DPDK 模式部署](#42-dpdk-模式部署)
  - [4.3 AF_XDP 模式部署](#43-af_xdp-模式部署)
  - [4.4 Conductor 调度服务部署](#44-conductor-调度服务部署)
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
| 标准 TURN | 无 | 任意 Linux |
| DPDK Turbo | DPDK ≥22.07、NUMA 库 | 无强制要求 |
| AF_XDP Turbo | libbpf、libxdp、libjson-c、clang/llvm | Linux ≥5.4（推荐 ≥5.10） |

---

## 二、三种模式构建

### 2.1 标准 TURN 模式

完全兼容原生 coturn，无 Turbo 扩展，适合不需要 SFU 广播的场景。

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
- `bin/turnserver` — TURN/STUN 主服务
- `bin/turnadmin` — 用户数据库管理工具
- `bin/turnutils_*` — 测试工具集

### 2.2 DPDK Turbo 模式

最高性能模式，适合物理服务器、专用网卡场景。

#### 2.2.1 安装 DPDK

```bash
# 安装 DPDK 编译依赖
apt-get install -y meson ninja-build libnuma-dev python3-pyelftools

# 下载并编译 DPDK（推荐 22.07+）
wget https://fast.dpdk.org/rel/dpdk-22.07.tar.xz
tar xJf dpdk-22.07.tar.xz
cd dpdk-22.07
meson build --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu
cd build
ninja
sudo ninja install
sudo ldconfig

# 验证 DPDK 安装
pkg-config --modversion libdpdk
```

#### 2.2.2 编译 coturn-turbo

```bash
cd coturn-turbo

# 生成 Makefile 并注入 DPDK 编译标志
./configure CFLAGS="-DTURN_USE_DPDK $(pkg-config --cflags libdpdk 2>/dev/null)"

# 编译
make -j$(nproc)

# 安装
sudo make install
```

> **关键**：`CFLAGS` 中的 `-DTURN_USE_DPDK` 宏决定编译时选择 DPDK 网络后端。Makefile 会自动检测该宏并编译 `turbo_dpdk.c`、链接 `libdpdk`。

### 2.3 AF_XDP Turbo 模式

高性能且可与内核共享网卡，适合云服务器、虚拟化环境。

#### 2.3.1 安装 AF_XDP 依赖

```bash
# Ubuntu/Debian（内核 ≥5.4）
apt-get install -y clang llvm libbpf-dev libxdp-dev libjson-c-dev

# CentOS/RHEL（需 EPEL + elrepo）
yum install -y clang llvm libbpf-devel libxdp-devel json-c-devel
```

#### 2.3.2 编译 coturn-turbo

```bash
cd coturn-turbo

# 生成 Makefile 并注入 AF_XDP 编译标志
./configure CFLAGS="-DTURN_USE_AFXDP"

# 编译
make -j$(nproc)

# 安装
sudo make install
```

> **关键**：`CFLAGS` 中的 `-DTURN_USE_AFXDP` 宏决定编译时选择 AF_XDP 网络后端。Makefile 会自动检测该宏并编译 `turbo_af_xdp.c`、链接 `-lbpf -lxdp -ljson-c`。

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

### 3.2 Turbo 模式打包（DPDK / AF_XDP）

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

# 复制部署脚本
cp scripts/install_coturn_on_aws_ec2.sh ${PKG_DIR}/scripts/ 2>/dev/null || true

# 如果是 DPDK 模式，附加说明
cat > ${PKG_DIR}/README.txt << 'EOF'
coturn-turbo (DPDK 模式) 部署包
================================
1. 配置大页内存: echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
2. 绑定网卡到 DPDK: sudo dpdk-devbind.py -b vfio-pci <PCI地址>
3. 编辑 etc/turnserver.conf，设置 turbo=true 和 turbo-api-port
4. 以 root 运行: sudo bin/turnserver -c etc/turnserver.conf --turbo -o
EOF

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

### 4.2 DPDK 模式部署

#### 4.2.1 大页内存配置

```bash
# 分配 1024 个 2MB 大页（共 2GB）
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 挂载大页文件系统
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# 验证
grep Huge /proc/meminfo
```

#### 4.2.2 网卡绑定

```bash
# 加载 VFIO 驱动
sudo modprobe vfio-pci

# 查看网卡 PCI 地址
ip link show
# 找到目标网卡对应的 PCI 地址，如 0000:02:00.0

# 将网卡从内核驱动解绑，绑定到 vfio-pci
sudo dpdk-devbind.py -b vfio-pci 0000:02:00.0

# 验证绑定状态
dpdk-devbind.py --status
```

#### 4.2.3 配置文件

编辑 `/etc/turnserver/turnserver.conf`：

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=0.0.0.0
realm=north
lt-cred-mech
user=claude:password

# Turbo 模式（必须）
turbo=true
turbo-api-port=9999
```

#### 4.2.4 启动

```bash
# DPDK 模式需要 root 权限
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

#### 4.2.5 部署要求

| 维度 | 要求 |
|------|------|
| CPU | x86-64，建议预留 1-2 个核心给 DPDK（isolcpus） |
| 内存 | 大页内存 ≥1GB |
| 网卡 | DPDK 兼容（Intel X500/700/800、Mellanox ConnectX-4/5/6） |
| 权限 | 需要 root 或 CAP_SYS_ADMIN |
| BIOS | 建议启用 VT-d/AMD-Vi (IOMMU) |

### 4.3 AF_XDP 模式部署

#### 4.3.1 内核检查

```bash
# 检查内核版本（需 ≥5.4）
uname -r

# 检查 XDP 支持
ethtool -k eth0 | grep xdp
```

#### 4.3.2 配置文件

编辑 `/etc/turnserver/turnserver.conf`：

```ini
listening-port=3478
listening-ip=0.0.0.0
relay-ip=0.0.0.0
realm=north
lt-cred-mech
user=claude:password

# Turbo 模式
turbo=true
turbo-api-port=9999
```

#### 4.3.3 启动

```bash
# AF_XDP 模式需要 root 权限（加载 XDP 程序）
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v
```

#### 4.3.4 部署要求

| 维度 | 要求 |
|------|------|
| 内核 | Linux ≥5.4（推荐 ≥5.10） |
| 网卡 | XDP 原生模式兼容（Intel i40e/ice/ixgbe、Mellanox mlx5） |
| 网卡独占 | 无需，可与内核共享 |
| 权限 | 需要 root 或 CAP_BPF + CAP_NET_ADMIN |

### 4.4 Conductor 调度服务部署

Conductor 是独立进程，用于多节点集群的房间调度与信令管理。

#### 4.4.1 编译

```bash
cd coturn-turbo/src/apps/conductor
# Conductor 编译由主 Makefile 处理
cd ../../..
make conductor 2>/dev/null || true
```

#### 4.4.2 启动

```bash
# 需要 Redis 作为后端
./conductor --listen 0.0.0.0 --port 8080 --ws-port 8765 \
    --redis "redis://127.0.0.1:6379/0" --node-id "conductor-1"
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

### 5.2 Turbo 模式（DPDK / AF_XDP）

```bash
# 前台运行（调试）
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -v

# 后台运行
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v

# 指定 API 端口
sudo turnserver -c /etc/turnserver/turnserver.conf \
    --turbo --turbo-api-port 9999 -o -v
```

### 5.3 验证运行状态

```bash
# 检查进程
ps aux | grep turnserver

# 检查端口
ss -ulnp | grep 3478
ss -ulnp | grep 9999   # Turbo API 端口

# 检查日志
tail -f /var/log/turnserver/turnserver_*.log

# 测试 Turbo API（Turbo 模式）
curl http://localhost:9999/v1/room/list
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

#### 6.2.1 Turbo API 测试

```bash
# 列出房间（应返回空列表）
curl -s http://localhost:9999/v1/room/list | python3 -m json.tool

# 创建房间
curl -s -X POST http://localhost:9999/v1/room/create \
    -H "Content-Type: application/json" \
    -d '{"room_id":"room123"}' | python3 -m json.tool

# 加入房间
curl -s -X POST http://localhost:9999/v1/room/join \
    -H "Content-Type: application/json" \
    -d '{"room_id":"room123","member_id":"user1","ip":"192.168.1.10","port":50000}' \
    | python3 -m json.tool

# 查询房间信息
curl -s "http://localhost:9999/v1/room/info?room_id=room123" | python3 -m json.tool

# 离开房间
curl -s -X POST http://localhost:9999/v1/room/leave \
    -H "Content-Type: application/json" \
    -d '{"room_id":"room123","member_id":"user1"}' | python3 -m json.tool
```

#### 6.2.2 性能测试

```bash
# 监控 CPU 和内存
cd examples
./cpu-mem.sh

# 在另一个终端启动服务器并运行压力测试
# 使用 turnutils_uclient 进行并发连接测试
bin/turnutils_uclient -n 100 -u claude -w password -T 10 127.0.0.1
```

#### 6.2.3 负载测试

```bash
# 终端 1: 启动 Turbo 服务器
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo -o -v

# 终端 2: 创建房间并加入多个客户端
for i in $(seq 1 50); do
    curl -s -X POST http://localhost:9999/v1/room/join \
        -H "Content-Type: application/json" \
        -d "{\"room_id\":\"perf_test\",\"member_id\":\"user${i}\",\"ip\":\"10.0.0.${i}\",\"port\":$((50000+i))}"
done

# 查看房间状态
curl -s "http://localhost:9999/v1/room/info?room_id=perf_test" | python3 -m json.tool
```

---

## 七、故障排查

### 7.1 标准模式

| 问题 | 解决方案 |
|------|----------|
| 端口被占用 | `ss -ulnp \| grep 3478`，关闭占用进程 |
| 认证失败 | 检查 `user=` 配置格式为 `username:password` |
| 日志无法写入 | 检查 `/var/log/turnserver/` 目录权限 |

### 7.2 DPDK 模式

| 问题 | 解决方案 |
|------|----------|
| `Failed to initialize turbo network interface` | 检查网卡是否正确绑定到 vfio-pci |
| 大页内存不足 | `grep Huge /proc/meminfo`，增加 nr_hugepages |
| EAL 初始化失败 | 确保以 root 运行，检查 IOMMU 是否在 BIOS 中启用 |
| 启动崩溃（`turbo=true`） | 确认编译时带 `-DTURN_USE_DPDK`，运行 `turnserver --version` 检查 |

### 7.3 AF_XDP 模式

| 问题 | 解决方案 |
|------|----------|
| `Failed to initialize turbo network interface` | 检查接口名是否正确，确认内核 ≥5.4 |
| XDP 程序加载失败 | 安装 libbpf-dev、libxdp-dev，检查网卡驱动是否支持 XDP |
| 启动崩溃（`turbo=true`） | 确认编译时带 `-DTURN_USE_AFXDP`，运行 `turnserver --version` 检查 |

### 7.4 通用排查

```bash
# 查看最详细日志
turnserver -c /etc/turnserver/turnserver.conf --turbo -V

# 检查编译模式（通过启动日志判断）
turnserver -c /etc/turnserver/turnserver.conf --turbo -v 2>&1 | grep -i turbo

# 检查动态库依赖
ldd bin/turnserver | grep -E "dpdk|bpf|xdp"
```

---

## 八、配置说明

### 8.1 Turbo 相关配置参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `turbo` | bool | false | 启用 Turbo 模式。仅在编译时包含 `-DTURN_USE_DPDK` 或 `-DTURN_USE_AFXDP` 时可设为 true |
| `turbo-api-port` | uint16 | 0（禁用） | Turbo HTTP API 端口，提供房间管理 REST 接口 |

### 8.2 三种模式对比

| 特性 | 标准模式 | DPDK Turbo | AF_XDP Turbo |
|------|----------|------------|--------------|
| 编译标志 | 无 | `-DTURN_USE_DPDK` | `-DTURN_USE_AFXDP` |
| 网络后端 | 内核协议栈 | DPDK 用户态 | AF_XDP 内核旁路 |
| 端口模型 | 每会话分配端口 | 单端口复用 (3478) | 单端口复用 (3478) |
| SFU 广播 | 不支持 | 支持 | 支持 |
| 房间管理 | 不支持 | 支持 | 支持 |
| HTTP API | 管理控制台 | +房间管理 REST API | +房间管理 REST API |
| 并发能力 | ~500-2000 流 | ~10,000+ 流 | ~5,000 流 |
| P99 延迟 | ~200µs | ~30µs | ~60µs |
| 网卡独占 | 否 | 是（绑定到 vfio-pci） | 否（与内核共享） |
| 部署复杂度 | 低 | 高 | 中 |

### 8.3 配置文件位置

| 文件 | 说明 |
|------|------|
| `examples/etc/turnserver.conf` | 完整参考配置，包含所有 coturn 原生选项 + Turbo 选项（文档末尾） |
| `conf/turbo.conf.example` | Turbo 模式最小配置示例，可直接使用 |

---

## 附录：快速参考卡

### 标准模式一键启动

```bash
./configure && make -j$(nproc) && sudo make install
sudo cp examples/etc/turnserver.conf /etc/turnserver/turnserver.conf
sudo turnserver -c /etc/turnserver/turnserver.conf -o
```

### DPDK 模式一键启动

```bash
# 1. 安装 DPDK
apt-get install -y meson ninja-build libnuma-dev
# (下载编译 DPDK 22.07+，参见 2.2.1)

# 2. 编译
./configure CFLAGS="-DTURN_USE_DPDK $(pkg-config --cflags libdpdk 2>/dev/null)"
make -j$(nproc) && sudo make install

# 3. 配置
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo modprobe vfio-pci
sudo dpdk-devbind.py -b vfio-pci 0000:02:00.0

# 4. 启动
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo --turbo-api-port 9999 -o -v
```

### AF_XDP 模式一键启动

```bash
# 1. 安装依赖
apt-get install -y clang llvm libbpf-dev libxdp-dev libjson-c-dev

# 2. 编译
./configure CFLAGS="-DTURN_USE_AFXDP"
make -j$(nproc) && sudo make install

# 3. 启动
sudo turnserver -c /etc/turnserver/turnserver.conf --turbo --turbo-api-port 9999 -o -v
```
