# Lynxi HE200板级支持包说明

## 1. 简介


## 2. 编译说明

推荐使用[env工具](https://www.rt-thread.org/download.html#download-rt-thread-env-tool)，可以在console下进入到`bsp\raspberry-pi\raspi4-64`目录中，运行以下命令：

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

来编译这个板级支持包。如果编译正确无误，会产生 `rtthread.elf`, `rtthread.bin` 文件。

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

5. 中断0只有0~3核存在，4~7核无中断产生

6. emmc初始化失败
1、针对emmc v4的版本，需要设置SDHCI_CLOCK_PLL_EN，具体参考lx_mmc_clock_freq_change接口
