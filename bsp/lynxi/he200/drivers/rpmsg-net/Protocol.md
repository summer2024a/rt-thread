# RPMsg-Net Protocol

本文档描述 `bsp/lynxi/he200/drivers/rpmsg-net` 当前在 RT-Thread 下的协议格式、状态协商和收发路径。

## 1. 目标

`rpmsg-net` 通过 RPMsg-Lite 在两侧处理器之间传输以太网帧，并向 RT-Thread 网络栈暴露一个 `eth_device`。

当前实现目标：
- 使用 `rpmsg_queue_create()` 接收消息；
- 使用 `rpmsg_queue_recv_nocopy()` 进行阻塞式零拷贝接收；
- 使用 `rpmsg_lite_alloc_tx_buffer()` + `rpmsg_lite_send_nocopy()` 发送；
- 将控制面消息和数据面消息统一封装在同一协议头下；
- 在 RT-Thread 中通过 `eth_device_ready()` + `eth_rx()` 对接 lwIP。

## 2. 协议常量

定义位于 `rpmsg_net_proto.h`。

| 名称 | 值 | 说明 |
| --- | --- | --- |
| `RPMSG_NET_LINK_ID` | `0U` | RPMsg link id |
| `RPMSG_NET_LOCAL_EPT_ADDR` | `0x100U` | 本地 endpoint 地址 |
| `RPMSG_NET_REMOTE_EPT_ADDR` | `0x101U` | 对端 endpoint 地址 |
| `RPMSG_NET_MTU` | `1500U` | 驱动允许的最大 MTU |
| `RPMSG_NET_PROTO_VER` | `0x0001U` | 当前协议版本 |
| `RPMSG_NET_PKT_TYPE_DATA` | `0x01U` | 数据报文 |
| `RPMSG_NET_PKT_TYPE_HELLO` | `0x02U` | 能力协商报文 |
| `RPMSG_NET_PKT_TYPE_LINK` | `0x03U` | 链路状态报文 |

## 3. 报文格式

### 3.1 通用消息头

```c
struct rpmsg_net_msg_hdr
{
    rt_uint16_t type;
    rt_uint16_t version;
    rt_uint32_t length;
} __attribute__((packed));
```

字段说明：
- `type`：消息类型；
- `version`：协议版本；
- `length`：整个协议消息长度，包含头部；
- 所有字段当前按小端处理，代码中的转换函数当前为直通实现。

接收侧会校验：
- `len >= sizeof(struct rpmsg_net_msg_hdr)`；
- `version == RPMSG_NET_PROTO_VER`；
- `length >= sizeof(struct rpmsg_net_msg_hdr)`；
- `length <= 实际接收长度`。

### 3.2 HELLO 消息

```c
struct rpmsg_net_hello_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint8_t mac[ETH_ALEN];
    rt_uint16_t mtu;
    rt_uint16_t reserved;
} __attribute__((packed));
```

用途：
- 上报本端 MAC；
- 通知本端 MTU；
- 标记本端协议面准备完成。

行为：
- 本端 `open()` 后如果尚未发送过 HELLO，则发送；
- 收到对端 HELLO 后，保存对端 MAC；
- 使用更小的 MTU 作为运行 MTU；
- 设置 `remote_ready = RT_TRUE`。

### 3.3 LINK 消息

```c
struct rpmsg_net_link_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint32_t link_up;
} __attribute__((packed));
```

用途：
- 通知对端本地链路状态。

字段：
- `link_up = 1` 表示链路可用；
- `link_up = 0` 表示链路不可用。

行为：
- 本端 `open()` 后发送一次 `link_up = 1`；
- 本端关闭或注销时尝试发送一次 `link_up = 0`；
- 收到后更新 `remote_link_up`。

### 3.4 DATA 消息

``c
struct rpmsg_net_data_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint8_t payload[0];
} __attribute__((packed));
```

用途：
- 承载完整以太网帧。

`payload` 内容：
- 直接存放 lwIP/`pbuf` 中的原始以太网报文；
- 不额外增加二层私有头。

长度约束：
- `payload` 长度必须大于 0；
- 当前驱动限制 `MAX_PKT_SIZE = 1536`；
- 接收侧如果 `msg_len <= sizeof(struct rpmsg_net_data_msg)`，则认为无效。

## 4. RT-Thread 下的状态模型

驱动状态由以下几个标志共同决定：
- `local_link_up`
- `remote_link_up`
- `remote_ready`
- `hello_sent`
- `link_state_sent`
- `pending_link_down`

其中网络载波状态由 `rpmsg_net_sync_carrier()` 统一处理：

```text
carrier_on = local_link_up && remote_link_up && remote_ready
```

即只有本地链路已打开、远端声明链路已打开、并且远端已完成 HELLO 协商时，才会对 lwIP 调用 `netif_set_link_up()`。

## 5. 发送流程

### 5.1 统一发送 helper

当前代码将 HELLO、LINK、DATA 三条发送路径统一到了一个内部 helper：
- 申请 TX buffer；
- 检查 buffer 大小；
- 调用 fill 回调填充协议内容；
- 使用 `rpmsg_lite_send_nocopy()` 发送。

统一后特点：
- 发送接口风格一致；
- 控制消息和数据消息都直接写入 RPMsg TX buffer；
- 避免额外临时堆缓冲。

### 5.2 HELLO 发送

触发时机：
- `rpmsg_net_open()` 中，如果 `hello_sent == RT_FALSE`。

填充内容：
- `type = RPMSG_NET_PKT_TYPE_HELLO`
- `version = RPMSG_NET_PROTO_VER`
- `length = sizeof(struct rpmsg_net_hello_msg)`
- `mac = local_mac`
- `mtu = rdev->mtu`

发送成功后：
- `hello_sent = RT_TRUE`

### 5.3 LINK 发送

触发时机：
- `rpmsg_net_open()` 中发送 `link_up = 1`；
- `rpmsg_net_request_link_down()` 中尝试发送 `link_up = 0`。

发送成功后：
- `link_state_sent = link_up`
- `pending_link_down = RT_FALSE`

发送失败时：
- 如果是链路下线通知，则置 `pending_link_down = RT_TRUE`。

### 5.4 DATA 发送

发送入口：
- `rpmsg_net_tx(rt_device_t dev, struct pbuf *p)`

检查顺序：
1. 驱动是否 `initialized`；
2. `rpmsg_net_is_ready()` 是否为真；
3. `p->tot_len` 是否在 `(0, MAX_PKT_SIZE]`；
4. 是否能成功申请 RPMsg TX buffer。

填充方式：
- 填写 `struct rpmsg_net_data_msg` 头部；
- 遍历 `pbuf` 链，将每段 payload 复制进共享内存 TX buffer。

说明：
- 这里的“nocopy”针对 RPMsg 层成立，即不再经过额外临时消息缓冲；
- 但由于 lwIP 侧输入是 `pbuf` 链，仍需要将其内容拼接复制到连续 TX buffer 中。

## 6. 接收流程

### 6.1 RX 回调

endpoint 回调函数为：

```c
static int32_t rpmsg_net_recv_cb(void *payload, uint32_t len, uint32_t src, void *priv)
{
    return rpmsg_queue_rx_cb(payload, len, src, priv);
}
```

说明：
- 回调本身不做复杂解析；
- 仅将接收 buffer 挂入 RPMsg queue；
- 避免在回调上下文中执行耗时逻辑。

### 6.2 RX 线程

驱动独立创建 `rx_thread`，在线程中阻塞等待：

- `rpmsg_queue_recv_nocopy(..., RL_BLOCK)`
- 收到后调用 `rpmsg_net_dispatch_rx()`
- 处理完毕后调用 `rpmsg_queue_nocopy_free()` 释放 RX buffer

该模型避免了在 `eth_rx()` 路径里反复轮询 RPMsg queue。

### 6.3 控制消息处理

当 `type != DATA` 时，接收路径调用 `rpmsg_net_handle_control()`：
- `HELLO`：进入 `rpmsg_net_apply_remote_hello()`
- `LINK`：进入 `rpmsg_net_apply_remote_link()`

控制面处理后会调用 `rpmsg_net_sync_carrier()`，同步 lwIP 的 link 状态。

### 6.4 DATA 消息处理

收到 DATA 后：
1. 分配 `pbuf_alloc(PBUF_RAW, payload_len, PBUF_POOL)`；
2. 将共享内存中的 payload 拷贝进 `pbuf`；
3. 把 `pbuf *` 投递到 RT-Thread 消息队列 `rx_mq`；
4. 调用 `eth_device_ready()` 通知网络栈；
5. 在 `eth_rx()` 中从 `rx_mq` 取出 `pbuf` 返回给 lwIP。

说明：
- RPMsg RX buffer 本身是 nocopy 获取；
- 但为了适配 lwIP/RT-Thread `eth_device` 接口，最终仍需要复制到 `pbuf`。

## 7. 初始化与注销

### 7.1 初始化

入口：`rt_rpmsg_net_init()`

步骤：
1. 调用 `rpmsg_lite_remote_init()` 初始化 RPMsg-Lite 实例；
2. 创建 `link_wait_thread`；
3. 在线程中阻塞等待 `rpmsg_lite_wait_for_link_up()`；
4. link up 后创建 RPMsg queue 和 endpoint；
5. 注册 `eth_device`；
6. 等待 `netif` 就绪后发送 HELLO 和 LINK UP；
7. 设置 `initialized = RT_TRUE`。

### 7.2 注销

入口：`rpmsg_net_device_unregister()`

步骤：
1. 尝试向对端发送 LINK DOWN；
2. 关闭并注销 RT-Thread 网络设备；
3. 停止 RX 线程；
4. 销毁 endpoint 与 queue；
5. 释放消息队列和设备对象；
6. 清空全局上下文。

## 8. 错误处理约定

当前实现出现以下情况会丢弃数据或返回错误：
- 接收长度小于头部长度；
- 协议版本不匹配；
- `length` 字段非法；
- DATA 消息无 payload；
- `pbuf_alloc()` 失败；
- `rx_mq` 满；
- TX buffer 申请失败；
- `rpmsg_lite_send_nocopy()` 返回错误；
- 驱动未 ready 时发送数据。

## 9. 与旧文档的差异

当前 RT-Thread 实现与传统 Linux netdev 风格描述不同，主要差异如下：
- 不使用 `net_device` / `skb` 模型；
- 使用 RT-Thread `eth_device` 与 lwIP `pbuf`；
- 接收侧使用 `rpmsg_queue_create()` + 阻塞 RX 线程，而不是在网络设备收包路径中轮询队列；
- 发送侧统一为 `alloc/fill/send` 模式；
- 设备生命周期与 RT-Thread 初始化线程模型绑定。

```
#define RPMSG_NET_LINK_ID          0U

// 端点地址
#define RPMSG_NET_LOCAL_EPT_ADDR   0x100U  // 本地端点
#define RPMSG_NET_REMOTE_EPT_ADDR  0x101U  // 远端端点

// 网络接口
#define RPMSG_NET_IFNAME           "rpn%d"
#define RPMSG_NET_MTU              1500U

// 消息类型
#define RPMSG_NET_PKT_TYPE_DATA    0x01U   // 数据包
#define RPMSG_NET_PKT_TYPE_HELLO   0x02U   // 握手/能力协商
#define RPMSG_NET_PKT_TYPE_LINK    0x03U   // 链路状态通知

// 协议版本
#define RPMSG_NET_PROTO_VER        0x0001U
```

## 通信时序

### 完整链路建立过程

```
主设备 (Master)                          从设备 (Remote)
       |                                       |
       | 1. rpmsg_lite_master_init             |
       |-------------------------------------->|
       |                                       | 初始化共享内存
       |                                       | 创建 Virtqueue 对
       |                                       | 注册中断回调
       |                                       |
       | 2. 等待链路建立 (1000ms timeout)      |
       |<--------------------------------------|
       |                                       |
       | 3. HELLO (mtu=1500, mac=AA:BB:CC)     |
       |-------------------------------------->|
       |                                       | 保存 MAC/MTU
       |                                       | remote_ready=true
       |                                       |
       | 4. HELLO (mtu=1500, mac=DD:EE:FF)     |
       |<--------------------------------------|
       | 保存 MAC/MTU                          |
       | remote_ready=true                     |
       |                                       |
       | 5. LINK (link_up=1)                   |
       |-------------------------------------->|
       |                                       | local_link_up=true
       |                                       |
       | 6. LINK (link_up=1)                   |
       |<--------------------------------------|
       | remote_link_up=true                   |
       | netif_carrier_on()                    | netif_carrier_on()
       |                                       |
       |========== 数据传输阶段 ================|
       |                                       |
       | 7. DATA (以太网帧 #1)                 |
       |-------------------------------------->|
       |                                       | 提取 payload
       |                                       | netif_rx() -> IP 栈
       |                                       |
       | 8. DATA (以太网帧 #2)                 |
       |<--------------------------------------|
       | 提取 payload                          |
       | netif_rx() -> IP 栈                   |
       |                                       |
       | 9. LINK (link_up=0) [可选]            |
       |-------------------------------------->|
       |                                       | netif_carrier_off()
       |                                       |
```

### 关键时序说明

1. **初始化阶段** (步骤 1-2):
   - Master 端调用 `rpmsg_lite_master_init()` 初始化共享内存
   - Remote 端需提前完成共享内存和 Virtqueue 初始化
   - 等待对端链路就绪（超时 1000ms）

2. **握手阶段** (步骤 3-4):
   - 先完成初始化的一方主动发送 HELLO
   - HELLO 消息携带 MAC 地址和 MTU 信息
   - 收到 HELLO 后设置 `remote_ready=true`

3. **链路同步阶段** (步骤 5-6):
   - 双方通过 LINK 消息交换链路状态
   - 当 `local_link_up && remote_link_up && remote_ready` 时开启载波
   - 网络接口进入可收发状态

4. **数据传输阶段** (步骤 7-8):
   - 以太网帧被封装到 DATA 消息的 payload 中
   - 通过 Virtqueue 进行异步传输
   - 接收方通过中断或轮询获取数据

5. **链路关闭阶段** (步骤 9):
   - 任意一方可主动发送 LINK (link_up=0) 通知断开
   - 接收方关闭网络载波

---

## 字节序处理

### 小端序规范

所有多字节字段在传输时均使用**小端序 (Little Endian)**:

``c
#include <linux/byteorder/generic.h>

// 类型定义
__le16  // 16 位小端整数
__le32  // 32 位小端整数

// 转换宏
cpu_to_le16(val)  // CPU 字节序 -> 小端
cpu_to_le32(val)  // CPU 字节序 -> 小端
le16_to_cpu(val)  // 小端 -> CPU 字节序
le32_to_cpu(val)  // 小端 -> CPU 字节序
```

### 示例代码

``c
// 发送前转换
msg->hdr.type = cpu_to_le16(RPMSG_NET_PKT_TYPE_DATA);
msg->hdr.version = cpu_to_le16(RPMSG_NET_PROTO_VER);
msg->hdr.length = cpu_to_le32(msg_len);
msg->mtu = cpu_to_le16(mtu_value);
msg->link_up = cpu_to_le32(link_state);

// 接收时转换
uint16_t type = le16_to_cpu(hdr->type);
uint16_t version = le16_to_cpu(hdr->version);
uint32_t length = le32_to_cpu(hdr->length);
uint16_t mtu = le16_to_cpu(hello_msg->mtu);
uint32_t link_up = le32_to_cpu(link_msg->link_up);
```

---

## 错误处理

### 接收端验证顺序

``c
// 1. 最小长度检查
if (len < sizeof(struct rpmsg_net_msg_hdr)) {
    priv->ndev->stats.rx_dropped++;
    free_buffer();
    return;
}

// 2. 协议版本匹配
if (le16_to_cpu(hdr->version) != RPMSG_NET_PROTO_VER) {
    priv->ndev->stats.rx_dropped++;
    free_buffer();
    return;
}

// 3. 声明长度合法性
if (hdr_len < min_len || hdr_len > rx_len) {
    priv->ndev->stats.rx_dropped++;
    free_buffer();
    return;
}

// 4. 消息类型识别
switch (le16_to_cpu(hdr->type)) {
case RPMSG_NET_PKT_TYPE_DATA:
    handle_data();
    break;
case RPMSG_NET_PKT_TYPE_HELLO:
    handle_hello();
    break;
case RPMSG_NET_PKT_TYPE_LINK:
    handle_link();
    break;
default:
    // 未知类型，丢弃
    priv->ndev->stats.rx_dropped++;
    break;
}
```

### 常见错误场景

| 错误场景 | 处理方式 | 统计计数器 |
|---------|---------|-----------|
| 接收长度 < 头部大小 | 直接丢弃 | `rx_dropped++` |
| 协议版本不匹配 | 直接丢弃 | `rx_dropped++` |
| 声明长度 > 实际长度 | 直接丢弃 | `rx_dropped++` |
| 声明长度 < 最小长度 | 直接丢弃 | `rx_dropped++` |
| DATA 消息无 payload | 直接丢弃 | `rx_dropped++` |
| SKB 分配失败 | 直接丢弃 | `rx_dropped++` |
| 未知消息类型 | 直接丢弃 | `rx_dropped++` |

### 发送端错误处理

``c
// 长度检查
if (skb->len > ndev->mtu + ETH_HLEN) {
    ndev->stats.tx_dropped++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

// 空包检查
if (skb->len == 0U) {
    ndev->stats.tx_dropped++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

// 链路未就绪
if (!rpmsg_net_is_ready(priv)) {
    ndev->stats.tx_dropped++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

// 内存分配失败
msg = kmalloc(msg_len, GFP_ATOMIC);
if (!msg) {
    ndev->stats.tx_dropped++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

// RPMsg 发送失败
ret = rpmsg_net_send_buffer(priv, msg, msg_len);
if (ret != RL_SUCCESS) {
    ndev->stats.tx_dropped++;
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}
```

---

## 附录：消息格式速查表

### HELLO 消息 (16 字节)

```
Offset  Size  Field         Value
------  ----  -----         -----
0x00    2     type          0x0002
0x02    2     version       0x0001
0x04    4     length        0x00000010
0x08    6     mac           XX:XX:XX:XX:XX:XX
0x0E    2     mtu           0x05DC (1500)
0x10    2     reserved      0x0000
```

### LINK 消息 (12 字节)

```
Offset  Size  Field         Value
------  ----  -----         -----
0x00    2     type          0x0003
0x02    2     version       0x0001
0x04    4     length        0x0000000C
0x08    4     link_up       0x00000001 (up) / 0x00000000 (down)
```

### DATA 消息 (8 字节 + N)

```
Offset  Size  Field         Value
------  ----  -----         -----
0x00    2     type          0x0001
0x02    2     version       0x0001
0x04    4     length        8 + payload_len
0x08    N     payload       Ethernet frame
```

## RPMsg-Net Remote 侧协议实现规范

### 问题背景

根据 Protocol.md 文档定义的通信时序，Remote 侧应该**被动响应**Master 侧的消息，而不是主动发送。

**正确的通信时序**：
1. Master 发送 HELLO → Remote 收到后回复 HELLO
2. Master 发送 LINK UP → Remote 收到后回复 LINK UP
3. Remote 不应该在初始化后主动先发 HELLO 和 LINK

### 原始问题

**错误实现的历史演变**：

1. **第一版错误**：Remote 侧只被动接收，不回复
   - ❌ 导致 Master 无法获取 Remote 的能力信息和链路状态

2. **第二版错误**：Remote 侧在初始化后立即主动发送 HELLO 和 LINK UP
   - ❌ 违反了协议定义的"被动响应"模式
   - ❌ 可能导致时序混乱或重复消息

3. **第三版错误**（已修复）：在 `rpmsg_link_wait_thread_entry()` 中不主动发送，但在 `rpmsg_net_open()` 中仍然主动发送
   - ❌ `eth_device_init` 可能触发 `open`，导致违反协议
   - ❌ 设备打开时就主动发送 HELLO 和 LINK，不符合被动响应要求

### 正确实现方案

#### 1. 初始化阶段 - 仅注册设备，完全等待

修改 `rpmsg_link_wait_thread_entry()` 函数：

```c
static void rpmsg_link_wait_thread_entry(void *parameter)
{
    // ... 等待 link up ...
    
    /* 启动设备 - 仅注册 endpoint 和 queue */
    ret = rpmsg_net_start(inst);
    if (ret == RT_EOK)
    {
        LOG_I("rpmsg-net device registered successfully");
        
        /* 等待网络接口 UP */
        wait_netif_up(&g_rpmsg_net_ctx.device->parent);

        /* 
         * ✨ 关键修改：Remote 侧不再主动发送 HELLO 和 LINK
         * 等待 Master 先发送 HELLO 和 LINK UP 消息
         * 收到后会在 rpmsg_net_apply_remote_hello() 和 
         * rpmsg_net_apply_remote_link() 中自动回复
         */
        LOG_I("Waiting for master's HELLO and LINK messages...");
        
        g_rpmsg_net_ctx.initialized = RT_TRUE;
        LOG_I("rpmsg-net device ready for RX/TX (waiting for master)");
    }
}
```

#### 2. 设备打开阶段 - 不主动发送

修改 `rpmsg_net_open()` 函数：

```c
static rt_err_t rpmsg_net_open(rt_device_t dev, rt_uint16_t oflag)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;

    if (rdev == RT_NULL)
    {
        return -RT_ERROR;
    }

    LOG_D("Opening rpmsg-net device");

    rpmsg_net_mark_local_state(rdev, RT_TRUE);
    rpmsg_net_sync_carrier(rdev);

    /* 
     * ✨ 关键修改：遵循协议时序，Remote 侧不主动发送 HELLO 和 LINK
     * 只有在收到 Master 的 HELLO 或 LINK 消息后，才会在回调函数中回复
     * 这里不再主动发送，避免违反"被动响应"模式
     */
    LOG_I("Device opened, waiting for master's messages...");

    return RT_EOK;
}
```

#### 3. HELLO 消息响应 - 被动触发

在 `rpmsg_net_apply_remote_hello()` 函数中实现自动回复：

``c
static void rpmsg_net_apply_remote_hello(struct rpmsg_net_device *rdev,
                                         const struct rpmsg_net_hello_msg *msg)
{
    // ... 保存 MAC 和 MTU ...
    rt_memcpy(rdev->remote_mac, msg->mac, sizeof(rdev->remote_mac));
    rdev->remote_ready = RT_TRUE;
    LOG_D("remote hello: mtu = %d", mtu);

    /* ✨ 收到 HELLO 后才回复 HELLO（如果还没发送过） */
    if (!rdev->hello_sent)
    {
        if (rpmsg_net_send_hello(rdev) != RT_EOK)
        {
            LOG_W("Failed to send HELLO reply");
        }
    }

    rpmsg_net_sync_carrier(rdev);
}
```

#### 4. LINK 消息响应 - 被动触发

在 `rpmsg_net_apply_remote_link()` 函数中实现自动回复：

``c
static void rpmsg_net_apply_remote_link(struct rpmsg_net_device *rdev,
                                        const struct rpmsg_net_link_msg *msg)
{
    // ... 更新 remote_link_up 状态 ...
    rdev->remote_link_up = (rpmsg_net_le32_to_cpu(msg->link_up) != 0U) ? RT_TRUE : RT_FALSE;
    LOG_D("remote link: up = %d", rdev->remote_link_up);

    /* ✨ 收到 LINK UP 后才回复 LINK UP（如果还没发送过） */
    if (rdev->remote_link_up && !rdev->link_state_sent)
    {
        if (rpmsg_net_send_link_state(rdev, RT_TRUE) != RT_EOK)
        {
            LOG_W("Failed to send LINK UP reply");
        }
    }

    rpmsg_net_sync_carrier(rdev);
}
```

### 完整通信时序（修正后）

```
Master (Host)                          Remote (Slave)
       |                                     |
       | 1. rpmsg_lite_master_init           |
       |------------------------------------>|
       |                                     | 
       | 2. 等待链路建立                     |
       |<------------------------------------|
       |                                     |
       | 3. HELLO (mtu=1500, mac=AA:BB:CC)   | ← Master 主动发起
       |------------------------------------>| 
       |                                     | 保存 MAC/MTU
       |                                     | remote_ready=true
       |                                     | 4. HELLO (reply) ✨
       |<------------------------------------| Remote 被动回复
       | 保存 MAC/MTU                        |
       | remote_ready=true                   |
       |                                     |
       | 5. LINK (link_up=1)                 | ← Master 主动发起
       |------------------------------------>| local_link_up=true
       |                                     | 6. LINK (link_up=1) ✨
       |<------------------------------------| remote_link_up=true
       | remote_link_up=true                 |
       | netif_carrier_on()                  | netif_carrier_on()
       |                                     |
       |========== 数据传输阶段 ==============|
```

### 关键设计原则

1. **彻底的被动响应模式**：
   - ✅ Remote 侧只在收到消息后才回复
   - ❌ 不在初始化时主动发送
   - ❌ 不在设备打开时主动发送
   - ❌ 不在线程启动时主动发送
   - 避免时序混乱和重复消息

2. **三重保护机制**：
   - **保护 1**：`rpmsg_link_wait_thread_entry()` 不主动发送
   - **保护 2**：`rpmsg_net_open()` 不主动发送
   - **保护 3**：通过 `hello_sent` 和 `link_state_sent` 标志确保只回复一次

3. **防重复机制**：
   - 通过 `hello_sent` 标志确保只回复一次 HELLO
   - 通过 `link_state_sent` 标志确保只回复一次 LINK

4. **状态同步**：
   - 双方都能正确获取对端的 MAC、MTU 和链路状态
   - 只有当三个条件都满足时才开启载波：
     - `local_link_up == true`
     - `remote_link_up == true`
     - `remote_ready == true`

### 验证要点

1. **初始化日志**：
   ```
   I/rpmsg-net: Waiting for rpmsg link up...
   I/rpmsg-net: rpmsg link is up, starting device...
   I/rpmsg-net: rpmsg-net device registered successfully
   I/rpmsg-net: Waiting for master's HELLO and LINK messages...
   I/rpmsg-net: rpmsg-net device ready for RX/TX (waiting for master)
   ```

2. **设备打开日志**：
   ```
   D/rpmsg-net: Opening rpmsg-net device
   I/rpmsg-net: Device opened, waiting for master's messages...
   ```

3. **收到 HELLO 后的日志**：
   ```
   D/rpmsg-net: remote hello: mtu = 1500
   I/rpmsg-net: Sent HELLO reply to master
   ```

4. **收到 LINK UP 后的日志**：
   ```
   D/rpmsg-net: remote link: up = 1
   I/rpmsg-net: Sent LINK UP reply to master
   I/rpmsg-net: netif carrier ON
   ```

### 文件位置

- 协议定义：`/data/biao.xia/rt-thread/bsp/lynxi/he200/drivers/rpmsg-net/rpmsg_net_proto.h`
- 实现代码：`/data/biao.xia/rt-thread/bsp/lynxi/he200/drivers/rpmsg-net/rpmsg_net.c`
- 协议文档：`/data/biao.xia/rt-thread/bsp/lynxi/he200/drivers/rpmsg-net/Protocol.md`

### 相关函数

- `rpmsg_link_wait_thread_entry()` - Remote 侧初始化线程，仅注册设备不主动发送
- `rpmsg_net_open()` - 设备打开函数，不主动发送 HELLO 和 LINK
- `rpmsg_net_apply_remote_hello()` - 被动响应 HELLO 消息并回复
- `rpmsg_net_apply_remote_link()` - 被动响应 LINK 消息并回复
- `rpmsg_net_send_hello()` - 发送 HELLO 消息（仅在收到 HELLO 后调用）
- `rpmsg_net_send_link_state()` - 发送 LINK 状态消息（仅在收到 LINK 后调用）
- `rpmsg_net_sync_carrier()` - 同步 lwIP 网络接口载波状态
