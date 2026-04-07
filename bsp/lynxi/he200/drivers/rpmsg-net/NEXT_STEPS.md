# RPMsg-Net — 后续工作（NEXT_STEPS）

记录开放项与建议优先级；**任何 shmem / 协议变更须与 Linux host `drivers_test/rpmsg-net` 同步**。

## 结项摘要（最近一轮）

- **阶段结论**：设备->Host 吞吐长期稳定在 `~11 Mbps / ~930 pps`，设备侧长期观测到 `TX alloc_wait avg=1ms`。
- **已做但未提升**：日志开关、异步 TX、`iperf` 快路径/`udpblast`、通知合并、等待策略微调等。
- **通信异常新发现（高优先级）**：双端 `buffer_count` 不一致  
  - Host：`RPMSG_NET_BUFFER_COUNT = 256`  
  - Device：`RPMSG_PLATFORM_BUFFER_COUNT = 128`
- **建议先做**：先把 `buffer_count` 双端统一后再做通信回归；否则后续性能与稳定性结论不可靠。

---

## P0 — 联调回归与吞吐基线（在槽位已放大之后）

**已落实（需回归验证）**：

- **`board_pkgs/rpmsg-lite/.../rpmsg_platform.c`** 中 `platform_get_custom_shmem_config()`：**`buffer_payload_size=4080`**、**`buffer_count=128`**、**`vring_size=16384`**、**`vring_align=4096`**（与 host `rpmsg_net_bridge.c` 一致）。
- **`rpmsg_net_ip_mtu_from_buffer_payload()`** 按 **RL_BUFFER_PAYLOAD（已不含 std_hdr）** 计算，不再重复减 16。
- 注册后 **`rpmsg_net_apply_shmem_mtu_cap()`** + HELLO 协商 **`netif->mtu`**。

**建议动作**：

1. host↔device 双向 **iperf**，记录方向、MTU、是否仍出现 `tx buffer too small` / 高 TX fail。
2. 若改 **`buffer_count` / `vring_size`**：用 **`virtio_ring.h` 的 `vring_size(n, align)`** 校验预留区，并更新 **`rpmsg-lite/VRING.md`**（host 树内文档）。

---

## P1 — RX 零拷贝与内存（可选）

- 评估 **`RPMSG_NET_ZERO_COPY_RX`** + lwIP **`LWIP_SUPPORT_CUSTOM_PBUF`** 的收益与释放语义。
- 结合 **`RT_LWIP_PBUF_NUM`** / 内存池，观察 RX 高峰 **`pbuf_alloc`** 失败与 purge 逻辑。

---

## P1 — SMP 与调度（延续）

- 绑核与 **DW 定时器 IRQ** 亲和性已可配置；若仍有丢包，结合 **`list_thread`**、GIC 与优先级再调。
- 热路径避免高频 **`LOG_D` / `LOG_HEX`**。

---

## P2 — 协议与文档

- **`Protocol.md`**：可选补充 **MTU 上限与 `buffer_payload_size` 对应关系**（与 **`VRING.md`** 交叉引用）。
- iperf 对比时注明 **方向** 与 **TCP 窗口**。

---

## P2 — 构建与仓库卫生

- **`bsp/lynxi/he200/.config`** / **`rtconfig.h`** / **`board_pkgs/Kconfig`** / 顶层 **`Kconfig`** 同步；合入前 **`scons`** 全量编译（需设置 **`RTT_CC_PREFIX`**，见 **`README.md`**）。
- 跨目录修改在提交信息中分项说明（**`board_pkgs/rpmsg-lite/.../rpmsg_platform.c`**、`rpmsg_net.c` 等）。
- **`packages/pkgs.json`** 通常不入库：新增协作者时说明 **board 定制在 `board_pkgs`**，在线包仅 **`packages/`** 本地 Env 拉取；避免误以为 `PKG_USING_*` 仍指向 `packages/rpmsg-lite-latest`。

---

## 已搁置 / 待决策

- 驱动内 **以太网帧跨多 RPMsg 消息分片重组**：复杂度高，当前以 **槽深 + MTU 对齐** 为主。

---

## 已解决 / 归档（便于对照）

- **host ping 设备不回**：**`rt_mq_recv()`** 成功条件为 **返回值 > 0**（非 `RT_EOK`）。
- **吞吐极低（Kbit/s 级）**：多与 **单槽装不下 MTU**、host **单 skb + RL_DONT_BLOCK + 长延迟重试** 等有关；当前 host 侧已改为 **TX 队列 + nocopy + `RL_BLOCK`**（见 host **`CONTEXT.md` / `NEXT_STEPS.md`**）。
