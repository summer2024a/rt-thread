# RPSH (Remote Protocol Shell) 模块说明

## 1. 功能概述

RPSH 是一个基于以太网的远程 Shell 服务模块，允许客户端通过自定义以太网帧协议远程连接到 RT-Thread 系统并执行命令。该模块工作在数据链路层，使用原始以太网帧进行通信，适用于嵌入式设备的对等网络连接场景。

## 2. 技术规格

| 参数 | 值 |
|------|-----|
| 以太网类型 (EtherType) | 0x88B6 |
| 最大会话数 | 4 |
| 单帧最大载荷 | 128 字节 |
| 接收缓冲区 | 4096 字节 |
| 发送缓冲区 | 4096 字节 |
| 会话超时时间 | 20 秒 |
| 心跳间隔 | 4 秒 |

## 3. 通信协议

### 3.1 帧格式

```
+----------------+----------------+----------------+----------------+
|   目的MAC(6)   |    源MAC(6)    |   EtherType(2) |    数据        |
+----------------+----------------+----------------+----------------+
      ↑                ↑                ↑
   目的地址          源地址         0x88B6 (RPSH)
```

### 3.2 帧类型定义

| 类型值 | 名称 | 说明 |
|--------|------|------|
| 1 | LOGIN | 登录请求 |
| 2 | LOGOUT | 登出请求 |
| 3 | CMD | 命令请求 |
| 4 | RESP | 命令响应 |
| 5 | HEARTBEAT | 心跳保活 |
| 6 | ACK | 确认帧 |
| 7 | PTY | 虚拟终端数据 (预留) |
| 8 | FILE | 文件传输 (预留) |
| 9 | RESIZE | 窗口大小变化 (预留) |

### 3.3 帧标志位（flags）

| 位 | 名称 | 说明 |
|----|------|------|
| 0x01 | PACKET_FLAG_MORE | 分片传输，后续还有 RESP |
| 0x00 | PACKET_FLAG_LAST | 最后一帧（与 MORE 互斥） |
| 0x80 | PACKET_FLAG_ASYNC | 异步日志 RESP（非本行命令输出） |

### 3.4 Shell 帧结构

```c
typedef struct shell_frame {
    uint8_t     type;        // 帧类型
    uint8_t     flags;       // 标志 (PACKET_FLAG_MORE / PACKET_FLAG_LAST)
    uint8_t     session_id;  // 会话ID
    uint8_t     reserved;    // 保留

    uint16_t    seq;         // 序列号
    uint16_t    data_len;    // 当前帧数据长度
    uint16_t    total_len;   // 总数据长度

    uint16_t    crc;         // CRC16 校验
    uint8_t     data[128];   // 载荷数据
} __attribute__((packed)) shell_frame_t;
```

## 4. 会话管理

### 4.1 会话结构

每个会话包含以下信息：
- 会话 ID (1-4)
- 客户端 MAC 地址
- 认证级别 (0: guest, 1: admin)
- 最后心跳时间
- 接收缓冲区
- 统计信息 (包计数、丢包计数、字节数)

### 4.2 认证凭证

| 用户名 | 密码 | 权限级别 |
|--------|------|----------|
| admin | admin123 | 管理员 |
| user | user123 | 普通用户 |

## 5. MSH 命令

模块提供以下 Finsh/MSH 命令：

### 5.1 sid 命令

会话管理命令，用于查看和踢除会话。

```bash
# 列出所有活动会话
sid list

# 踢除指定会话
sid kick <session_id>
```

输出示例：
```
sid list
sid:1 auth:1 mac:02:12:34:56:78:9a
sid:2 auth:0 mac:02:11:22:33:44:55
```

### 5.2 stat 命令

显示所有会话的通信统计信息。

```bash
stat
```

输出示例：
```
stat
sid:1 pkt:100 drop:0 bytes:5120
sid:2 pkt:50 drop:2 bytes:2048
```

## 6. 工作流程

### 6.1 登录流程

1. 客户端发送 LOGIN 帧，包含用户名和密码
2. 服务器验证凭证
3. 验证成功后分配会话 ID，发送 ACK 确认
4. 会话建立，后续可以发送命令

### 6.2 命令执行流程

1. 客户端发送 CMD 帧（可分片）
2. 服务器接收完整命令
3. 执行命令并将输出通过 RESP 帧返回
4. 支持分片传输大数据

### 6.3 心跳机制

- 客户端定期发送 HEARTBEAT 帧
- 服务器更新会话的最后心跳时间
- 超过 20 秒无心跳则自动清除会话

## 7. API 接口

### 7.1 初始化函数

```c
int rpmsg_shell_server_init(void);
```

初始化 RPSH 服务器（内部通过 `rpmsg_net_eth_device_get()` 绑定以太网设备），并通过 `INIT_APP_EXPORT` 随应用启动。

**返回值：**
- 0: 成功
- 负值: 失败

## 8. 依赖项

- RT-Thread 内核
- lwIP 协议栈
- netdev 网络设备接口
- finsh Shell (用于命令导出)

## 9. 使用指导

### 9.1 设备侧（RT-Thread / BSP）

1. 在 `SConscript` 中引入 rpsh 组件并完成编译（与现有 he200 BSP 集成方式一致）。
2. 固件中 `rpmsg_shell_server_init()` 由 `INIT_APP_EXPORT` 自动调用；无需在业务代码里手动调用，除非关闭自动导出并自行在合适时机初始化。
3. 确保设备侧 **rpmsg-net** 以太网已就绪，且能收到 host 发往 `rpsh_proto.h` 中默认设备 MAC 的帧（或通过 `rpmsg-net` 日志确认对端 MAC）。
4. 设备侧命令与交互：
   - 普通命令：与 MSH 一致，由 `msh_exec` 执行。
   - **Tab**：收到以 `\t` 结尾的 CMD 时走 `msh_auto_complete`，输出中带有 `finsh_get_prompt()` 与补全后的前缀；RESP 末尾附带仅供 client 解析的同步尾标（见联调记录）。
   - **`pull`**：仅用于把 backlog 中的日志刷到输出，不执行 `msh_exec`。

### 9.2 Host 侧（rpsh_client）

#### 依赖与权限

- 使用 **AF_PACKET 原始套接字**，Linux 上通常需要 **root** 或 **`CAP_NET_RAW`**。
- 需指定本机与设备互通的 **网卡名**（如 `eth0`）。

#### 编译

```bash
cd bsp/lynxi/he200/applications/rpsh/client
make
```

生成可执行文件 `rpsh_client`。

#### 命令行

```text
rpsh_client [--no-async-log] <ifname> [device_mac]
```

| 参数 | 说明 |
|------|------|
| `--no-async-log` | 关闭设备异步推送的 ulog 日志（仅影响 `PACKET_FLAG_ASYNC` 帧；普通命令 RESP 仍显示） |
| `ifname` | 本机网卡名，用于抓发 EtherType `0x88B6` 的帧 |
| `device_mac` | 可选，设备 MAC，默认 `02:12:20:11:34:00`（与常见 rpmsg-net 日志一致） |

示例：

```bash
sudo ./rpsh_client eth0
sudo ./rpsh_client eth0 02:12:20:11:34:00
sudo ./rpsh_client --no-async-log eth0
```

#### 登录

启动后会提示输入密码；支持 `admin123` / `user123`，或简写 `admin` / `user`；也支持 `user/密码` 形式（只取 `/` 后一段）。

#### 会话内操作

| 键 | 行为 |
|----|------|
| **Enter** | 提交当前行，发送 CMD；本地保留 **命令历史**（最多 32 条，连续重复不重复入栈） |
| **Tab** | 将「当前行 + `\t`」发给设备，由设备做 **MSH 补全**；输出中可见设备 FinSH 提示符与补全结果，本地行尾与设备同步 |
| **↑ / ↓** | 浏览本地历史命令（支持常见 ANSI CSI / VT100 序列） |
| **Ctrl+C** | 退出，并发送 LOGOUT（若因心跳超时退出则不再发 LOGOUT） |

#### 异步日志与心跳

- **空闲时** 设备可能推送 **异步日志**（`PACKET_FLAG_ASYNC`）；client 会插入输出并 **重绘 `rpsh> ` 与当前输入行**。
- 若长时间 **收不到任何有效 server 帧**（CRC+会话 匹配，含 ACK/RESP），超过 **HB_INTERVAL_SEC×4+2** 秒（默认 `HB_INTERVAL_SEC=4` 时约 **18 秒**）则判定 **server 失联**，打印提示并以 **非零退出码** 退出。
- 需要安静界面时加 **`--no-async-log`**。

## 10. 文件列表

| 文件 | 说明 |
|------|------|
| rpsh_server.c | 设备端主实现 |
| rpsh_proto.h | 协议定义（与 client 侧一致） |
| client/rpsh_client.c | Linux host 客户端 |
| client/Makefile | 客户端编译 |
| SConscript | BSP 构建配置 |
| rpsh.md | 本文档 |

## 11. 今日联调排查记录（client + server）

本节记录本次联调中，围绕“host 远程使用 msh 的交互体验”所做的排查路径、机理分析和最终方案，便于后续继续维护。

### 11.1 目标与症状

- 目标：
  - host 侧要有完整命令回显；
  - 不输入命令时，也能看到设备侧系统日志（ulog/rt_kprintf）；
  - 心跳失联后 client 自动退出，避免假在线。
- 主要症状：
  - 早期仅在执行命令时才能“顺带”看到部分日志；
  - client 的 Tab 与方向键交互不足（不支持补全/历史）；
  - 设备侧串口输入在某些路径下失效（见“已知问题项”）。

### 11.2 机理结论（核心）

1. **日志链路需要区分“执行期”与“空闲期”**
   - 执行命令期间，日志应并入当前 RESP，保证命令结果完整。
   - 非执行期间，日志需要先进入 backlog，再异步推给活跃会话。

2. **仅靠“下一次命令拉取日志”无法满足实时观测**
   - 因此协议层新增了 `PACKET_FLAG_ASYNC`，用于标识异步日志 RESP。

3. **client 必须把异步输出与当前输入行重绘解耦**
   - 异步日志到来时先输出日志，再恢复 `rpsh> + 当前输入`，避免打断编辑体验。

4. **心跳判活不能只“发不收”**
   - client 发送 HEARTBEAT 的同时，必须把收到的 ACK 计入“server 存活时间”。

### 11.3 本次关键改动汇总

#### 协议

- `rpsh_proto.h`（server/client）新增：
  - `PACKET_FLAG_ASYNC (0x80)`：异步日志帧标记。

#### server（`rpsh_server.c`）

- 新增 ulog backend 分流策略：
  - `g_exec_in_progress = true`：日志写入当前命令输出缓冲；
  - 否则写入 backlog（环形保留，避免丢失全部上下文）。
- 主循环中加入 backlog 异步推送：
  - 当无命令执行且 backlog 非空，向活动 session 下发
    `FRAME_TYPE_RESP | PACKET_FLAG_ASYNC`。
- `pull` 语义调整：
  - 仅用于 flush backlog，不再执行 `msh_exec`。
- Tab 补全走设备侧：
  - client 发送“当前行 + `\t`”；
  - server 调用 `msh_auto_complete`（及可选 `msh_opt_auto_complete`），并输出
    `finsh_get_prompt() + 补全后前缀`；
  - 为了让 client 同步本地编辑行，在 RESP 末尾附加 `RPSH_LINE` 尾标信息，client 解析后重绘本地输入。

#### client（`rpsh_client.c`）

- 异步日志显示：
  - 识别 `PACKET_FLAG_ASYNC`，立即打印并重绘输入行。
- 新增开关：
  - `--no-async-log` 可关闭异步日志刷屏（不影响普通命令响应）。
- 心跳失联自动退出：
  - 对收到的有效帧（CRC + session 匹配）刷新 `last_server_rx`；
  - 超过阈值（`HB_INTERVAL_SEC * 4 + 2`）未收到 server 响应则退出并返回非零码。
- 本地交互增强：
  - 支持上/下箭头历史（含常见 ANSI/VT100 序列及带参数 CSI）；
  - Tab 不再本地提示“unsupported”，而是发送补全请求给设备侧；
  - 响应结束时解析 server 尾标，更新本地 `line_buf` 并重绘。

### 11.4 联调后行为（当前基线）

- 命令执行：有命令回显，有结果输出，提示符连续。
- 空闲状态：可收到设备端异步日志（可按需关闭）。
- 键盘交互：支持上/下历史与设备侧 Tab 补全。
- 连接保活：server 无心跳响应时，client 自动退出。

## 12. 已知问题项（需跟进）

### 12.1 设备侧串口无法输入（重要）

**现象：**
- 在部分路径下，设备本地串口 shell 输入失效或异常（尤其与远程执行/console 切换相关）。

**已确认机理：**
- `rt_console_set_device()` 在某些配置（特别是 `RT_USING_POSIX_STDIO`）下，会触发旧 console 设备 close/重绑定；
- finsh/getchar 与 POSIX stdio 的输入路径可能被扰动，导致串口输入不可用或不稳定。

**当前处理策略（本轮）：**
- 以 host 远程可用性优先，保留远程捕获/日志链路能力；
- 减少不必要的激进 console 来回切换，降低输入路径抖动风险。

**影响范围：**
- 对“远程调试体验”影响可控（host 侧可工作）；
- 对“设备本地串口持续可用性”仍有风险，属于遗留问题。

**后续建议：**
1. 将“命令输出捕获”和“系统默认 console”彻底解耦，避免频繁切全局 console；
2. 明确 finsh 输入设备与 POSIX stdio console 的职责边界，避免互相覆盖；
3. 设计可回归的串口输入测试用例（登录、执行、退出、并发日志场景）并固化。