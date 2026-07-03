/*
 * GIC监控线程 - 定期打印GIC关键信息和ISR统计
 */

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "lynxi.h"  /* for MAX_HANDLERS */

/* GIC寄存器地址 */
#define GICD_BASE       0x08000000
#define GICR_BASE       0x08100000

/* GIC Distributor寄存器 */
#define GICD_CTLR       (GICD_BASE + 0x000)
#define GICD_TYPER      (GICD_BASE + 0x004)
#define GICD_ISENABLER1 (GICD_BASE + 0x104)  /* SPI 32-63 */
#define GICD_ISENABLER2 (GICD_BASE + 0x108)  /* SPI 64-95 */

/* GIC Redistributor寄存器 (SGI_base = GICR_BASE + 0x10000) */
#define GICR_SGI_BASE   (GICR_BASE + 0x10000)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x100)  /* SGI/PPI 0-31 */
#define GICR_IGROUPR0   (GICR_SGI_BASE + 0x80)
#define GICR_IGRPMODR0  (GICR_SGI_BASE + 0xD00)

/* MAX_HANDLERS定义在lynxi.h中 */
#ifndef MAX_HANDLERS
#define MAX_HANDLERS 64
#endif

/* ISR计数器（外部变量） */
extern volatile rt_uint32_t gtimer_isr_counter;
extern volatile rt_uint32_t uart_isr_count;

/* 系统ISR表（用于统计） */
extern struct rt_irq_desc isr_table[];

static void print_gic_status(void)
{
    volatile rt_uint32_t *gicd_ctlr = (rt_uint32_t *)GICD_CTLR;
    volatile rt_uint32_t *gicd_typer = (rt_uint32_t *)GICD_TYPER;
    volatile rt_uint32_t *gicd_isenabler1 = (rt_uint32_t *)GICD_ISENABLER1;
    volatile rt_uint32_t *gicd_isenabler2 = (rt_uint32_t *)GICD_ISENABLER2;
    volatile rt_uint32_t *gicr_isenabler0 = (rt_uint32_t *)GICR_ISENABLER0;
    volatile rt_uint32_t *gicr_igroupr0 = (rt_uint32_t *)GICR_IGROUPR0;
    volatile rt_uint32_t *gicr_igrpmodr0 = (rt_uint32_t *)GICR_IGRPMODR0;

    rt_uint64_t icc_igrpen1_el1 = 0;
    rt_uint64_t icc_pmr_el1 = 0;
    rt_uint64_t daif = 0;

    asm volatile("mrs %0, S3_0_C12_C12_7" : "=r"(icc_igrpen1_el1));  /* ICC_IGRPEN1_EL1 */
    asm volatile("mrs %0, S3_0_C4_C6_0" : "=r"(icc_pmr_el1));        /* ICC_PMR_EL1 */
    asm volatile("mrs %0, DAIF" : "=r"(daif));

    rt_kprintf("\n");
    rt_kprintf("========================================\n");
    rt_kprintf(" GIC Monitor - Time: %d ticks\n", rt_tick_get());
    rt_kprintf("========================================\n");

    /* GIC Distributor状态 */
    rt_kprintf("\n[GIC Distributor]\n");
    rt_kprintf("  CTLR:      0x%08x (Grp1NS=%d ARE_NS=%d DS=%d)\n",
               *gicd_ctlr,
               (*gicd_ctlr >> 1) & 1,
               (*gicd_ctlr >> 5) & 1,
               (*gicd_ctlr >> 6) & 1);
    rt_kprintf("  TYPER:     0x%08x (IRQ lines=%d)\n",
               *gicd_typer, (*gicd_typer & 0x1F));

    /* GIC Redistributor状态 (Timer30是PPI) */
    rt_kprintf("\n[GIC Redistributor - SGI/PPI]\n");
    rt_kprintf("  ISENABLER0:  0x%08x (Timer30=%d, bit30)\n",
               *gicr_isenabler0, (*gicr_isenabler0 >> 30) & 1);
    rt_kprintf("  IGROUPR0:    0x%08x (Timer30 Group=%d)\n",
               *gicr_igroupr0, (*gicr_igroupr0 >> 30) & 1);
    rt_kprintf("  IGRPMODR0:   0x%08x (Timer30 Mod=%d)\n",
               *gicr_igrpmodr0, (*gicr_igrpmodr0 >> 30) & 1);

    /* SPI中断启用状态 */
    rt_kprintf("\n[GIC Enabled Interrupts - SPI]\n");
    rt_kprintf("  ISENABLER[1]: 0x%08x (UART57=%d, SPI 32-63)\n",
               *gicd_isenabler1, (*gicd_isenabler1 >> 25) & 1);
    rt_kprintf("  ISENABLER[2]: 0x%08x (SPI 64-95)\n", *gicd_isenabler2);

    /* ICC系统寄存器 */
    rt_kprintf("\n[ICC System Registers]\n");
    rt_kprintf("  ICC_IGRPEN1_EL1: 0x%llx (Enable=%d)\n",
               icc_igrpen1_el1, icc_igrpen1_el1 & 1);
    rt_kprintf("  ICC_PMR_EL1:     0x%llx (Priority mask)\n", icc_pmr_el1);
    rt_kprintf("  DAIF:            0x%llx (IRQ mask=%d)\n",
               daif, (daif >> 7) & 1);

    /* ISR统计 */
    rt_kprintf("\n[ISR Statistics]\n");
    // rt_kprintf("  Timer ISR calls:  %d\n", gtimer_isr_counter);
    // rt_kprintf("  UART ISR calls:   %d\n", uart_isr_count);
    rt_kprintf("  System tick:      %d\n", rt_tick_get());

    /* ISR表信息 */
    rt_kprintf("\n[ISR Table - Active Handlers]\n");
    int active_isr = 0;
    for (int i = 0; i < MAX_HANDLERS; i++) {  /* 使用MAX_HANDLERS，避免越界 */
        if (isr_table[i].handler != RT_NULL) {
            active_isr++;
            if (i <= 5 || i == 30 || i == 57) {  /* 只显示关键ISR */
                rt_kprintf("  IRQ %2d: handler=%p\n",
                           i, isr_table[i].handler);
            }
        }
    }
    rt_kprintf("  Total active ISR: %d (max=%d)\n", active_isr, MAX_HANDLERS);

    rt_kprintf("\n========================================\n");
}

static void pmon_gic_thread(void *parameter)
{
    int count = 0;

    while (1) {

        count++;

        /* 仅每5次迭代输出一次 */
        if (count % 5 == 0) {
            // rt_kprintf("[GIC] #%d Tick=%d ISR=%d\n", count, rt_tick_get(), gtimer_isr_counter);
            rt_kprintf("[GIC] #%d Tick=%d\n", count, rt_tick_get());
        }

        /* Busy wait ~500ms — rt_thread_mdelay hangs because timer PPI
         * can't be enabled (HCR_EL2.TTC=1 traps CNTP_CTL_EL0 writes). */
        // for (volatile int i = 0; i < 5000000; i++) { }
        rt_thread_mdelay(500);
        // rt_hw_us_delay(10000);

        if (count > 500) {
            rt_kprintf("[GIC] Done\n");
            break;
        }
    }
}

int pmon_gic_init(void)
{
    rt_thread_t tid = rt_thread_create("pmon_gic",
                                       pmon_gic_thread,
                                       RT_NULL,
                                       4096,
                                       15,
                                       10);

    if (tid) {
        rt_thread_startup(tid);
        rt_kprintf("[GIC Monitor] Thread created\n");
        return RT_EOK;
    }

    rt_kprintf("[GIC Monitor] Failed to create thread\n");
    return -RT_ERROR;
}

INIT_APP_EXPORT(pmon_gic_init);