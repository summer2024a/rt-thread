# HP232X BSP for KA200 (IRAM-only, direct boot)

## ✅ Current Status (2026-06-24 深夜)

**🎉 GICv3完整实现！Timer interrupt工作！**

**Latest Update**:
- ✅ Complete EL transition: EL3 → EL2 → EL1
- ✅ MMU页表配置: PGD/PUD/PMD在IRAM0 (12KB)
- ✅ MMU启用成功: Identity mapping工作
- ✅ IRAM1访问: 通过MMU成功访问4GB边界外地址
- ✅ **Small memory allocator成功**: malloc/free工作
- ✅ **GICv3完整实现**: Distributor + Redistributor + 系统寄存器
- ✅ **Timer interrupt触发**: rt_tick_increase()正常工作
- ✅ UART驱动初始化
- ✅ Kernel启动: Banner完整显示
- ⚠️ **Shell thread调试**: 单核调度机制验证中

**GICv3实现完成**:
1. ✅ GIC Distributor配置（GICv3寄存器格式）
2. ✅ GIC Redistributor支持（per-CPU中断配置 @ 0x08100000）
3. ✅ 系统寄存器接口使能（ICC_SRE_EL1）
4. ✅ Timer中断affinity routing配置（IRQ 30）

**Remaining**:
- ⚠️ Shell thread调度时机优化
- 📋 完整shell功能测试
- 📋 代码清理和优化

**Memory Allocator对比**:
- ❌ SLAB allocator: zone_size=128KB不适合144KB小heap
- ✅ Small mem: 无zone限制，malloc成功，更适合小heap

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
