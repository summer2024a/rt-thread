#include <rtconfig.h>

#ifdef RT_USING_SMP

    .section .text.cpu_id, "ax"
    .align 2

/*
 * ============================================================================
 * 函数：rt_hw_cpu_id_set
 * 功能：设置当前 CPU 的逻辑 ID
 * 输入：x0 = CPU ID (0-7)
 * 输出：无
 * 说明：将 CPU ID 保存到 TPIDR_EL1 寄存器，每个 CPU 独立
 * ============================================================================
 */
.globl rt_hw_cpu_id_set
.type rt_hw_cpu_id_set, %function
rt_hw_cpu_id_set:
    mov x9, lr
    mrs x0,MPIDR_EL1
    and x1, x0, #15
    msr tpidr_el1, x1
    mov lr, x9
    RET

/*
 * ============================================================================
 * 函数：rt_hw_cpu_id
 * 功能：获取当前 CPU 的逻辑 ID
 * 输入：无
 * 输出：x0 = CPU ID (0-7)
 * 说明：从 TPIDR_EL1 寄存器读取 CPU ID
 * ============================================================================
 */
    .globl rt_hw_cpu_id
    .type rt_hw_cpu_id, %function

/*
 * rt_hw_cpu_id - Get current CPU ID
 *
 * KA200 MPIDR 映射关系:
 *   CPU0: MPIDR[8:0] = 0x000  -> CPU ID 0
 *   CPU1: MPIDR[8:0] = 0x001  -> CPU ID 1
 *   CPU2: MPIDR[8:0] = 0x002  -> CPU ID 2
 *   CPU3: MPIDR[8:0] = 0x003  -> CPU ID 3
 *   CPU4: MPIDR[8:0] = 0x100  -> CPU ID 4
 *   CPU5: MPIDR[8:0] = 0x101  -> CPU ID 5
 *   CPU6: MPIDR[8:0] = 0x102  -> CPU ID 6
 *   CPU7: MPIDR[8:0] = 0x103  -> CPU ID 7
 *
 * 返回值:
 *   x0 = CPU ID (0-7)
 */
rt_hw_cpu_id:
    /* 读取 MPIDR_EL1 寄存器 */
    mrs     x0, mpidr_el1

    /* 提取 Aff0 和 Aff1 字段 */
    /* MPIDR 格式: [63:40]Reserved [39:32]Aff3 [31:24]Reserved [23:16]Aff2 [15:8]Aff1 [7:0]Aff0 */
    /* KA200: Aff1[8] 表示簇 ID, Aff0[7:0] 表示核内 ID */

    /* 保存原始 MPIDR 值 */
    mov     x1, x0

    /* 提取 Aff1[8] (簇 ID) */
    lsr     x2, x0, #8
    and     x2, x2, #0x1
    lsl     x2, x2, #2          /* 簇 ID * 4 (每簇 4 核) */

    /* 提取 Aff0[7:0] (核 ID) */
    and     x0, x1, #0xFF

    /* 验证核 ID 是否有效 (0-3) */
    cmp     x0, #4
    bge     .cpu_id_invalid

    /* 计算最终 CPU ID = 簇 ID * 4 + 核 ID */
    add     x0, x2, x0

    ret

.cpu_id_invalid:
    /* 无效的 CPU ID，返回 0 */
    mov     x0, #0
    ret

    .size rt_hw_cpu_id, .-rt_hw_cpu_id

#ifdef RT_USING_OFW
    /* 使用 MPIDR 表查找 CPU ID (适用于设备树配置) */
    .globl rt_hw_cpu_id_from_table
    .type rt_hw_cpu_id_from_table, %function

/*
 * rt_hw_cpu_id_from_table - Get CPU ID from MPIDR table
 *
 * 输入:
 *   x0 = 当前 CPU 的 MPIDR 值
 *
 * 输出:
 *   x0 = CPU ID (0-7)
 *   x1 = 临时寄存器
 *   x2 = 临时寄存器
 *   x3 = 临时寄存器
 */
rt_hw_cpu_id_from_table:
    /* 保存调用者保存寄存器 */
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    /* 加载 MPIDR 表地址 */
    adrp    x1, rt_cpu_mpidr_table
    add     x1, x1, #:lo12:rt_cpu_mpidr_table

    /* 掩码：提取 Aff0 和 Aff1 */
    mov     x3, #0xff00ffffff
    and     x0, x0, x3

    /* CPU ID 计数器 */
    mov     x2, #0

.cpu_id_table_loop:
    /* 加载表项 */
    ldr     x3, [x1], #8

    /* 检查表项是否为 0 (结束标记) */
    cbz     x3, .cpu_id_not_found

    /* 掩码并比较 */
    and     x3, x3, x3
    and     x3, x3, x3
    cmp     x3, x0
    beq     .cpu_id_found

    /* 下一个 CPU ID */
    add     x2, x2, #1

    /* 检查是否超出最大 CPU 数 */
    cmp     x2, #8
    bge     .cpu_id_not_found

    b       .cpu_id_table_loop

.cpu_id_found:
    mov     x0, x2
    b       .cpu_id_table_end

.cpu_id_not_found:
    mov     x0, #0

.cpu_id_table_end:
    /* 恢复调用者保存寄存器 */
    ldp     x29, x30, [sp], #16
    ret

    .size rt_hw_cpu_id_from_table, .-rt_hw_cpu_id_from_table
#endif /* RT_USING_OFW */

    /* CPU 启动时的 MPIDR 保存函数 */
    .globl rt_hw_cpu_mpidr_save
    .type rt_hw_cpu_mpidr_save, %function

/*
 * rt_hw_cpu_mpidr_save - Save MPIDR to table
 *
 * 输入:
 *   x0 = CPU ID
 *
 * 输出:
 *   无
 */
rt_hw_cpu_mpidr_save:
    /* 保存调用者保存寄存器 */
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    /* 读取 MPIDR_EL1 */
    mrs     x1, mpidr_el1

    /* 加载 MPIDR 表地址 */
    adrp    x2, rt_cpu_mpidr_table
    add     x2, x2, #:lo12:rt_cpu_mpidr_table

    /* 计算表项地址 = 基地址 + CPU_ID * 8 */
    lsl     x0, x0, #3
    add     x2, x2, x0

    /* 保存 MPIDR 到表 */
    str     x1, [x2]

    /* 恢复调用者保存寄存器 */
    ldp     x29, x30, [sp], #16
    ret

    .size rt_hw_cpu_mpidr_save, .-rt_hw_cpu_mpidr_save

#endif /* RT_USING_SMP */

    .end