/*
 * drv_pcie.c — PCIe init / EP ATU / DMA loopback (hp640 spl_cmd.c port).
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_PCIE

#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "biz_exec_handlers.h"
#include "drv_emmc.h"
#include "biz_ipc.h"
#include "biz_log.h"
#include "tick.h"

#define LYNCHIP_CPR                 0x12500000UL
#define BOOT_SELECT                 (LYNCHIP_CPR + 0x64UL)
#define CLOCK_SW_CTRL               (LYNCHIP_CPR + 0xCCUL)
#define PCIE_PLL_CONFIG0            (LYNCHIP_CPR + 0x54UL)
#define PCIE_PLL_CONFIG2            (LYNCHIP_CPR + 0x5CUL)
#define PCIE_CTRL                   (LYNCHIP_CPR + 0x84UL)
#define PLL_LOCK_STATUS             (LYNCHIP_CPR + 0xD4UL)
#define PLL_LOCK_VALUE              0x3F3FU

#define PCIE_AXI_DBI_ADDR           0x1A000000UL
#define PCIE_HEADER_TYPE_ADDRESS_OFFSET 0x0CU
#define PCIE_HEADER_TYPE_MASK       0x10000U

#define PCIE_DMA_CAP_BASE           0x200000UL
#define PCIE_DMA_CAP_BASE_OFF       (PCIE_AXI_DBI_ADDR + PCIE_DMA_CAP_BASE)

#define PCIE_DMA_WRITE_CH0_PWR_EN_OFF           0x128U
#define PCIE_DMA_WRITE_CH1_PWR_EN_OFF           0x12CU
#define PCIE_DMA_WRITE_CH2_PWR_EN_OFF           0x130U
#define PCIE_DMA_WRITE_CH3_PWR_EN_OFF           0x134U
#define PCIE_DMA_READ_CH0_PWR_EN_OFF            0x168U
#define PCIE_DMA_READ_CH1_PWR_EN_OFF            0x16CU
#define PCIE_DMA_READ_CH2_PWR_EN_OFF            0x170U
#define PCIE_DMA_READ_CH3_PWR_EN_OFF            0x174U
#define PCIE_DMA_WRITE_ENGINE_EN_OFF            0x0CU
#define PCIE_DMA_WRITE_DONE_IMWR_LOW_OFF        0x60U
#define PCIE_DMA_WRITE_DONE_IMWR_HIGH_OFF       0x64U
#define PCIE_DMA_WRITE_ABORT_IMWR_LOW_OFF       0x68U
#define PCIE_DMA_WRITE_ABORT_IMWR_HIGH_OFF      0x6CU
#define PCIE_DMA_WRITE_CH01_IMWR_DATA_OFF       0x70U
#define PCIE_DMA_WRITE_CH23_IMWR_DATA_OFF       0x74U
#define PCIE_DMA_WRITE_CHANNEL_ARB_WEIGHT_LOW_OFF   0x18U
#define PCIE_DMA_WRITE_CHANNEL_ARB_WEIGHT_HIGH_OFF  0x1CU
#define PCIE_DMA_READ_INT_MASK_OFF              0xA8U
#define PCIE_DMA_READ_LINKED_LIST_ERR_EN_OFF    0xC4U
#define PCIE_DMA_WRITE_INT_MASK_OFF             0x54U
#define PCIE_DMA_WRITE_LINKED_LIST_ERR_EN_OFF   0x90U
#define PCIE_DMA_READ_ENGINE_EN_OFF             0x2CU
#define PCIE_DMA_READ_DONE_IMWR_LOW_OFF         0xCCU
#define PCIE_DMA_READ_DONE_IMWR_HIGH_OFF        0xD0U
#define PCIE_DMA_READ_ABORT_IMWR_LOW_OFF        0xD4U
#define PCIE_DMA_READ_ABORT_IMWR_HIGH_OFF       0xD8U
#define PCIE_DMA_READ_CH01_IMWR_DATA_OFF        0xDCU
#define PCIE_DMA_READ_CH23_IMWR_DATA_OFF        0xE0U
#define PCIE_DMA_READ_CHANNEL_ARB_WEIGHT_LOW_OFF    0x38U
#define PCIE_PCIE_DMA_READ_CHANNEL_ARB_WEIGHT_HIGH_OFF 0x3CU
#define PCIE_DMA_WRITE_ERR_STATUS_OFF           0x5CU
#define PCIE_DMA_READ_ERR_STATUS_LOW_OFF        0xB4U
#define PCIE_DMA_READ_ERR_STATUS_HIGH_OFF       0xB8U

#define DMA_CH_CONTROL1_OFF_WRCH_0              0x200U
#define DMA_CH_CONTROL1_OFF_WRCH_1              0x400U
#define DMA_CH_CONTROL1_OFF_WRCH_2              0x600U
#define DMA_CH_CONTROL1_OFF_WRCH_3              0x800U
#define DMA_CH_CONTROL1_OFF_RDCH_0              0x300U
#define DMA_CH_CONTROL1_OFF_RDCH_1              0x500U
#define DMA_CH_CONTROL1_OFF_RDCH_2              0x700U
#define DMA_CH_CONTROL1_OFF_RDCH_3              0x900U

#define DMA_TRANSFER_SIZE_OFF_WRCH_0            0x208U
#define DMA_SAR_LOW_OFF_WRCH_0                  0x20CU
#define DMA_SAR_HIGH_OFF_WRCH_0                 0x210U
#define DMA_DAR_LOW_OFF_WRCH_0                  0x214U
#define DMA_DAR_HIGH_OFF_WRCH_0                 0x218U
#define DMA_WRITE_DOORBELL_OFF                  0x10U
#define DMA_WRITE_INT_STATUS_OFF                0x4CU
#define DMA_WRITE_INT_CLEAR_OFF                 0x58U

#define LOOPBACK_DMA_DOORBELL_CHAN_0          0U
#define LOOPBACK_DMA_INT_STATUS_CHAN_0          1U
#define LOOPBACK_DMA_INT_CLRAR_CHAN_0           1U

#define DMA_SRC_ADDR                0x100000000ULL
#define DMA_DST_ADDR                0x480000000ULL
#define PCIE_TEST_SIZE              0x40000U
#define STRESS_LOOP_REG_BASE_ADDR   0x00400204U
#define STRESS_TIME_REG_BASE_ADDR   0x00400200U

#define lower_32_bits(n)            ((uint32_t)((uint64_t)(n) & 0xffffffffULL))
#define upper_32_bits(n)            ((uint32_t)((uint64_t)(n) >> 32))

static inline uint32_t pci_readl(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void pci_writel(uint32_t val, uint32_t addr)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline uint8_t pci_readb(uint64_t addr)
{
    return *(volatile uint8_t *)(uintptr_t)addr;
}

static inline void pci_writeb(uint8_t val, uint64_t addr)
{
    *(volatile uint8_t *)(uintptr_t)addr = val;
}

static int drv_pcie_type_rc(void)
{
    return (pci_readl((uint32_t)(PCIE_AXI_DBI_ADDR + PCIE_HEADER_TYPE_ADDRESS_OFFSET))
            & PCIE_HEADER_TYPE_MASK) ? 1 : 0;
}

int drv_pcie_is_rc(void)
{
    return drv_pcie_type_rc();
}

static void pcie_dw_prog_outbound_atu_unroll(int index, uint64_t cpu_addr,
                                             uint64_t pci_addr, uint32_t size)
{
    uint32_t base = 0x1A180000U + (uint32_t)index * 0x200U;

    pci_writel(0x00, base);
    pci_writel(0x80000000U, base + 0x04U);
    pci_writel(lower_32_bits(cpu_addr), base + 0x08U);
    pci_writel(upper_32_bits(cpu_addr), base + 0x0CU);
    pci_writel(lower_32_bits(cpu_addr + size - 1U), base + 0x10U);
    pci_writel(lower_32_bits(pci_addr), base + 0x14U);
    pci_writel(upper_32_bits(pci_addr), base + 0x18U);
}

static void pcie_dw_prog_inbound_atu_unroll(int index, uint64_t pci_addr,
                                            uint64_t cpu_addr, uint32_t size)
{
    uint32_t base = 0x1A180100U + (uint32_t)index * 0x200U;

    pci_writel(0x00, base);
    pci_writel(0x80000000U, base + 0x04U);
    pci_writel(lower_32_bits(pci_addr), base + 0x08U);
    pci_writel(upper_32_bits(pci_addr), base + 0x0CU);
    pci_writel(lower_32_bits(pci_addr + size - 1U), base + 0x10U);
    pci_writel(lower_32_bits(cpu_addr), base + 0x14U);
    pci_writel(upper_32_bits(cpu_addr), base + 0x18U);
}

static int init_pcie_ep(void)
{
    pci_writel((pci_readl(0x1A000008U) & 0xFFFFU) | 0x12000000U, 0x1A000008U);
    pci_writel(0x02051E9FU, 0x1A00002CU);
    pci_writel(0x3FFFFFFFU, 0x1A080018U);

    pcie_dw_prog_inbound_atu_unroll(0, 0x440000000ULL, 0x1200000000ULL, 0x10000000U);
    pcie_dw_prog_inbound_atu_unroll(1, 0x450000000ULL, 0x1280000000ULL, 0x10000000U);
    pcie_dw_prog_inbound_atu_unroll(2, 0x460000000ULL, 0x1B00000000ULL, 0x10000000U);
    pcie_dw_prog_inbound_atu_unroll(3, 0x470000000ULL, 0x1B80000000ULL, 0x10000000U);
    pcie_dw_prog_inbound_atu_unroll(4, 0x480000000ULL, 0x04000000ULL, 0x10000000U);

    pcie_dw_prog_outbound_atu_unroll(0, 0x440000000ULL, 0x1200000000ULL, 0x10000000U);
    pcie_dw_prog_outbound_atu_unroll(1, 0x450000000ULL, 0x1280000000ULL, 0x10000000U);
    pcie_dw_prog_outbound_atu_unroll(2, 0x460000000ULL, 0x1B00000000ULL, 0x10000000U);
    pcie_dw_prog_outbound_atu_unroll(3, 0x470000000ULL, 0x1B80000000ULL, 0x10000000U);
    pcie_dw_prog_outbound_atu_unroll(4, 0x480000000ULL, 0x04000000ULL, 0x10000000U);

    pci_writel(pci_readl(0x1B000004U) | 0x10U, 0x1B000004U);
    pci_writel(0x01U, 0x1B000008U);
    return BIZ_SUCCESS;
}

static int init_pcie_rc(void)
{
    BIZ_INFO("PCIe RC initialization completed\n");
    return BIZ_SUCCESS;
}

/*
 * Wait for PCIe PLL lock (hp640 spl hp640_pll_init / lx_boot pll_init).
 * Do not re-toggle PLL if bootwrapper already locked it.
 */
static int pcie_wait_pll_locked(void)
{
    int timeout = 100000;

    if ((pci_readl(PLL_LOCK_STATUS) & 0xFFFFU) == PLL_LOCK_VALUE)
        return BIZ_SUCCESS;

    while ((pci_readl(PLL_LOCK_STATUS) & 0xFFFFU) != PLL_LOCK_VALUE)
    {
        rt_hw_us_delay(10);
        if (--timeout == 0)
        {
            BIZ_WARN("PCIe PLL lock timeout (status=0x%x)\n",
                     pci_readl(PLL_LOCK_STATUS));
            return BIZ_ERR_NORMAL;
        }
    }

    return BIZ_SUCCESS;
}

/*
 * Full PCIe PLL bring-up + reset (spl_cmd pcie_init).
 * Used by exec-task pcie setup / mode switch, not boot.
 */
static int pcie_init(void)
{
    int timeout;

    pci_writel(0x1U, 0x1B000230U);
    pci_writel(pci_readl(0x12000008U) | 0xCU, 0x12000008U);
    pci_writel(pci_readl(0x12000024U) | 0xC0U, 0x12000024U);
    pci_writel(pci_readl(CLOCK_SW_CTRL) | 0x13FU, CLOCK_SW_CTRL);
    pci_writel(pci_readl(BOOT_SELECT) | 0x20U, BOOT_SELECT);
    pci_writel(pci_readl(PCIE_PLL_CONFIG0) & 0xFFFFFFFEU, PCIE_PLL_CONFIG0);
    pci_writel(0x007D681U, PCIE_PLL_CONFIG2);
    pci_writel(pci_readl(PCIE_PLL_CONFIG0) | 0x1U, PCIE_PLL_CONFIG0);

    timeout = 100000;
    while (pci_readl(PLL_LOCK_STATUS) != PLL_LOCK_VALUE)
    {
        rt_hw_us_delay(10);
        if (--timeout == 0)
        {
            BIZ_WARN("PCIe PLL lock timeout (status=0x%x)\n",
                     pci_readl(PLL_LOCK_STATUS));
            return BIZ_ERR_NORMAL;
        }
    }

    pci_writel(pci_readl(CLOCK_SW_CTRL) & 0xFFFFFEC0U, CLOCK_SW_CTRL);
    pci_writel(pci_readl(BOOT_SELECT) & 0xFFFFFFDFU, BOOT_SELECT);
    pci_writel(pci_readl(PCIE_CTRL) | 0x2U, PCIE_CTRL);
    pci_writel(pci_readl(PCIE_CTRL) | 0x1U, PCIE_CTRL);
    pci_writel(pci_readl(0x1B000004U) & 0xFFFFFFEFU, 0x1B000004U);

    if (drv_pcie_type_rc())
    {
        BIZ_INFO("Initializing PCIe RC mode\n");
        rt_thread_mdelay(100);
        init_pcie_rc();
    }
    else
    {
        BIZ_INFO("Initializing PCIe EP mode\n");
        init_pcie_ep();
    }

    pci_writel(0x10107878U, 0x1A0008E8U);
    return BIZ_SUCCESS;
}

/*
 * Boot-time PCIe: link/EP setup only.
 * lx_boot.c pll_init may call pcie_dma_intr_init() for EP MSIX sync — not needed
 * for hp232x business firmware.  lxpci_non_linklist_dma_config() is deferred to
 * pcie stress / exec tasks only.
 */
static int pcie_boot_link_init(void)
{
    if (pcie_wait_pll_locked() != BIZ_SUCCESS)
        return BIZ_ERR_NORMAL;

    if (drv_pcie_type_rc())
    {
        BIZ_INFO("PCIe RC mode (boot)\n");
        rt_thread_mdelay(100);
        return init_pcie_rc();
    }

    BIZ_INFO("PCIe EP mode (boot)\n");
    return init_pcie_ep();
}

static int set_pcie_mode(enum HP640_PCIE_MODE mode)
{
    switch (mode)
    {
    case HP640_PCIE_MODE_RC:
        return pcie_init();
    case HP640_PCIE_MODE_EP:
        return init_pcie_ep();
    default:
        return BIZ_ERR_CLI_PARAM;
    }
}

void drv_pcie_boot_init(void)
{
    if (pcie_boot_link_init() != BIZ_SUCCESS)
        HP_LOGI("[drv] pcie boot link init skipped\n");
}

static void lxpci_non_linklist_dma_config(void)
{
    /* Deferred to pcie stress / exec only — not called at boot (cf. lx_boot pcie_dma_intr_init). */
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH0_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH1_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH2_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH3_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH0_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH1_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH2_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH3_PWR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_ENGINE_EN_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_DONE_IMWR_LOW_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_DONE_IMWR_HIGH_OFF);
    pci_writel(4, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_ABORT_IMWR_LOW_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_ABORT_IMWR_HIGH_OFF);
    pci_writel(0xff01ff00U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH01_IMWR_DATA_OFF);
    pci_writel(0xff03ff02U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CH23_IMWR_DATA_OFF);
    pci_writel(0x5678U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CHANNEL_ARB_WEIGHT_LOW_OFF);
    pci_writel(0x1234U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_CHANNEL_ARB_WEIGHT_HIGH_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_INT_MASK_OFF);
    pci_writel(0x00ff00ffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_LINKED_LIST_ERR_EN_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_INT_MASK_OFF);
    pci_writel(0x00ff00ffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_LINKED_LIST_ERR_EN_OFF);
    pci_writel(1, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_ENGINE_EN_OFF);
    pci_writel(8, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_DONE_IMWR_LOW_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_DONE_IMWR_HIGH_OFF);
    pci_writel(0xcU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_ABORT_IMWR_LOW_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_ABORT_IMWR_HIGH_OFF);
    pci_writel(0xff01ff00U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH01_IMWR_DATA_OFF);
    pci_writel(0xff03ff02U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CH23_IMWR_DATA_OFF);
    pci_writel(0x8765U, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_CHANNEL_ARB_WEIGHT_LOW_OFF);
    pci_writel(0x4321U, PCIE_DMA_CAP_BASE_OFF + PCIE_PCIE_DMA_READ_CHANNEL_ARB_WEIGHT_HIGH_OFF);
    pci_writel(0, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_INT_MASK_OFF);
    pci_writel(0x00ff00ffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_LINKED_LIST_ERR_EN_OFF);
    pci_writel(0x00ff00ffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_WRITE_ERR_STATUS_OFF);
    pci_writel(0x00ff00ffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_ERR_STATUS_LOW_OFF);
    pci_writel(0xffffffffU, PCIE_DMA_CAP_BASE_OFF + PCIE_DMA_READ_ERR_STATUS_HIGH_OFF);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_WRCH_0);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_WRCH_1);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_WRCH_2);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_WRCH_3);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_RDCH_0);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_RDCH_1);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_RDCH_2);
    pci_writel(0x18U, PCIE_DMA_CAP_BASE_OFF + DMA_CH_CONTROL1_OFF_RDCH_3);
}

static void lxpci_loopback_dma_init_data(uint32_t size)
{
    uint32_t i, j, count = size / 16U;
    uint8_t val, val2;

    for (i = 0; i < count; i++)
    {
        val = (i % 2U == 0U) ? 0x00U : 0xFFU;
        val2 = (i % 2U == 0U) ? 0xFFU : 0x00U;
        for (j = 0; j < 16U; j++)
        {
            pci_writeb(val, DMA_SRC_ADDR + (uint64_t)i * 16U + j);
            pci_writeb(val2, DMA_DST_ADDR + (uint64_t)i * 16U + j);
        }
    }
}

static int lxpci_loopback_dma_compare_data(uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size; i++)
    {
        uint8_t sval = pci_readb(DMA_SRC_ADDR + i);
        uint8_t dval = pci_readb(DMA_DST_ADDR + i);

        if (sval != dval)
        {
            BIZ_ERROR("PCIe DMA mismatch @0x%x src=0x%02x dst=0x%02x\n", i, sval, dval);
            return BIZ_ERR_NORMAL;
        }
    }
    return BIZ_SUCCESS;
}

static int lxpci_loopback_dma_wr_test_chan0(uint32_t size, unsigned long times,
                                            unsigned int *tcost)
{
    unsigned long step = 100UL;
    unsigned long cost_total = 0;
    int ret = BIZ_SUCCESS;
    uint32_t wait;

    if (times != 0 && times < step)
        step = times;

    for (unsigned long i = 0; i < step; i++)
    {
        rt_tick_t t0 = rt_tick_get();

        lxpci_loopback_dma_init_data(size);
        pci_writel(size, PCIE_DMA_CAP_BASE_OFF + DMA_TRANSFER_SIZE_OFF_WRCH_0);
        pci_writel(lower_32_bits(DMA_SRC_ADDR), PCIE_DMA_CAP_BASE_OFF + DMA_SAR_LOW_OFF_WRCH_0);
        pci_writel(upper_32_bits(DMA_SRC_ADDR), PCIE_DMA_CAP_BASE_OFF + DMA_SAR_HIGH_OFF_WRCH_0);
        pci_writel(lower_32_bits(DMA_DST_ADDR), PCIE_DMA_CAP_BASE_OFF + DMA_DAR_LOW_OFF_WRCH_0);
        pci_writel(upper_32_bits(DMA_DST_ADDR), PCIE_DMA_CAP_BASE_OFF + DMA_DAR_HIGH_OFF_WRCH_0);
        pci_writel(LOOPBACK_DMA_DOORBELL_CHAN_0, PCIE_DMA_CAP_BASE_OFF + DMA_WRITE_DOORBELL_OFF);

        wait = 0;
        while (pci_readl(PCIE_DMA_CAP_BASE_OFF + DMA_WRITE_INT_STATUS_OFF)
               != LOOPBACK_DMA_INT_STATUS_CHAN_0)
        {
            wait++;
            if (wait > 10000U)
            {
                BIZ_ERROR("PCIe DMA timeout\n");
                return BIZ_ERR_PRIM_TIMEOUT;
            }
            rt_hw_us_delay(1);
        }

        cost_total += (unsigned long)(rt_tick_get() - t0);
        pci_writel(LOOPBACK_DMA_INT_CLRAR_CHAN_0,
                   PCIE_DMA_CAP_BASE_OFF + DMA_WRITE_INT_CLEAR_OFF);
        rt_hw_us_delay(100);

        ret = lxpci_loopback_dma_compare_data(size);
        if (ret != BIZ_SUCCESS)
            return ret;
    }

    if (tcost)
        *tcost = (unsigned int)cost_total;

    return ret;
}

static int process_pcie_stress(unsigned int blk_cnt, unsigned int type,
                               unsigned long times, unsigned int *tcost)
{
    (void)type;
    lxpci_non_linklist_dma_config();
    return lxpci_loopback_dma_wr_test_chan0(blk_cnt, times, tcost);
}

int biz_exec_pcie_setup(const HP640_Task *task)
{
    (void)task;

    if (drv_pcie_type_rc())
        return pcie_init();

    return BIZ_SUCCESS;
}

int biz_exec_pcie_stress(const HP640_Task *task)
{
    unsigned int tcost = 0;
    unsigned int cnt = 0;
    unsigned char combined[8];
    uint32_t emmc_addr0, emmc_addr1;
    int ret;

    if (!task)
        return BIZ_ERR_CLI_PARAM;

    if (!drv_pcie_type_rc())
        return BIZ_SUCCESS;

    emmc_addr0 = STRESS_LOOP_REG_BASE_ADDR;
    emmc_addr0 |= (0x80U | (4U / 4U - 1U)) << 24;
    emmc_addr1 = STRESS_TIME_REG_BASE_ADDR;
    emmc_addr1 |= (0x80U | (8U / 4U - 1U)) << 24;

    if (task->times == 0)
    {
        while (1)
        {
            cnt = (cnt + 1U) & 0xFFFFU;
            ret = process_pcie_stress(task->blk_cnt ? task->blk_cnt : PCIE_TEST_SIZE,
                                      task->type, 100000UL, &tcost);
            if (ret != BIZ_SUCCESS)
                return ret;

            memcpy(combined, &tcost, 4);
            memcpy(combined + 4, &cnt, 4);
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, combined, sizeof(combined));
            ret = drv_emmc_write_blocks(emmc_addr1, combined, 1, BIZ_BLK_SIZE);
            if (ret != BIZ_SUCCESS)
            {
                biz_mcu_err_post((biz_err_code_t)ret);
                return ret;
            }
        }
    }

    ret = drv_emmc_write_blocks(emmc_addr0, (uint8_t *)&cnt, 1, BIZ_BLK_SIZE);
    if (ret != BIZ_SUCCESS)
    {
        biz_mcu_err_post((biz_err_code_t)ret);
        return ret;
    }

    ret = process_pcie_stress(task->blk_cnt ? task->blk_cnt : PCIE_TEST_SIZE,
                              task->type, task->times, &tcost);
    if (ret != BIZ_SUCCESS)
        return ret;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &tcost, sizeof(tcost));
    ret = drv_emmc_write_blocks(emmc_addr1, (uint8_t *)&tcost, 1, BIZ_BLK_SIZE);
    if (ret != BIZ_SUCCESS)
        biz_mcu_err_post((biz_err_code_t)ret);

    return ret;
}

int drv_pcie_set_mode(enum HP640_PCIE_MODE mode)
{
    return set_pcie_mode(mode);
}

int drv_pcie_dma_test(uint32_t test_size, unsigned long times)
{
    unsigned int cost = 0;
    int ret;

    if (!drv_pcie_type_rc())
        return BIZ_SUCCESS;

    if (times == 0)
    {
        while (1)
        {
            ret = process_pcie_stress(1, test_size, 0, &cost);
            if (ret != BIZ_SUCCESS)
                break;
        }
    }
    else
    {
        ret = process_pcie_stress(1, test_size, times, &cost);
    }

    BIZ_INFO("PCIe DMA test cost=%u ticks\n", cost);
    return ret;
}

#endif /* BIZ_MOD_PCIE */
