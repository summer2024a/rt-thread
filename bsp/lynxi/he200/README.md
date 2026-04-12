# Lynxi HE200板级支持包说明

## 1. 简介


## 2. 编译说明

### 2.1 更新online-packages
```
source ~/.env/env.sh
pkgs --update
```

### 2.2 编译

**编译前**须导出交叉编译器前缀（末尾保留 `-`），否则 `scons` 会找不到 `aarch64-none-elf-gcc`：

```bash
export RTT_CC_PREFIX=/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
```

推荐使用[env工具](https://www.rt-thread.org/download.html#download-rt-thread-env-tool)，在 console 下进入本 BSP 目录后执行：

```
宿主机运行以下命令：
source ~/.env/env.sh
export RTT_CC_PREFIX=/data/biao.xia/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
scons --clean
scons --menuconfig
scons -j8

容器中运行以下命令：
source ~/.env/env.sh
export RTT_CC_PREFIX=/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
scons --clean
scons --menuconfig
scons -j8
```

来编译本板级支持包（`bsp/lynxi/he200`）。若成功，会生成 `rtthread.elf`、`rtthread.bin`。

```
 \ | /
- RT -     Thread Operating System
 / | \     5.3.0 build Mar  4 2026 15:48:29
 2006 - 2024 Copyright by RT-Thread team
lwIP-2.1.2 initialized!
[I/sal.skt] Socket Abstraction Layer initialize success.
[I/utest] utest is initialize success.
[I/utest] total utest testcase num: (0)
Hi, this is RT-Thread!!
msh />
```

## 3. 支持情况

| 驱动 | 支持情况  |  备注  |
| ------ | ----  | :------:  |
| UART | 支持 | UART0|
| GIC | 支持 | GICV3|
| SMP | 支持 | 8核启动|
| EMMC | 支持 | HS200|
| IPC | 支持 |EP设备rpmsg-net|


## 4.参考板卡
```
/data/biao.xia/rt-thread/bsp/ck802/libraries/common/usart/dw_usart.c
/data/biao.xia/rt-thread/bsp/thead-smart/drivers/ck_usart.c
/data/biao.xia/rt-thread/bsp/cvitek/drivers/drv_uart.c

/data/biao.xia/rt-thread/bsp/raspberry-pi/raspi-dm2.0/drivers/sdhci/
/data/biao.xia/rt-thread/bsp/k230/drivers/interdrv/sdio/

驱动参考
/data/biao.xia/rt-thread/bsp/cvitek/drivers/
```

## 5. 问题与解决
1. mmu初始化失败

2. 串口无法输入数据

3. gic初始化失败

4. 多核启动失败

5. **IPI（SGI）异常**（8 核 **ARM Arch Timer** 的 tick 在各核上均正常）

   **现象**：问题集中在 **核间中断 IPI**（GICv3 上为 **SGI**），而非「整段本地中断全挂」。**Arch Timer** 走 **PPI**，在 **8 核 SMP 下各核均能进 tick**，说明 **每核 GICR 上对 PPI 的使能/优先级、以及基本本地中断路径** 大体可用，与「整段 GICR 基址算错导致所有 SGI/PPI 都坏」的假设 **不一致**。

   **为何 IPI 仍可单独异常**：IPI 依赖 **发端** 写系统寄存器（如 **`ICC_SGI1R_EL1`**）时的 **目标 affinity**（Aff3/Aff2/Aff1/Aff0），以及 **收端** 对对应 **SGI 号** 的 **umask/优先级**。双 cluster 时常见疏漏包括：**`rt_hw_ipi_send` / `arm_gic_send_affinity_sgi` 路径上目标 MPIDR 与 `rt_cpu_mpidr_table` 不一致**、**跨簇投递时 Aff2（cluster）未带上**、或 **调度 IPI 号在部分核上仍被 mask**。

   **本 BSP 应对照代码**：`libcpu/aarch64/common/gicv3.c` 中与 SGI 相关的实现；**`rt_cpu_mpidr_table`**（`board.c`，4～7 为 `0x100`～`0x103`）是否被 **IPI 发送路径完整使用**。可参考 **lynxi-linux** 中 `lynchip-lite-base.dtsi` 的 **CPU `reg`** 与 **GIC `reg`**（**`0x08100000` / 1MB Redistributor**），与 **HE200** 手册一致即可。

   **`cpu_release_paddr[]`**：与 Linux `cpu-release-addr` 不同属 **boot 侧已约定**，分析 IPI 时可 **忽略**。

   **建议排查**：在 **4～7 核** 上确认 **调度/停止/SMP_CALL 等 IPI** 的 **umask**；在 **发 IPI 侧** 打印或跟踪 **写入 `ICC_SGI1R_EL1` 的 affinity 字段** 是否等于目标核 **`MPIDR_EL1` 的合法截取**；必要时核对 **GICD 是否对 SGI 有额外限制**（视实现而定）。

   **上游已修**：`libcpu/aarch64/common/gicv3.c` 中 **`gicv3_sgi_target_list_set()`** 曾对 **`cpu_mask` 原地清零**，导致一次 `rt_hw_ipi_send()` 若同时命中 **多个 affinity 组**（例如掩码同时含 cluster0 与 cluster1 的 CPU），只有第一组会收到正确 **TargetList**，与 Linux 行为不一致。已改为仅在本组内迭代 **`group_mask = sgi_aff_table[i].cpu_mask[] & cpu_mask`**。

6. emmc初始化失败
1、针对emmc v4的版本，需要设置SDHCI_CLOCK_PLL_EN，具体参考lx_mmc_clock_freq_change接口
