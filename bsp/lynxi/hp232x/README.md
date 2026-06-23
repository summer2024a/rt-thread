# HP232X BSP for KA200 (IRAM-only, direct boot)

## ✅ Current Status (2026-06-23 16:40)

**🎉 MMU完全启用成功！所有板级初始化完成！**

Test output:
```
ECO
POK!
IBKMPDUATB1RCGgUuTt

 \ | /
- RT -     Thread Operating System
 / | \     5.3.0 build Jun 23 2026 08:40:09
 2006 - 2024 Copyright by RT-Thread team
```

**Verified (调试标记解析)**:
- ✅ Complete EL transition: EL3 → EL2 → EL1
- ✅ MMU页表配置: PGD/PUD/PMD在IRAM0 (12KB)
- ✅ MMU启用成功: SCTLR_EL1.M=1, C=1, I=1
- ✅ IRAM1访问: 通过MMU访问4GB边界外地址
- ✅ Cache启用: Data Cache + Instruction Cache
- ✅ GICv3中断初始化成功
- ✅ UART驱动初始化成功
- ✅ Timer初始化成功
- ✅ Kernel启动: Banner完整显示

**调试标记含义** (`IBKMPDUATB1RCGgUuTt`):
- M/P/D/U/A/T/B/1/R/C = MMU完整启用流程
- G/g = GIC中断初始化成功
- U/u = UART初始化成功
- T/t = Timer初始化成功

**Remaining**:
- ⚠️ Shell未显示（应用初始化阶段需要调试）
- 📋 下一步：调试rt_application_init，检查线程创建

## Overview

This BSP targets the **HP232X** board using the **KA200** SoC — an ARMv8-A
dual-core processor with **two IRAM segments** and **no external DDR**.

### Key characteristics

| Feature       | Value                                    |
|---------------|------------------------------------------|
| SoC           | KA200 (ARMv8-A, Cortex-A53 class)       |
| CPU cores     | 2 (dual-core SMP)                        |
| Memory        | IRAM0 1MB + IRAM1 1MB (no DDR)          |
| Boot flow     | bootcode → rt-thread (no bootwrapper)   |
| UART          | DW APB UART0 @ 0x10006000, 115200 baud  |
| Shell         | MSH (FinSH)                              |
| GIC           | GICv3 / GIC500                           |

## Memory layout

```
IRAM0: 0x04000000 ~ 0x040FFFFF  (1MB)  — kernel text + data + bss
IRAM1: 0x04100000 ~ 0x041FFFFF  (1MB)  — heap (64KB) + page pool (128KB)
                                        — mailbox for secondary CPU at end
```

## Boot flow

Unlike HE200 which uses a bootwrapper (EL3 init, GIC init, CCI/CMN init,
spin-table, then jump to SPL/u-boot/kernel), HP232X boots **directly from
bootcode** into rt-thread:

```
Bootcode (IRAM0 @ 0x04000000)
  → sets up EL3 → EL1 transition
  → initializes GIC
  → sets cntfrq_el0
  → jumps to rt-thread entrypoint at IRAM0 + text_offset
```

The bootwrapper functionality (from `lynxi-bootwrapper`) is handled by
chip-internal bootcode, so the BSP only needs the rt-thread kernel
initialization (MMU, interrupts, UART, timer).

## Building

```bash
cd bsp/lynxi/hp232x
scons
```

The output is `rtthread.elf` and `rtthread.bin`.  The binary should be
loaded at IRAM0 (0x04000000) by the bootcode.

## Configuration

Use `menuconfig` to adjust settings:

```bash
scons --menuconfig
```

Key configs:
- `RT_CPUS_NR=2` — dual-core SMP
- `ARCH_RAM_OFFSET=0x04000000` — IRAM0 base address
- `ARCH_TEXT_OFFSET=0x0` — no offset (direct boot)
- `ARCH_HEAP_SIZE=0x10000` — 64KB heap (limited IRAM)
