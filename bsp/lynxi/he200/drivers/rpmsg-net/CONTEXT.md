# RPMsg-Net Context

## 1. 组件位置

- 代码目录：`bsp/lynxi/he200/drivers/rpmsg-net`
- 主要源码：`rpmsg_net.c`
- 协议定义：`rpmsg_net_proto.h`
- 协议说明：`Protocol.md`

## 2. 当前实现目标

当前版本围绕以下目标进行了重构：
- 使用 RPMsg queue 代替在设备收包路径中的被动轮询；
- 使用 RPMsg-Lite nocopy API 降低共享内存收发开销；
- 将状态尽量集中在统一的数据结构中；
- 让 RT-Thread `eth_device` 与 RPMsg 收发线程职责清晰分离；
- 降低丢包风险，尤其是高频 RX 场景。

## 3. 关键数据结构

### 3.1 `struct rpmsg_net_device`

保存单个网络设备实例的运行时状态，包括：
- `parent`：RT-Thread `eth_device`
- `rpmsg_inst`：RPMsg-Lite 实例
- `rpmsg_ept`：endpoint
- `rpmsg_queue`：blocking RX queue
- `rx_thread`：独立接收线程
- `rx_mq`：向 `eth_rx()` 投递 `pbuf *` 的消息队列
- `remote_ready` / `local_link_up` / `remote_link_up`
- `hello_sent` / `link_state_sent` / `pending_link_down`
- `remote_addr` / `mtu`
- `local_mac` / `remote_mac`

### 3.2 `struct rpmsg_net_context`

保存模块级上下文：
- `device`
- `rpmsg_inst`
- `link_wait_thread`
- `mac_reg_base`
- `initialized`

## 4. 线程模型

### 4.1 link wait 线程

线程入口：`rpmsg_link_wait_thread_entry()`

职责：
- 阻塞等待 `rpmsg_lite_wait_for_link_up()`；
- link up 后创建设备与 endpoint；
- 等待 `netif` 可用；
- 发送 HELLO 和 LINK UP；
- 标记 `initialized = RT_TRUE`。

### 4.2 RX 线程

线程入口：`rpmsg_net_rx_thread_entry()`

职责：
- 阻塞调用 `rpmsg_queue_recv_nocopy(..., RL_BLOCK)`；
- 从 queue 取到共享内存数据；
- 解析控制报文或数据报文；
- 数据报文转成 `pbuf` 后投递到 `rx_mq`；
- 最后通过 `rpmsg_queue_nocopy_free()` 归还 RX buffer。

### 4.3 网络栈接收路径

`rpmsg_net_rx()` 本身不再碰 RPMsg queue。

它只做一件事：
- 从 `rx_mq` 非阻塞取出已经准备好的 `pbuf *`；
- 返回给 RT-Thread/lwIP。

这意味着：
- RPMsg 接收与网络栈取包完全解耦；
- 避免在 `eth_rx()` 路径里进行 queue 轮询和协议解析。

## 5. 收发数据流

### 5.1 TX 数据流

lwIP `pbuf` -> `rpmsg_net_tx()` -> 统一 `alloc/fill/send` helper -> RPMsg TX buffer -> `rpmsg_lite_send_nocopy()`

说明：
- 发送侧不再先构造临时堆消息再复制；
- 控制消息和 DATA 消息均直接写入 RPMsg TX buffer；
- 由于 `pbuf` 链不是连续内存，DATA 发送时仍需拼接复制到连续 TX buffer。

### 5.2 RX 数据流

RPMsg callback -> `rpmsg_queue_rx_cb()` -> RX thread -> `rpmsg_net_dispatch_rx()` -> `pbuf` -> `rx_mq` -> `eth_device_ready()` -> `rpmsg_net_rx()`

说明：
- callback 只负责入队；
- 真正的解析、复制和投递在 RX 线程执行；
- lwIP 通过标准 `eth_device` 机制取包。

## 6. 控制面约束

### 6.1 链路 ready 条件

`rpmsg_net_is_ready()` 要求：
- `local_link_up == RT_TRUE`
- `remote_link_up == RT_TRUE`
- `remote_ready == RT_TRUE`

满足后才允许正常发数据。

### 6.2 载波同步

`rpmsg_net_sync_carrier()` 负责同步 lwIP 侧 link 状态：
- ready 时 `netif_set_link_up()`
- 否则 `netif_set_link_down()`

### 6.3 HELLO/LINK 作用

- `HELLO`：同步 MAC、MTU，并确认协议面 ready；
- `LINK`：同步链路 up/down 状态；
- 两者都属于控制面消息，由统一发送 helper 生成并发送。

## 7. 清理与生命周期

### 7.1 资源释放顺序

清理时重点资源包括：
- RX 线程
- endpoint
- RPMsg queue
- `rx_mq`
- `eth_device`
- 全局 context

### 7.2 关键辅助函数

- `rpmsg_net_stop_rx_path()`：停止 RX 线程并销毁 endpoint/queue
- `rpmsg_net_free_device()`：释放设备对象
- `rpmsg_net_cleanup_unregistered_device()`：处理注册失败场景
- `rpmsg_net_device_unregister()`：统一设备注销路径
- `rpmsg_net_reset_context()`：清空模块上下文

## 8. 当前实现边界

### 8.1 已完成

- 已切换到 `rpmsg_queue_create()`
- 已使用 `rpmsg_queue_recv_nocopy()`
- 已使用 `rpmsg_lite_alloc_tx_buffer()` + `rpmsg_lite_send_nocopy()`
- 已将 HELLO/LINK/DATA 三条发送路径统一到同一个 helper 模式
- 已将大部分全局状态收敛到结构体中
- **`platform_get_custom_shmem_config()`** 与 Linux bridge 对齐：**4080 / 128 / 16384 / 4096**（修改须双端同步）
- **`rpmsg_net_ip_mtu_from_buffer_payload()`** 已按 **RL_BUFFER_PAYLOAD** 语义计算（不重复减 `rpmsg_std_hdr`）；**`rpmsg_net_apply_shmem_mtu_cap()`** 在注册与 HELLO 路径限制 **`rdev->mtu` / `netif->mtu`**

### 8.2 当前仍存在的复制

虽然 RPMsg 层已经采用 nocopy：
- RX 侧仍需把共享内存内容复制到 lwIP `pbuf`
- TX 侧 DATA 仍需把 `pbuf` 链拼接复制到连续 TX buffer

这是由当前 RT-Thread `eth_device` / lwIP 接口形态决定的。

### 8.3 后续可继续优化方向

- 如底层支持，可继续评估更好的 TX 失败回收语义；
- 如网络栈允许，可评估减少 RX 到 `pbuf` 的复制；
- 可继续压缩初始化/错误路径中的重复状态切换逻辑；
- 可补充真实硬件或 QEMU 验证说明；
- **吞吐与 MTU**：已通过 **加大 shmem 槽（4080/128/16384）**、**修正 IP MTU 与 `buffer_payload` 公式**、与 **host TX 路径优化** 一并缓解；回归与剩余项见 **`NEXT_STEPS.md`**。

## 9. 本次问题排查总结（host ping 设备不回包）

### 9.1 现象

- host 侧可发送 ARP/IPv4 到设备；
- 设备侧可见 `rpmsg_net_dispatch_rx()` 收到帧并调用 `eth_device_ready()`；
- 但设备不回 ARP/ICMP，`rpmsg_net_tx()` 未触发。

### 9.2 根因

根因在 `rx_mq` 出队判定逻辑错误：

- `rt_mq_send()` 返回 `rt_err_t`，成功值为 `RT_EOK`；
- **`rt_mq_recv()` 返回 `rt_ssize_t`，成功时返回接收字节数（`> 0`）**；
- 旧代码将 `rt_mq_recv(...) == RT_EOK` 作为成功条件，导致实际收到消息（如返回 `8`）却被误判为失败；
- 结果是 `rx_mq` 只入不出，lwIP 收不到有效 `pbuf`，因此不会产生回包。

### 9.3 已完成修复

- 将 `rpmsg_net_rx()` 中 `rt_mq_recv()` 成功判断改为 `> 0`；
- 将 `rpmsg_net_stop_rx_path()` 与失败清理分支中从 `rx_mq` 取包的判断也统一改为 `> 0`；
- 保留本次排查确认过的必要修复：
  - RX 分配失败/拷贝失败路径均释放 `pbuf`，避免泄漏；
  - `rpmsg_net_tx()` 失败时不再释放上层 `pbuf`（遵循 `eth_tx` 所有权）；
  - TX 目的地址优先使用接收路径学习到的 `remote_addr`；
  - `rpmsg_net_rx()` 的 `rt_mq_recv` 使用短超时（1 tick）降低空轮询竞态。

### 9.4 配置与联调注意事项

- 设备 IP 必须与 host 发包目标一致（本次联调使用 `1.1.1.2/24`）；
- 调试期临时日志已清理，默认回到低噪声级别；
- 若再次出现“只收不回”，优先检查：
  - `rt_mq_recv()` 返回值判定是否被改回错误写法；
  - `rpnet0` 的 IP/MAC 与 host 发送目标是否一致；
  - 控制面 endpoint 地址定义（`LOCAL/REMOTE`）是否与对端一致。

### 9.5 排查思路与步骤复盘（按实际执行顺序）

本次采用的是“先分层，再用最小日志逐层排除”的方法，避免一次性大改：

1. 先判断链路层是否可达  
   - 已知 `rpmsg-lite` 基础链路可用；
   - 设备能收到 host 发来的帧，说明 RPMsg 传输层不是首要问题。

2. 对齐 `virtio_net` 基线实现  
   - 对比 `virtio_net.c` 的收发路径，确认 `eth_device_ready -> eth_rx -> netif->input -> eth_tx` 闭环；
   - 检查 `rpmsg-net` 是否在该闭环前被额外门控（控制面 ready、地址路由等）。

3. 从“是否到达发送函数”切分问题  
   - 给 `rpmsg_net_tx()` 增加发送入口日志；
   - 现象是长期没有 TX 日志，说明问题发生在 TX 之前（RX 入栈路径）。

4. 验证帧内容是否合理  
   - 增加轻量包头日志（目的/源 MAC、ethertype、IPv4/ARP关键信息）；
   - 看到 ARP/IPv4 目标字段后，先修正了设备 IP 与 host 测试网段不一致的问题（`1.1.1.2/24`）。

5. 验证 lwIP 线程链路是否走通  
   - 在 `eth_device_ready`、`eth_rx_thread` 增加临时日志，确认通知确实送达且 RX 线程被唤醒；
   - 说明问题不在 mailbox 投递，而在 `eth_rx()` 取包结果。

6. 针对 `rx_mq` 做入队/出队计数  
   - 增加 `enqueue/dequeue/empty` 计数日志；
   - 观测到 `enqueue` 持续增长而 `dequeue=0`，且 `mq_recv` 返回值为 `8`（非 0）。

7. 锁定根因并修复  
   - 查 `rt_mq_recv()` 语义：成功返回“接收字节数”，不是 `RT_EOK`；
   - 将所有 `rt_mq_recv(...) == RT_EOK` 改为 `> 0`；
   - 修复后闭环恢复，host ping 设备可通。

8. 清理排障影响  
   - 去除高频 `rt_kprintf` 和信息级临时日志，保留必要的 `LOG_D` 轻量包头日志用于后续联调；
   - 回退 `ethernetif.c` 临时插桩，避免长期污染通用 lwIP 端口层。

## 10. SMP 绑核与中断亲和性（he200）

在 **RT-Thread SMP** 下，rpmsg-net 与 lwIP 工作线程若与 rpmsg 软中断在不同核上迁移，易出现竞态与丢包。当前通过 **menuconfig / `rtconfig.h`** 可配置：

| 宏 / 配置项 | 作用 |
|-------------|------|
| `BSP_RPMSG_NET_BIND_CPU0` | 开启后：将 `tcpip`、`erx`、`etx`、`rpnet_rx`、`rpmsg_wait` 等绑到 `BSP_RPMSG_NET_CPU`（名称历史遗留，不限于 CPU0）。 |
| `BSP_RPMSG_NET_CPU` | 上述线程的目标 CPU 索引（会做 `0 .. RT_CPUS_NR-1` 夹紧）。 |
| `BSP_RPMSG_NET_IRQ_FOLLOW_WORKER_CPU` | 为 y 时，rpmsg-lite **DW 定时器** IRQ 的 GIC 亲和性与 `BSP_RPMSG_NET_CPU` 一致。 |
| `BSP_RPMSG_NET_IRQ_CPU` | 在关闭 “IRQ 跟随 worker” 后可见，单独指定定时器 IRQ 所在 CPU。 |

**中断安装与绑核位置**（与 PCIe MSI-X 无直接关系）：

- `packages/rpmsg-lite-latest/.../rpmsg_platform.c` 中 `rpmsg_platform_timer_request_irq()`：在 `rt_hw_interrupt_install` / `umask` 之后调用 `rt_hw_interrupt_set_affinity()`。
- 定时器下标与 IRQ 号线性关系：`TIMER_IRQ_START + RPMSG_PLATFORM_TIMER_TVQ_IDX(2)` / `+ RVQ_IDX(3)`，需与 `lynxi.h` 中 `TIMER_IRQ_START` 一致。
- GICv3 单 SPI 绑核 API：`libcpu/aarch64` 中 `rt_hw_interrupt_set_affinity(vector, cpu_index)`（详见 `interrupt.h`）。

**RPSH（`applications/rpsh/rpsh_server.c`）**：`sh_srv` 线程在开启 `BSP_RPMSG_NET_BIND_CPU0` 时与 `BSP_RPMSG_NET_CPU` 对齐，避免与 rpmsg-net 栈跨核迁移。

## 11. RPSH 与 lwIP 接收分队列

RPSH 使用 EtherType **0x88B6**。若与 lwIP 共用同一 `rx_mq`，`sh_srv` 与 `erx` 都会在 `eth_rx` 路径上抢同一邮箱，曾出现 **非 shell 帧被 shell 路径误处理/释放**，导致 **ICMP/ARP 丢失、host ping 不通**。

当前做法：

- **`rx_mq`**：仅投递普通以太网帧给 lwIP（`rpnet_rx_mq`）。
- **`shell_mq`**：仅投递 RPSH 帧给 `sh_srv`（`rpmsg_net_shell_rx_try()` 等路径）。

实现集中在 `rpmsg_net.c` 与 `rpsh_server.c`，协议仍见 `Protocol.md` / `rpmsg_net_proto.h`。

## 12. 相关文件索引（扩展）

| 路径 | 说明 |
|------|------|
| `bsp/lynxi/he200/drivers/Kconfig` | `BSP_USING_RPMSG_NET`、绑核与 IRQ CPU 配置。 |
| `bsp/lynxi/he200/packages/rpmsg-lite-latest/.../rpmsg_platform.c` | RPMsg 平台：定时器 IRQ、`platform_get_custom_shmem_config()`（`buffer_payload_size` / `buffer_count` / `vring_size` 等，须与 **Linux host** 一致）。 |
| `libcpu/aarch64/common/interrupt.c` | `rt_hw_interrupt_set_affinity` 实现入口（GICv3）。 |
| Linux：`tools/drivers_test/rpmsg-net/rpmsg-lite/VRING.md` | vring 与 buffer 池 **详细内存部署**（§1.5）；设备侧无副本时可引用该路径。 |
| 同目录 **`NEXT_STEPS.md`** | 后续优先级与归档对照。 |

## 13. 与 Linux host 的 shmem 约定与文档

| 项 | 设备（RT-Thread） | Host（drivers_test） |
|----|-------------------|----------------------|
| 配置入口 | `packages/rpmsg-lite-latest/.../rpmsg_platform.c` 中 `RPMSG_PLATFORM_*`、`platform_get_custom_shmem_config()` | `rpmsg_net_bridge.c` 中 `RPMSG_NET_*` |
| 当前典型值 | `buffer_payload_size=4080`，`buffer_count=128`，`vring_size=16384`，`vring_align=4096` | 同上，须逐字段一致 |
| 布局说明 | — | 同仓库 **`rpmsg-lite/VRING.md`**（§1.5 详细偏移） |

**注意**：修改任一侧后必须 **同步另一侧** 并 **双端重编**，否则 `rpmsg_lite` 初始化或运行期行为异常。
