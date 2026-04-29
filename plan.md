# coturn-turbo 单端口UDP relay 方案开发计划

## 阶段一：基础功能改造

### 1. 架构预处理
- [ ] 阅读梳理 coturn 源码结构，理解 relay 端口分配、socket管理、session映射流程
- [ ] 明确将要hook/重构的代码入口（如 turn_ports.c、turn_server.c、mainrelay.c）

### 2. 单端口UDP relay管理
- [ ] 实现全局relay UDP socket创建与绑定（如配置端口40000）
- [ ] 将 relay UDP分配逻辑修改为强制分配该固定端口
- [ ] 创建 session <-> username/tuple 的 hash/map 映射结构
- [ ] 修改/重写收包主循环，实现 single socket 多session demux（基于 username/tuple/session-id）

### 3. 数据流重构
- [ ] 修改发送、转发逻辑，保证所有relay输出走同一UDP fd
- [ ] 完善session归属校验，非法数据丢弃/记录
- [ ] 接入session状态管理、回收、过期清理

## 阶段二：兼容性和安全性加固

- [ ] 多客户端/多session并发压力测试（模拟同时百/千用户relay）
- [ ] 对多种NAT与ICE场景进行回归验证
- [ ] 安全隔离测试（同端口不同session间的数据不可混淆、不泄漏）
- [ ] 基本速率限制/DoS防护patch
- [ ] 日志观测、故障告警机制完善

## 阶段三：性能优化与扩展

- [ ] 提炼IO事件收发主循环为可插拔后端（libevent/epoll/io_uring 预留）
- [ ] 撰写io_uring udp eventloop的初步demo，配置build宏与接口
- [ ] 分析性能数据，必要时研究 AF_XDP 集成可行性与demo落盘
- [ ] 优化 session查找数据结构（大流量下hash/lru效率保障）

## 阶段四：文档与CI/CD

- [ ] 编写修改后的 usage 手册和配置指南，强化端口分配/部署说明
- [ ] 示例配置与基本benchmark、兼容性用例收录
- [ ] 持续集成测试脚本完善、合并主干代码更新策略

---

## 任务进度推进说明

- 各条任务按优先级和依赖关系依次推进
- 每阶段可拉分任务子分支，由方案作者/社区成员协同完成
- 阶段目标完成后建议输出 milestone review 总结复盘

---

如需细化每个任务的分解与代码入口指引，可按需补充与调整.