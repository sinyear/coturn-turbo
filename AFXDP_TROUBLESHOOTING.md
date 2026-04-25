# AF_XDP 模式外部无法访问排查文档

## 问题描述

在云服务器（单队列 virtio_net）上启用 AF_XDP 模式（auto/skb）后：
- 外部机器无法访问 3478 和 9999 端口
- SSH 也无法连接
- 标准模式（非 DPDK/AF_XDP）正常工作
- turnserver 停止后，XDP 程序仍残留在网卡上

---

## 根因分析

### 问题 1：XDP 程序退出时未从网卡分离

**现象**：`turnserver` 停止后 `ip link show eth0` 仍然显示 `prog/xdp id xx name xdp_dispatcher`

**根因**：`remove_xdp_prog()` 仅调用 `xdp_program__close()` 关闭了用户态句柄，**没有从网卡上 detach XDP 程序**。libxdp dispatcher 继续拦截所有流量。

**修复**（`turbo_af_xdp.c`）：
- 新增 `remove_all_xdp_progs()` 使用 `xdp_multiprog__detach()` 一次性移除整个 dispatcher 链
- 重写 `remove_xdp_prog()` 在关闭句柄后强制调用清理函数

### 问题 2：xdp_prog.o 编译时缺少 BTF 信息

**现象**：`bpftool prog loadall /usr/local/share/turnserver/xdp_prog.o /sys/fs/bpf/test_xdp` 报错：
```
libbpf: BTF is required, but is missing or corrupted.
```

**根因**：Makefile 中编译 BPF 程序的命令缺少 `-g` 参数，导致没有生成 BTF（BPF Type Format）信息。libxdp 的 `xdp_program__open_file()` 要求 BPF 程序包含 BTF。

**修复**（`Makefile.in`）：
```diff
- $(CLANG) -O2 -target bpf -I/usr/include -c $< -o $@
+ $(CLANG) -O2 -target bpf -g -I/usr/include -c $< -o $@
```

### 问题 3：xdp_simple section 不被 libbpf 识别

**现象**：加载 xdp_prog.o 时报错：
```
libbpf: failed to guess program type from ELF section 'xdp_simple'
```

**根因**：`xdp_prog.c` 中定义了 `SEC("xdp_simple")` section，这不是 libbpf 支持的标准 section 名称。libbpf 在加载整个 `.o` 文件时会因无法识别的 section 而失败。

**修复**（`xdp_prog.c`）：删除 `xdp_filter_simple` 函数（从未被使用，只是无用的 fallback）

### 问题 4：xsk_def_prog 自动安装拦截所有流量

**现象**：`bpftool prog show` 显示三个 XDP 程序：
```
237: ext  name xdp_filter      ← 自定义程序
243: xdp  name xdp_dispatcher  ← libxdp dispatcher
246: ext  name xsk_def_prog    ← libbpf 自动安装！
```

**根因**：`xsk_socket__create_shared()` 内部会自动安装一个 `xsk_def_prog` 到网卡上，该程序将 **所有** 流量重定向到 AF_XDP socket。在单队列云 VM 上，所有流量走 queue 0，导致 SSH/HTTP 等全部被拦截。

**修复**（`turbo_af_xdp.c`）：
```diff
- xsk_cfg.libbpf_flags = 0;
+ xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
```

### 问题 5：port_map BPF map 未填充端口值

**现象**：自定义 XDP 程序的 `port_map` map 中值为 0，永远无法匹配到端口 3478 的流量。

**根因**：`xdp_prog.c` 中的 eBPF 程序通过 `bpf_map_lookup_elem(&port_map, &port_key)` 读取目标端口，但用户态代码 **从未向该 map 写入值**。当 port_map[0] = 0 时，所有 UDP 包都返回 XDP_PASS。

但此时 dispatcher 已经接管流量，且 `xsk_def_prog` 会将所有流量重定向到 AF_XDP socket。两者叠加导致所有外部流量都无法到达内核协议栈。

**修复**（`turbo_af_xdp.c`）：
- 新增 `populate_port_map()` 函数：通过 `bpf_obj_get_info_by_fd()` 获取程序关联的 map 列表，找到 `port_map` 后写入 `htons(port)` 值
- 在 `turbo_afxdp_init()` 中，XDP 程序加载成功后立即调用此函数

### 问题 6：安装路径不一致

**现象**：`make install` 安装到 `/usr/local/share/turnserver/xdp_prog.o`，但代码搜索路径只有 `/usr/share/turnserver/xdp_prog.o`

**修复**（`turbo_af_xdp.c`）：增加 `/usr/local/share/turnserver/xdp_prog.o` 到搜索路径列表

---

## 修复文件汇总

| 文件 | 修改内容 |
|------|----------|
| `src/turbo/network/turbo_af_xdp.c` | 1. 添加 `#include <bpf/bpf.h>` 和 `#include <arpa/inet.h>`<br>2. 新增 `populate_port_map()` 填充 BPF map<br>3. 新增 `remove_all_xdp_progs()` 强制清理 XDP<br>4. 重写 `remove_xdp_prog()` 调用完整清理流程<br>5. `libbpf_flags` 使用 `INHIBIT_PROG_LOAD` 禁止自动安装<br>6. 添加 `/usr/local/share/turnserver/xdp_prog.o` 搜索路径<br>7. 保存 `ifindex` 到 priv 结构体供清理使用 |
| `src/turbo/network/turbo_af_xdp.h` | 新增 `ifindex` 字段到 `turbo_afxdp_priv` 结构体 |
| `src/turbo/network/xdp_prog.c` | 1. 删除无效的 `xdp_simple` section<br>2. 保留 `SEC("xdp")` 和 `port_map` |
| `Makefile.in` | 编译 xdp_prog.o 时添加 `-g` 参数生成 BTF 信息 |

---

## 诊断命令速查

### 查看 XDP 程序状态
```bash
# 查看网卡是否挂载 XDP 程序
ip link show eth0

# 查看所有已加载的 BPF/XDP 程序
bpftool prog show

# 查看网卡上的 XDP 绑定
bpftool net show dev eth0

# 查看所有 BPF map
bpftool map show
```

### 验证 xdp_prog.o 有效性
```bash
# 检查文件格式
file /usr/local/share/turnserver/xdp_prog.o

# 验证 BTF 信息（新版 libbpf 要求）
bpftool prog loadall /usr/local/share/turnserver/xdp_prog.o /sys/fs/bpf/test_xdp 2>&1

# 清理测试残留
rm -f /sys/fs/bpf/test_xdp*
```

### 清除残留 XDP
```bash
# 方式 1：使用 xdp-loader
xdp-loader unload eth0 -a

# 方式 2：使用 ip 命令
ip link set dev eth0 xdp off

# 方式 3：使用 bpftool
bpftool net detach xdp dev eth0
```

### 启动日志关键信息
正常启动应该看到：
```
af_xdp: interface 'eth0' -> ifindex=2
af_xdp: XDP program '/usr/local/share/turnserver/xdp_prog.o' loaded in skb mode
af_xdp: port_map set to 3478
af_xdp: successfully initialized interface 'eth0' (ifindex=2, mode=skb)
TURBO components initialized successfully
```

异常启动可能看到：
```
af_xdp: xdp_prog.o not found in any search path    # 路径问题
af_xdp: failed to open XDP program (errno=2)        # BTF 缺失
af_xdp: port_map not found in XDP program maps      # map 未找到
```

---

## 关键技术点

### 单队列云 VM 的特殊性

云服务器通常只有一个 combined queue（`ethtool -l eth0` 显示 `Combined: 1`）。在单队列环境下：

1. 所有入站流量都走 queue 0
2. AF_XDP socket 绑定到 queue 0 会捕获该队列上的 **所有** 流量
3. 必须依赖 XDP 程序在 BPF 层做包过滤，只将 TURN 端口流量重定向到 AF_XDP
4. 如果 XDP 程序未正确清理，所有外部流量（包括 SSH）都会被拦截

### libxdp Dispatcher 机制

libxdp 使用 `xdp_dispatcher` 作为多程序共享的中间层：
- `xdp_program__attach()` 会自动安装 dispatcher（如果不存在）
- 用户程序通过 `xdp_program__detach()` 从 dispatcher 中移除
- 但 `xdp_program__close()` 只关闭用户态句柄，**不会 detach**
- 因此清理时必须调用 detach 而非仅 close

### AF_XDP 自动 XDP 程序

`xsk_socket__create_shared()` 默认会安装 `xsk_def_prog`：
- 该程序将所有到达 queue 的流量重定向到 AF_XDP socket
- 这是设计行为，适合专用网卡/多队列场景
- 在单队列共享网卡上会导致所有服务不可用
- 使用 `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD` 可禁止自动安装
