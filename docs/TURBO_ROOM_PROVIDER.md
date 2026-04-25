# TURBO ROOM PROVIDER 开发指南

> **版本**: v2.0  
> **适用**: coturn-turbo 房间广播功能  
> **前提**: 已了解 coturn-turbo 基本配置和房间广播概念，参见 `TURBO.md`。

## 1. 概述

**Room Identity Provider** 是 coturn-turbo 房间广播功能的核心扩展机制。它定义了一个抽象接口，用于从 TURN 认证信息中提取**房间 ID** 和**成员 ID**。

该设计的目的是：

- **解耦**：coturn-turbo 不包含任何特定信令服务的业务逻辑。
- **可扩展**：允许集成各种外部鉴权系统，而无需修改 C 源码。
- **安全**：支持 HMAC 签名验证防篡改，支持 Token 过期检查。

## 2. 抽象接口

所有 Provider 必须实现 `turbo_room_provider_ops` 结构体，定义在 `src/turbo/room/turbo_room_provider.h`：

```c
#define MAX_ROOM_ID_LEN   64
#define MAX_MEMBER_ID_LEN 64

struct turbo_room_info {
    char room_id[MAX_ROOM_ID_LEN];
    char member_id[MAX_MEMBER_ID_LEN];
    uint64_t expiry;                     // Unix timestamp，0 表示永不过期
};

struct turbo_room_provider_ops {
    const char *name;                     // 提供者名称，用于配置选择

    int (*init)(void *config);            // 初始化，可读取配置
    int (*extract)(const char *username,
                   const char *realm,
                   const char *credential,
                   struct sockaddr_in6 *client_addr,
                   struct turbo_room_info *info);  // 核心提取逻辑
    void (*deinit)(void);                 // 清理
};
```

### 2.1 `extract` 方法详解

**参数**：
- `username`：客户端在 TURN Allocate 请求中提供的 USERNAME 属性值。
- `realm`：认证域。
- `credential`：客户端提供的密码（在 long-term credential 模式下有用，HMAC 模式下该值通常被忽略）。
- `client_addr`：客户端源地址，可用于审计或精细化控制。
- `info`：输出参数，填充 `room_id`, `member_id`, `expiry`。

**返回值**：
- `0`：成功提取，该 allocation 将关联到指定房间，并作为成员加入。
- `-1`：该流量不属于任何房间，应走标准 TURN 一对一转发。
- `-2`：提取失败或验证不通过（例如 Token 签名错误、过期），coturn-turbo 将拒绝该 Allocate 请求并返回 401。

**调用时机**：在 TURN Allocate 成功、分配地址之前调用。因此，验证失败将直接阻止分配，避免恶意连接占用资源。

## 3. 内置 Provider

### 3.1 `static` – 静态用户名解析

**配置值**：`static`

**功能**：直接从 `username` 字符串中解析 `room_id` 和 `member_id`，无需额外验证。

**格式要求**：
- 格式 1：`room<RoomID>:<MemberID>`  例如 `room123:user456`
- 格式 2：`room<RoomID>:user<MemberID>` 例如 `room123:user456`
- 也可解析简单的 `room123:456`。

**安全性**：无加密验证，客户端可自行构造。**仅适用于内网受信任环境或开发测试**。

**配置示例**：
```ini
turbo-rooms
turbo-room-id-provider static
```

**客户端用法**：
```javascript
iceServers: [{
  urls: "turn:server:3478",
  username: "room123:userA",   // 直接编码房间和成员
  credential: "any_password"
}]
```

### 3.2 `token_hmac` – HMAC 签名 Token

**配置值**：`token_hmac`

**功能**：使用共享密钥验证一个类 JWT 的 Token，提取其中的 `room_id`、`member_id` 和 `exp` 过期时间。即使客户端修改 Token，签名验证也会失败，从而防止越权。

**安全性**：高。Token 不可伪造，支持过期控制。

**依赖配置**：
- `turbo-room-token-secret`：与信令服务共享的 HMAC-SHA256 密钥。
- `turbo-room-token-expiry`：默认 Token 有效期（秒），Token 自身也可携带 `exp` 字段覆盖。

**Token 格式**：
```
header.payload.signature
```
其中各部分均为 Base64URL 编码：
- `header`：`{"alg":"HS256","typ":"JWT-like"}`
- `payload`：`{"room_id":"123","member_id":"userA","exp":1678886400}`
- `signature`：`HMAC-SHA256(secret, header + "." + payload)`

**信令服务生成 Token 示例 (Node.js)**：
```javascript
const crypto = require('crypto');

function generateRoomToken(roomId, memberId, secret, ttlSeconds = 3600) {
    const header = { alg: "HS256", typ: "JWT-like" };
    const body = {
        room_id: String(roomId),
        member_id: String(memberId),
        exp: Math.floor(Date.now() / 1000) + ttlSeconds
    };

    const encode = (obj) => Buffer.from(JSON.stringify(obj)).toString('base64url');
    const headerEnc = encode(header);
    const bodyEnc = encode(body);
    const signature = crypto.createHmac('sha256', secret)
                            .update(`${headerEnc}.${bodyEnc}`)
                            .digest('base64url');

    return `${headerEnc}.${bodyEnc}.${signature}`;
}
```

**配置示例**：
```ini
turbo-rooms
turbo-room-id-provider token_hmac
turbo-room-token-secret super_secret_key_123
turbo-room-token-expiry 3600
```

**客户端用法**：同标准 TURN，仅需将 token 作为 username 传入。
```javascript
iceServers: [{
  urls: "turn:server:3478",
  username: token,
  credential: "dummy"  // 值无关紧要，安全由 token 保证
}]
```

### 3.3 `lua_script` – Lua 脚本自定义解析

**配置值**：`lua_script`

**功能**：调用用户编写的 Lua 脚本进行身份识别，可以实现任意复杂的逻辑。例如，查询 Redis 判断用户所属房间，或者调用外部 HTTP API。

**依赖**：编译时需链接 LuaJIT 或 Lua 5.1+，需安装 `libluajit-dev`。

**配置示例**：
```ini
turbo-rooms
turbo-room-id-provider lua_script
turbo-room-lua-script /etc/turn/room_extractor.lua
```

**Lua 脚本规范**：  
脚本必须定义一个全局函数 `extract_room_info`，接收三个字符串参数：`username`, `realm`, `client_addr`（格式 `"IP:port"`），并返回两个值 `room_id, member_id`。若返回 `nil, nil` 则表示非房间流量。

**示例脚本**：
```lua
-- /etc/turn/room_extractor.lua

-- 简单示例：从 Redis 获取用户当前房间
local redis = require("redis")
local client = redis.connect("127.0.0.1", 6379)

function extract_room_info(username, realm, client_addr)
    local room_id = client:get("user:" .. username .. ":current_room")
    if room_id then
        local member_id = client:get("user:" .. username .. ":member_id") or username
        return room_id, member_id
    end
    return nil, nil  -- 不关联房间，走标准 TURN
end
```

**错误处理**：若 Lua 脚本执行出错或返回不符合预期，coturn-turbo 将记录日志并视为非房间流量（返回 -1），不会中断服务。

## 4. 开发自定义 Provider

### 4.1 步骤概览

1. 在 `src/turbo/room/` 下创建新文件，如 `provider_myauth.c`。
2. 实现 `turbo_room_provider_ops` 的全部方法。
3. 将你的 Provider 注册到 `turbo_room_provider_register()` 调用中（在 `turbo_room.c` 的初始化函数内）。
4. 更新 `configure.ac` 中相关编译选项（如果有额外依赖）。
5. 重新编译。

### 4.2 最小实现示例

**`provider_myauth.c`**：
```c
#include "turbo_room_provider.h"
#include <string.h>
#include <stdlib.h>

static char my_config_value[256];

static int myauth_init(void *config) {
    if (config) {
        strncpy(my_config_value, (const char*)config, sizeof(my_config_value));
    }
    // 可以在此建立数据库连接等
    return 0;
}

static int myauth_extract(const char *username,
                          const char *realm,
                          const char *credential,
                          struct sockaddr_in6 *client_addr,
                          struct turbo_room_info *info) {
    // 示例：假设 username 格式为 "roomID:memberID"
    char room[64], member[64];
    if (sscanf(username, "%63[^:]:%63s", room, member) == 2) {
        strncpy(info->room_id, room, MAX_ROOM_ID_LEN);
        strncpy(info->member_id, member, MAX_MEMBER_ID_LEN);
        info->expiry = 0; // 永不过期
        return 0;         // 房间流量
    }
    return -1; // 非房间流量
}

static void myauth_deinit(void) {
    // 清理资源
}

struct turbo_room_provider_ops myauth_provider_ops = {
    .name = "myauth",
    .init = myauth_init,
    .extract = myauth_extract,
    .deinit = myauth_deinit,
};
```

然后在 `turbo_room.c` 的初始化处注册：
```c
extern struct turbo_room_provider_ops myauth_provider_ops;
turbo_room_provider_register(&myauth_provider_ops);
```

配置时使用：
```ini
turbo-room-id-provider myauth
# 可选传递自定义配置字符串
# turbo-room-provider-config "some_init_param"
```

### 4.3 安全注意事项

- **输入校验**：`username` 可能来自未认证的客户端，其长度可能很大，需进行边界检查。
- **错误日志**：验证失败时记录客户端 IP 和原因，但**不要**记录敏感密钥或完整 Token。
- **性能**：`extract` 在每次 Allocate 时调用，应保持轻量，避免阻塞操作（如长时间网络调用）。如有必要，可缓存结果或使用异步机制。
- **时间检查**：如需使用 Token 过期，务必比较服务器当前时间，并考虑时钟偏差。

## 5. 配置与选择

在 `turnserver.conf` 中通过 `turbo-room-id-provider` 指定，该值必须与 Provider 的 `name` 字段完全匹配。内置的三个名称为：`static`, `token_hmac`, `lua_script`。自定义的也同理。

若配置了不存在的 Provider 名称，启动时会报错并退出。

## 6. 集成现有信令系统

多数信令系统已支持生成某种 Token 或自定义 TURN 凭证。推荐使用 `token_hmac` 方式：

1. 信令服务与 coturn-turbo 共享同一密钥。
2. 当用户加入房间时，信令服务生成短效 Token，其中包含 `room_id` 和 `member_id`。
3. 将 Token 作为 TURN `username` 下发给客户端。
4. 当用户离开房间时，信令服务不再提供新的 Token；旧 Token 过期后自动失效。
5. coturn-turbo 在 allocation 销毁时自动清理房间成员关系。

这种方式实现了**完全无状态化**：coturn-turbo 不需要知道房间何时创建或销毁，一切由信令服务通过 Token 生命周期控制。

## 7. 调试与监控

- **日志**：Provider 内部可以使用 `log_info`, `log_warn`, `log_error` 宏输出调试信息。设置 `CTURBO_DEBUG=1` 环境变量可获取更详细日志。
- **指标**：房间相关指标如 `turbo_room_members_current` 和 `turbo_room_broadcast_dropped_total` 可用于监控 Provider 效果。
- **测试**：可以使用 `turnutils_uclient` 模拟客户端，并附带自定义 USERNAME 进行测试。

## 8. 附录：内置 Provider 源码位置

| Provider | 源文件 |
| :--- | :--- |
| `static` | `src/turbo/room/provider_static.c` |
| `token_hmac` | `src/turbo/room/provider_token_hmac.c` |
| `lua_script` | `src/turbo/room/provider_lua.c` |

建议参考这些实现来开发自定义 Provider。
