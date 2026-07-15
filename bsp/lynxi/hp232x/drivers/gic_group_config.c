/*
 * HP232X GIC中断组别配置
 *
 * 问题：Timer30和UART57中断在Group0 Secure，Non-secure EL1无法处理
 * 解决：在Non-secure EL1重新配置中断组别到Group1 NS
 *
 * 中断号定义：
 *   - Timer IRQ: 30 (PPI, Private Peripheral Interrupt)
 *   - UART IRQ: 57 (SPI, Shared Peripheral Interrupt, IRQ_UART0 = 32 + 25)
 *
 * GICv3寄存器：
 *   - GICR_IGROUPR0: SGI/PPI组别寄存器 (IRQ 0-31)
 *   - GICR_IGRPMODR0: SGI/PPI模式寄存器 (IRQ 0-31)
 *   - GICD_IGROUPR: SPI组别寄存器 (IRQ 32+)
 *   - GICD_IGRPMODR: SPI模式寄存器 (IRQ 32+)
 *
 * Copyright (c) 2025 lynxi
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rthw.h>
#include <gicv3.h>
#include "lynxi.h"
#include "biz_log.h"

/* GICv3寄存器地址定义 */
#define GICR_SGI_BASE_OFFSET    0x10000     /* SGI_base offset from Redistributor base */
#define GICR_IGROUPR0_OFFSET    0x80        /* GICR_IGROUPR0 offset from SGI_base */
#define GICR_IGRPMODR0_OFFSET   0xD00       /* GICR_IGRPMODR0 offset from SGI_base */

#define GICD_IGROUPR_OFFSET     0x80        /* GICD_IGROUPR base offset */
#define GICD_IGRPMODR_OFFSET    0xD00       /* GICD_IGRPMODR base offset */

/* Timer andUART interrupt */
#define TIMER_IRQ_NUM           30
#define UART_IRQ_NUM            57
#define I2C0_IRQ_NUM            63

/**
 * 配置Timer中断组别为Group1 Non-Secure
 *
 * Timer30是PPI中断，需要在GIC Redistributor的SGI_base配置
 *
 * @param redist_base: Redistributor基地址（每个CPU有自己的Redistributor）
 */
void hp232x_config_timer_interrupt_group(rt_uint64_t redist_base)
{
    volatile rt_uint32_t *gicr_igroupr0;
    volatile rt_uint32_t *gicr_igrpmodr0;
    rt_uint32_t group_mask;
    rt_uint32_t mod_mask;

    /* 计算SGI_base地址 */
    rt_uint64_t sgi_base = redist_base + GICR_SGI_BASE_OFFSET;

    /* Timer30在IGROUPR0的第30位 */
    group_mask = (1 << TIMER_IRQ_NUM);

    /* Timer30在IGRPMODR0的第30位 */
    mod_mask = (1 << TIMER_IRQ_NUM);

    HP_LOGI("[GIC_GROUP] Configuring Timer IRQ%d group...\n", TIMER_IRQ_NUM);

    /* 配置GICR_IGROUPR0：设置Timer30为Group1 */
    gicr_igroupr0 = (volatile rt_uint32_t *)(sgi_base + GICR_IGROUPR0_OFFSET);

    /* 读取当前值 */
    rt_uint32_t current_group = *gicr_igroupr0;
    HP_LOGI("[GIC_GROUP] GICR_IGROUPR0 current=0x%08x, Timer30 bit=%d\n",
               current_group, (current_group & group_mask) ? 1 : 0);

    /* 设置Timer30为Group1（bit=1表示Group1，bit=0表示Group0） */
    *gicr_igroupr0 |= group_mask;

    /* 读取新值验证 */
    rt_uint32_t new_group = *gicr_igroupr0;
    HP_LOGI("[GIC_GROUP] GICR_IGROUPR0 new=0x%08x, Timer30 bit=%d (expect=1)\n",
               new_group, (new_group & group_mask) ? 1 : 0);

    /* 配置GICR_IGRPMODR0：设置Timer30为Non-Secure Group1 */
    gicr_igrpmodr0 = (volatile rt_uint32_t *)(sgi_base + GICR_IGRPMODR0_OFFSET);

    /* 读取当前值 */
    rt_uint32_t current_mod = *gicr_igrpmodr0;
    HP_LOGI("[GIC_GROUP] GICR_IGRPMODR0 current=0x%08x, Timer30 bit=%d\n",
               current_mod, (current_mod & mod_mask) ? 1 : 0);

    /* 设置Timer30为Non-Secure（bit=0表示Non-Secure，bit=1表示Secure） */
    *gicr_igrpmodr0 &= ~mod_mask;  /* 清除bit30，设置为Non-Secure */

    /* 读取新值验证 */
    rt_uint32_t new_mod = *gicr_igrpmodr0;
    HP_LOGI("[GIC_GROUP] GICR_IGRPMODR0 new=0x%08x, Timer30 bit=%d (expect=0)\n",
               new_mod, (new_mod & mod_mask) ? 1 : 0);

    /* 内存屏障确保写入生效 */
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    HP_LOGI("[GIC_GROUP] Timer IRQ%d configured to Group1 NS ✓\n", TIMER_IRQ_NUM);
}

/**
 * 配置UART中断组别为Group1 Non-Secure
 *
 * UART57是SPI中断，需要在GIC Distributor配置
 *
 * @param dist_base: Distributor基地址
 */
void hp232x_config_uart_interrupt_group(rt_uint64_t dist_base)
{
    volatile rt_uint32_t *gicd_igroupr;
    volatile rt_uint32_t *gicd_igrpmodr;
    rt_uint32_t group_reg_index;
    rt_uint32_t group_bit_index;
    rt_uint32_t group_mask;
    rt_uint32_t mod_reg_index;
    rt_uint32_t mod_bit_index;
    rt_uint32_t mod_mask;

    HP_LOGI("[GIC_GROUP] Configuring UART IRQ%d group...\n", UART_IRQ_NUM);

    /* UART57在GICD_IGROUPR1寄存器（每个寄存器管理32个中断）
     * 寄存器编号：57 / 32 = 1
     * 位索引：57 % 32 = 25
     */
    group_reg_index = UART_IRQ_NUM / 32;
    group_bit_index = UART_IRQ_NUM % 32;
    group_mask = (1 << group_bit_index);

    /* UART57在GICD_IGRPMODR1寄存器 */
    mod_reg_index = UART_IRQ_NUM / 32;
    mod_bit_index = UART_IRQ_NUM % 32;
    mod_mask = (1 << mod_bit_index);

    HP_LOGI("[GIC_GROUP] UART%d: reg_index=%d, bit_index=%d, mask=0x%x\n",
               UART_IRQ_NUM, group_reg_index, group_bit_index, group_mask);

    /* 配置GICD_IGROUPR：设置UART57为Group1 */
    gicd_igroupr = (volatile rt_uint32_t *)(dist_base + GICD_IGROUPR_OFFSET + group_reg_index * 4);

    /* 读取当前值 */
    rt_uint32_t current_group = *gicd_igroupr;
    HP_LOGI("[GIC_GROUP] GICD_IGROUPR%d current=0x%08x, UART57 bit=%d\n",
               group_reg_index, current_group, (current_group & group_mask) ? 1 : 0);

    /* 设置UART57为Group1 */
    *gicd_igroupr |= group_mask;

    /* 读取新值验证 */
    rt_uint32_t new_group = *gicd_igroupr;
    HP_LOGI("[GIC_GROUP] GICD_IGROUPR%d new=0x%08x, UART57 bit=%d (expect=1)\n",
               group_reg_index, new_group, (new_group & group_mask) ? 1 : 0);

    /* 配置GICD_IGRPMODR：设置UART57为Non-Secure */
    gicd_igrpmodr = (volatile rt_uint32_t *)(dist_base + GICD_IGRPMODR_OFFSET + mod_reg_index * 4);

    /* 读取当前值 */
    rt_uint32_t current_mod = *gicd_igrpmodr;
    HP_LOGI("[GIC_GROUP] GICD_IGRPMODR%d current=0x%08x, UART57 bit=%d\n",
               mod_reg_index, current_mod, (current_mod & mod_mask) ? 1 : 0);

    /* 设置UART57为Non-Secure */
    *gicd_igrpmodr &= ~mod_mask;

    /* 读取新值验证 */
    rt_uint32_t new_mod = *gicd_igrpmodr;
    HP_LOGI("[GIC_GROUP] GICD_IGRPMODR%d new=0x%08x, UART57 bit=%d (expect=0)\n",
               mod_reg_index, new_mod, (new_mod & mod_mask) ? 1 : 0);

    /* 内存屏障 */
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    HP_LOGI("[GIC_GROUP] UART IRQ%d configured to Group1 NS ✓\n", UART_IRQ_NUM);
}

void hp232x_config_i2c_interrupt_group(rt_uint64_t dist_base)
{
    volatile rt_uint32_t *gicd_igroupr;
    volatile rt_uint32_t *gicd_igrpmodr;
    rt_uint32_t reg_index = I2C0_IRQ_NUM / 32;
    rt_uint32_t bit_index = I2C0_IRQ_NUM % 32;
    rt_uint32_t mask = (1U << bit_index);

    gicd_igroupr = (volatile rt_uint32_t *)(dist_base + GICD_IGROUPR_OFFSET + reg_index * 4);
    gicd_igrpmodr = (volatile rt_uint32_t *)(dist_base + GICD_IGRPMODR_OFFSET + reg_index * 4);

    *gicd_igroupr |= mask;
    *gicd_igrpmodr &= ~mask;

    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    HP_LOGI("[GIC_GROUP] I2C IRQ%d configured to Group1 NS ✓\n", I2C0_IRQ_NUM);
}

/**
 * 初始化HP232X中断组别配置
 *
 * 在Non-secure EL1启动后调用，确保Timer和UART中断配置为Group1 NS
 */
void hp232x_init_interrupt_groups(void)
{
    rt_uint64_t dist_base;
    rt_uint64_t redist_base;
    int cpu_id;

    HP_LOGI("\n");
    HP_LOGI("========================================\n");
    HP_LOGI("HP232X Interrupt Group Configuration\n");
    HP_LOGI("========================================\n");

    /* 获取GIC基地址 */
    dist_base = platform_get_gic_dist_base();
    HP_LOGI("[GIC_GROUP] GIC Distributor base: 0x%llx\n", dist_base);

    /* 获取当前CPU的Redistributor基地址 */
    redist_base = platform_get_gic_redist_base();

#ifdef RT_USING_SMP
    /* SMP系统：需要计算当前CPU的Redistributor地址 */
    cpu_id = rt_hw_cpu_id();
    HP_LOGI("[GIC_GROUP] Current CPU ID: %d\n", cpu_id);

    /* 每个CPU的Redistributor大小为128KB (2 * 64KB) */
    redist_base += cpu_id * (2 * 0x10000);
#else
    cpu_id = 0;
#endif

    HP_LOGI("[GIC_GROUP] GIC Redistributor base for CPU%d: 0x%llx\n", cpu_id, redist_base);

    /* 配置Timer中断组别 */
    hp232x_config_timer_interrupt_group(redist_base);

    /* 配置UART中断组别 */
    hp232x_config_uart_interrupt_group(dist_base);
    hp232x_config_i2c_interrupt_group(dist_base);

    HP_LOGI("[GIC_GROUP] Interrupt group configuration complete ✓\n");
    HP_LOGI("========================================\n");
    HP_LOGI("\n");
}

/**
 * 验证中断组别配置
 *
 * 用于调试，检查Timer和UART中断的组别设置是否正确
 */
void hp232x_verify_interrupt_groups(void)
{
    rt_uint64_t dist_base;
    rt_uint64_t redist_base;
    rt_uint64_t sgi_base;
    volatile rt_uint32_t *gicr_igroupr0;
    volatile rt_uint32_t *gicr_igrpmodr0;
    volatile rt_uint32_t *gicd_igroupr1;
    volatile rt_uint32_t *gicd_igrpmodr1;
    rt_uint32_t timer_group_bit;
    rt_uint32_t timer_mod_bit;
    rt_uint32_t uart_group_bit;
    rt_uint32_t uart_mod_bit;

    HP_LOGI("\n");
    HP_LOGI("========================================\n");
    HP_LOGI("HP232X Interrupt Group Verification\n");
    HP_LOGI("========================================\n");

    /* 获取基地址 */
    dist_base = platform_get_gic_dist_base();
    redist_base = platform_get_gic_redist_base();

#ifdef RT_USING_SMP
    int cpu_id = rt_hw_cpu_id();
    redist_base += cpu_id * (2 * 0x10000);
#endif

    sgi_base = redist_base + GICR_SGI_BASE_OFFSET;

    /* 检查Timer30组别 */
    gicr_igroupr0 = (volatile rt_uint32_t *)(sgi_base + GICR_IGROUPR0_OFFSET);
    gicr_igrpmodr0 = (volatile rt_uint32_t *)(sgi_base + GICR_IGRPMODR0_OFFSET);

    timer_group_bit = (*gicr_igroupr0 >> TIMER_IRQ_NUM) & 1;
    timer_mod_bit = (*gicr_igrpmodr0 >> TIMER_IRQ_NUM) & 1;

    HP_LOGI("[GIC_VERIFY] Timer IRQ%d:\n", TIMER_IRQ_NUM);
    HP_LOGI("  GICR_IGROUPR0: 0x%08x (bit30=%d, expect=1)\n", *gicr_igroupr0, timer_group_bit);
    HP_LOGI("  GICR_IGRPMODR0: 0x%08x (bit30=%d, expect=0)\n", *gicr_igrpmodr0, timer_mod_bit);

    if (timer_group_bit == 1 && timer_mod_bit == 0) {
        HP_LOGI("  ✓ Timer configured to Group1 NS (correct)\n");
    } else {
        HP_LOGI("  ✗ Timer NOT in Group1 NS (wrong!) - Group=%d, Mod=%d\n",
                   timer_group_bit ? 1 : 0, timer_mod_bit ? "Secure" : "NS");
    }

    /* 检查UART57组别 */
    gicd_igroupr1 = (volatile rt_uint32_t *)(dist_base + GICD_IGROUPR_OFFSET + 1 * 4);
    gicd_igrpmodr1 = (volatile rt_uint32_t *)(dist_base + GICD_IGRPMODR_OFFSET + 1 * 4);

    uart_group_bit = (*gicd_igroupr1 >> 25) & 1;  /* UART57在bit25 */
    uart_mod_bit = (*gicd_igrpmodr1 >> 25) & 1;

    HP_LOGI("[GIC_VERIFY] UART IRQ%d:\n", UART_IRQ_NUM);
    HP_LOGI("  GICD_IGROUPR1: 0x%08x (bit25=%d, expect=1)\n", *gicd_igroupr1, uart_group_bit);
    HP_LOGI("  GICD_IGRPMODR1: 0x%08x (bit25=%d, expect=0)\n", *gicd_igrpmodr1, uart_mod_bit);

    if (uart_group_bit == 1 && uart_mod_bit == 0) {
        HP_LOGI("  ✓ UART configured to Group1 NS (correct)\n");
    } else {
        HP_LOGI("  ✗ UART NOT in Group1 NS (wrong!) - Group=%d, Mod=%d\n",
                   uart_group_bit ? 1 : 0, uart_mod_bit ? "Secure" : "NS");
    }

    HP_LOGI("========================================\n");
    HP_LOGI("\n");
}