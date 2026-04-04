#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <stdbool.h>
#include <string.h>
#include "lynxi.h"

#define DBG_TAG "drv_pcie"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*
 * EP 端 MSI-X 初始化（只需设置门铃寄存器，Host 端已配置好 MSI-X 表）
 *
 * 参考 Linux 端 lynd_pcie.c 中的 msix doorbell 实现。
 */

#define LYND_PCI_MSIX_NUM_MAX 32

#define PCIE_PORT_LOGIC_OFFSET 0x700
#define PCIE_PL_MSIX_DOORBELL_OFF 0x248
#define PCI_MSIX_DRV_BASE 3

static size_t g_pcie_dbi_base;
static bool g_msix_registered[LYND_PCI_MSIX_NUM_MAX];

/**
 * rt_pci_msix_init - 初始化 MSI-X 支持
 * @dbi_base: PCIe DBI 寄存器基地址（通常来自设备树 reg["pcie-dbi"]）
 *
 * 此函数不做 MSI-X 表初始化，仅初始化使用门铃触发 MSI-X 的基础。
 */
int rt_pci_msix_init(void)
{
    g_pcie_dbi_base = pcie_ep_base_addr;

    if (!g_pcie_dbi_base)
        return -EINVAL;

    memset(g_msix_registered, 0, sizeof(g_msix_registered));

    return RT_EOK;
}

/**
 * rt_pci_msix_deinit - 反初始化
 */
void rt_pci_msix_deinit(void)
{
    g_pcie_dbi_base = RT_NULL;
    memset(g_msix_registered, 0, sizeof(g_msix_registered));
}

/**
 * rt_pci_one_msix_deinit - 反初始化指定 MSI-X 向量
 */
int rt_pci_one_msix_deinit(int vector)
{
    if (vector < 0 || vector >= LYND_PCI_MSIX_NUM_MAX)
        return -EINVAL;

    g_msix_registered[vector] = false;

    return RT_EOK;
}

/**
 * rt_pci_msix_register_vector - 注册一个 MSI-X 向量
 * @vector: 向量号，范围 0..(LYND_PCI_MSIX_NUM_MAX-1)
 *
 * 仅用于标记该向量在 RT-Thread 端可用，用于后续 raise_irq 校验。
 */
int rt_pci_msix_register_vector(int vector)
{
    if (vector < 0 || vector >= LYND_PCI_MSIX_NUM_MAX)
        return -EINVAL;

    LOG_I("Register MSI-X IRQ %d", vector);

    g_msix_registered[vector] = true;
    return RT_EOK;
}

/**
 * rt_pci_msix_raise_irq - 触发指定 MSI-X 向量对应的中断
 * @vector: 向量号
 *
 * 该接口仅向 Host 端写门铃寄存器，不在 RT-Thread 端注册 IRQ 处理函数。
 */
int rt_pci_msix_raise_irq(int vector)
{
    if (!g_pcie_dbi_base)
        return -ENODEV;

    if (vector < 0 || vector >= LYND_PCI_MSIX_NUM_MAX)
        return -EINVAL;

    if (!g_msix_registered[vector])
        return -EINVAL;

    LOG_D("base 0x%x Raise MSI-X IRQ %d", g_pcie_dbi_base, vector);

    HWREG32(g_pcie_dbi_base + PCIE_PORT_LOGIC_OFFSET + PCIE_PL_MSIX_DOORBELL_OFF) = vector;

    return RT_EOK;
}

int wakeup_host_ipc(void)
{

    rt_pci_msix_init();
    rt_pci_msix_register_vector(PCI_MSIX_DRV_BASE);
    rt_pci_msix_raise_irq(PCI_MSIX_DRV_BASE);
    rt_pci_one_msix_deinit(PCI_MSIX_DRV_BASE);

    return 0;
}
INIT_DEVICE_EXPORT(wakeup_host_ipc);

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

/*
 * @brief: 触发指定 MSI-X 向量对应的中断
 * @argc: 命令行参数数量
 * @argv: 命令行参数数组
 *
 * 示例：pci_msix_raise 0
 */
int rt_pci_msix_raise(int argc, char *argv[])
{
    int vector;
    if (argc != 2) {
        rt_kprintf("Usage: %s <vector>\n", argv[0]);
        return -EINVAL;
    }

    vector = atoi(argv[1]);
    rt_pci_msix_init();
    rt_pci_msix_register_vector(vector);
    rt_pci_msix_raise_irq(vector);
    rt_pci_msix_deinit();

    return RT_EOK;
}

MSH_CMD_EXPORT_ALIAS(rt_pci_msix_raise, pci_msix_raise, Raise MSI-X IRQ(EP side));
#endif