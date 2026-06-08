/*
 * Lynxi KA200 DWMAC4 DMA register + 16-byte HW descriptor ring glue.
 */
#include <rtthread.h>
#include <rthw.h>
#include <mmu.h>
#include "board.h"
#include "lynxi_dwmac4.h"

static inline u32 lynxi_dwmac4_pa_lo(rt_uint64_t pa)
{
    return (u32)(pa & 0xFFFFFFFFU);
}

static inline u32 lynxi_dwmac4_pa_hi(rt_uint64_t pa)
{
    return (u32)(pa >> 32);
}

static rt_uint64_t lynxi_dwmac4_va_to_pa(void *va)
{
    void *pa;

    if (va == RT_NULL)
        return 0U;

    pa = rt_kmem_v2p(va);
    if (pa == RT_NULL || pa == ARCH_MAP_FAILED)
        return 0U;

    return (rt_uint64_t)(rt_ubase_t)pa;
}

/* Nocache DDR window (identity-mapped); offset past rpmsg-lite shmem */
#define LYNXI_DWMAC4_NC_BASE    ((rt_uintptr_t)(MEM_PADDR_START + MEM_CACHE_SZ + 0x00200000ULL))
static rt_uintptr_t lynxi_dwmac4_nc_cursor = LYNXI_DWMAC4_NC_BASE;
static u32 lynxi_dwmac4_tx_ring_hi;
static u32 lynxi_dwmac4_rx_ring_hi;

void *lynxi_dwmac4_alloc_nc(u32 size, rt_uint64_t *dma)
{
    rt_uintptr_t aligned = (lynxi_dwmac4_nc_cursor + 63U) & ~(rt_uintptr_t)63U;

    if (dma == RT_NULL)
        return RT_NULL;

    *dma = (rt_uint64_t)aligned;
    lynxi_dwmac4_nc_cursor = aligned + size;
    rt_memset((void *)aligned, 0, size);
    return (void *)aligned;
}

void lynxi_dwmac4_set_ring_pa(rt_bool_t tx, rt_uint64_t pa)
{
    if (tx)
        lynxi_dwmac4_tx_ring_hi = lynxi_dwmac4_pa_hi(pa);
    else
        lynxi_dwmac4_rx_ring_hi = lynxi_dwmac4_pa_hi(pa);
}

static inline LynxiHwDesc *hw_tx(synopGMACdevice *g, u32 idx)
{
    return &g->TxHwRing[idx];
}

static inline LynxiHwDesc *hw_rx(synopGMACdevice *g, u32 idx)
{
    return &g->RxHwRing[idx];
}

static inline u32 dwc_readl(synopGMACdevice *g, u32 off)
{
    return synopGMACReadReg(g->DmaBase, off);
}

static inline void dwc_writel(synopGMACdevice *g, u32 off, u32 val)
{
    synopGMACWriteReg(g->DmaBase, off, val);
}

rt_bool_t lynxi_dwmac4_buf_is_nc(const void *ptr)
{
    rt_ubase_t addr = (rt_ubase_t)ptr;

    return addr >= (rt_ubase_t)(MEM_PADDR_START + MEM_CACHE_SZ);
}

void lynxi_dwmac4_desc_flush(LynxiHwDesc *desc)
{
    if (desc != RT_NULL && !lynxi_dwmac4_buf_is_nc(desc))
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, desc, (int)sizeof(LynxiHwDesc));
    __asm__ volatile ("dmb st" ::: "memory");
}

static void lynxi_dwmac4_desc_invalidate(LynxiHwDesc *desc)
{
    if (desc != RT_NULL && !lynxi_dwmac4_buf_is_nc(desc))
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, desc, (int)sizeof(LynxiHwDesc));
    __asm__ volatile ("dmb ld" ::: "memory");
}

void lynxi_dwmac4_tx_desc_init(LynxiHwDesc *desc)
{
    LYNXI_DWC_DES0(desc) = 0U;
    LYNXI_DWC_DES1(desc) = 0U;
    LYNXI_DWC_DES2(desc) = 0U;
    LYNXI_DWC_DES3(desc) = 0U;
    lynxi_dwmac4_desc_flush(desc);
}

void lynxi_dwmac4_rx_desc_init(LynxiHwDesc *desc)
{
    lynxi_dwmac4_tx_desc_init(desc);
}

void lynxi_dwmac4_tx_submit(synopGMACdevice *g, u32 idx, u32 buf, u32 len, rt_ubase_t va)
{
    LynxiHwDesc *desc = hw_tx(g, idx);
    rt_uint64_t pa;

    if (va != 0 && lynxi_dwmac4_buf_is_nc((void *)va))
        pa = (rt_uint64_t)va;
    else if (va != 0)
        pa = lynxi_dwmac4_va_to_pa((void *)va);
    else
        pa = (rt_uint64_t)buf;

    LYNXI_DWC_DES0(desc) = lynxi_dwmac4_pa_lo(pa);
    LYNXI_DWC_DES1(desc) = lynxi_dwmac4_pa_hi(pa);
    LYNXI_DWC_DES2(desc) = len | LYNXI_DWC_TDES2_IOC;
    /* DES3[14:0] 为整帧长度（与 Zephyr eth_dwmac pkt_len|TDES3_* 一致） */
    LYNXI_DWC_DES3(desc) = DescOwnByDma | LYNXI_DWC_TDES3_FD | LYNXI_DWC_TDES3_LD | (len & 0x7FFFU);
    if (g->TxBufVa)
        g->TxBufVa[idx] = va;
    lynxi_dwmac4_desc_flush(desc);
}

void lynxi_dwmac4_rx_submit(synopGMACdevice *g, u32 idx, u32 buf, rt_ubase_t va)
{
    LynxiHwDesc *desc = hw_rx(g, idx);
    rt_uint64_t pa = va ? lynxi_dwmac4_va_to_pa((void *)va) : (rt_uint64_t)buf;

    LYNXI_DWC_DES0(desc) = lynxi_dwmac4_pa_lo(pa);
    LYNXI_DWC_DES1(desc) = lynxi_dwmac4_pa_hi(pa);
    LYNXI_DWC_DES2(desc) = 0U;
    LYNXI_DWC_DES3(desc) = DescOwnByDma | LYNXI_DWC_RDES3_BUF1V | LYNXI_DWC_RDES3_IOC;
    if (g->RxBufVa)
        g->RxBufVa[idx] = va;
    lynxi_dwmac4_desc_flush(desc);
}

void lynxi_dwmac4_tx_read(synopGMACdevice *g, u32 idx, u32 *buf, u32 *len, u32 *flags, rt_ubase_t *va)
{
    LynxiHwDesc *desc = hw_tx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    if (buf)
        *buf = LYNXI_DWC_DES0(desc);
    if (len)
        *len = LYNXI_DWC_DES2(desc) & 0x3FFFU;
    if (flags)
        *flags = LYNXI_DWC_DES3(desc);
    if (va && g->TxBufVa)
        *va = g->TxBufVa[idx];
}

void lynxi_dwmac4_rx_read(synopGMACdevice *g, u32 idx, u32 *buf, u32 *flags, rt_ubase_t *va)
{
    LynxiHwDesc *desc = hw_rx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    if (buf)
        *buf = LYNXI_DWC_DES0(desc);
    if (flags)
        *flags = LYNXI_DWC_DES3(desc);
    if (va && g->RxBufVa)
        *va = g->RxBufVa[idx];
}

rt_bool_t lynxi_dwmac4_tx_owned(synopGMACdevice *g, u32 idx)
{
    LynxiHwDesc *desc = hw_tx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    return (LYNXI_DWC_DES3(desc) & DescOwnByDma) != 0U;
}

rt_bool_t lynxi_dwmac4_tx_empty(synopGMACdevice *g, u32 idx)
{
    LynxiHwDesc *desc = hw_tx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    return !lynxi_dwmac4_tx_owned(g, idx) &&
           LYNXI_DWC_DES0(desc) == 0U &&
           LYNXI_DWC_DES2(desc) == 0U;
}

u32 lynxi_dwmac4_tx_in_use(synopGMACdevice *gmacdev)
{
    u32 i, n = 0U;

    if (gmacdev == RT_NULL)
        return 0U;

    for (i = 0; i < TRANSMIT_DESC_SIZE; i++)
    {
        if (!lynxi_dwmac4_tx_empty(gmacdev, i))
            n++;
    }
    return n;
}

rt_bool_t lynxi_dwmac4_rx_owned(synopGMACdevice *g, u32 idx)
{
    LynxiHwDesc *desc = hw_rx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    return (LYNXI_DWC_DES3(desc) & DescOwnByDma) != 0U;
}

rt_bool_t lynxi_dwmac4_rx_empty(synopGMACdevice *g, u32 idx)
{
    LynxiHwDesc *desc = hw_rx(g, idx);

    lynxi_dwmac4_desc_invalidate(desc);
    return !lynxi_dwmac4_rx_owned(g, idx) &&
           LYNXI_DWC_DES0(desc) == 0U &&
           (LYNXI_DWC_DES3(desc) & ~DescOwnByDma) == 0U;
}

void lynxi_dwmac4_take_ownership(synopGMACdevice *g, u32 idx)
{
    LynxiHwDesc *td = hw_tx(g, idx);
    LynxiHwDesc *rd = hw_rx(g, idx);

    LYNXI_DWC_DES3(td) &= ~DescOwnByDma;
    LYNXI_DWC_DES3(rd) &= ~DescOwnByDma;
    lynxi_dwmac4_desc_flush(td);
    lynxi_dwmac4_desc_flush(rd);
}

u32 lynxi_dwmac4_rx_frame_length(u32 flags)
{
    return flags & 0x7FFFU;
}

rt_bool_t lynxi_dwmac4_is_rx_valid(u32 flags)
{
    return ((flags & DescError) == 0U) &&
           ((flags & LYNXI_DWC_RDES3_FD) != 0U) &&
           ((flags & LYNXI_DWC_RDES3_LD) != 0U);
}

rt_bool_t lynxi_dwmac4_is_tx_valid(u32 flags)
{
    return (flags & 0x00008000U) == 0U;
}

rt_bool_t lynxi_dwmac4_is_last_index(u32 idx, u32 count)
{
    return idx == (count - 1U);
}

static void lynxi_dwmac4_mtl_init(synopGMACdevice *gmacdev)
{
    u32 tx_op;
    u32 rx_op;

    if (gmacdev == RT_NULL)
        return;

    /* RX queue 0 -> DMA channel 0；MAC 侧使能 RXQ0（对齐 Linux stmmac_rx_queue_enable DCB） */
    {
        u32 rxq0 = synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_RXQ_CTRL0);

        rxq0 = (rxq0 & ~0x3U) | LYNXI_MAC_RXQ0_DCB_EN;
        synopGMACWriteReg(gmacdev->MacBase, LYNXI_MAC_RXQ_CTRL0, rxq0);
    }
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MTL_RXQ_DMA_MAP0, 0U);

    tx_op = synopGMACReadReg(gmacdev->MacBase, LYNXI_MTL_CHAN0_TX_OP);
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MTL_CHAN0_TX_OP,
                       tx_op | LYNXI_MTL_OP_TXQEN | LYNXI_MTL_OP_TSF);

    rx_op = synopGMACReadReg(gmacdev->MacBase, LYNXI_MTL_CHAN0_RX_OP);
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MTL_CHAN0_RX_OP,
                       rx_op | LYNXI_MTL_OP_RSF);
}

void lynxi_dwmac4_dma_ring_init(synopGMACdevice *gmacdev)
{
    u32 rx_pbl = (32U << 16);
    u32 rx_rbsz = ((RX_BUF_SIZE & 0x7FFFU) << 1);

    lynxi_dwmac4_mtl_init(gmacdev);
    dwc_writel(gmacdev, LYNXI_DWC_SYSBUS_REG, LYNXI_DWC_SYSBUS_INIT);

    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_CTRL(0), (32U << 16));
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_CTRL(0), rx_pbl | rx_rbsz);

    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_LIST_H(0), lynxi_dwmac4_tx_ring_hi);
    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_LIST(0), (u32)gmacdev->TxHwDma);
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_LIST_H(0), lynxi_dwmac4_rx_ring_hi);
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_LIST(0), (u32)gmacdev->RxHwDma);
    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_RING(0), TRANSMIT_DESC_SIZE - 1U);
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_RING(0), RECEIVE_DESC_SIZE - 1U);

    lynxi_dwmac4_enable_interrupt(gmacdev);

    rt_kprintf("gmac: dwmac4 rings tx=%u rx=%u list=%08x%08x/%08x%08x\n",
               TRANSMIT_DESC_SIZE, RECEIVE_DESC_SIZE,
               lynxi_dwmac4_tx_ring_hi, (u32)gmacdev->TxHwDma,
               lynxi_dwmac4_rx_ring_hi, (u32)gmacdev->RxHwDma);
}

void lynxi_dwmac4_dma_start(synopGMACdevice *gmacdev)
{
    u32 tx = dwc_readl(gmacdev, LYNXI_DWC_CH_TX_CTRL(0));
    u32 rx = dwc_readl(gmacdev, LYNXI_DWC_CH_RX_CTRL(0));

    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_CTRL(0), tx | LYNXI_DWC_TX_ST);
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_CTRL(0), rx | LYNXI_DWC_RX_SR);
}

void lynxi_dwmac4_dma_stop(synopGMACdevice *gmacdev)
{
    u32 tx = dwc_readl(gmacdev, LYNXI_DWC_CH_TX_CTRL(0));
    u32 rx = dwc_readl(gmacdev, LYNXI_DWC_CH_RX_CTRL(0));

    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_CTRL(0), tx & ~LYNXI_DWC_TX_ST);
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_CTRL(0), rx & ~LYNXI_DWC_RX_SR);
}

void lynxi_dwmac4_enable_interrupt(synopGMACdevice *gmacdev)
{
    dwc_writel(gmacdev, LYNXI_DWC_CH_IRQ_EN(0),
               LYNXI_DWC_CH_IRQ_NIE | LYNXI_DWC_CH_IRQ_AIE |
               LYNXI_DWC_CH_IRQ_RIE | LYNXI_DWC_CH_IRQ_TIE);
}

void lynxi_dwmac4_disable_interrupt_all(synopGMACdevice *gmacdev)
{
    dwc_writel(gmacdev, LYNXI_DWC_CH_IRQ_EN(0), 0U);
}

u32 lynxi_dwmac4_read_clear_irq(synopGMACdevice *gmacdev)
{
    u32 st = dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0));
    dwc_writel(gmacdev, LYNXI_DWC_CH_STATUS(0), st);
    return st;
}

u32 lynxi_dwmac4_get_interrupt_type(synopGMACdevice *gmacdev)
{
    u32 st = dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0));
    u32 mapped = 0;

    dwc_writel(gmacdev, LYNXI_DWC_CH_STATUS(0), st);

    if (st & LYNXI_DWC_CH_STS_AIS)
        mapped |= synopGMACDmaError;
    if (st & LYNXI_DWC_CH_STS_RI)
        mapped |= synopGMACDmaRxNormal;
    if (st & LYNXI_DWC_CH_STS_TI)
        mapped |= synopGMACDmaTxNormal;

    return mapped;
}

void lynxi_dwmac4_rx_tail_update(synopGMACdevice *gmacdev, u32 index)
{
    u32 tail = (u32)gmacdev->RxHwDma + index * (u32)sizeof(LynxiHwDesc);

    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_TAIL(0), tail);
}

void lynxi_dwmac4_tx_tail_update(synopGMACdevice *gmacdev, u32 index)
{
    u32 tail = (u32)gmacdev->TxHwDma + index * (u32)sizeof(LynxiHwDesc);

    dwc_writel(gmacdev, LYNXI_DWC_CH_TX_TAIL(0), tail);
}

void lynxi_dwmac4_mac_apply(synopGMACdevice *gmacdev)
{
    u32 conf;

    if (gmacdev == RT_NULL)
        return;

    /* DMA SWR / eth_init reset clears MAC_ADDRESS0; reprogram each link/mac_init */
    lynxi_dwmac4_set_mac_addr(gmacdev, gmacdev->MacAddr);

    conf = synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_CONF);
    conf |= LYNXI_MAC_CONF_RE | LYNXI_MAC_CONF_TE | LYNXI_MAC_CONF_CST;

    if (gmacdev->DuplexMode == FULLDUPLEX)
        conf |= LYNXI_MAC_CONF_DM;
    else
        conf &= ~LYNXI_MAC_CONF_DM;

    conf &= ~(LYNXI_MAC_CONF_PS | LYNXI_MAC_CONF_FES);
    if (gmacdev->Speed == SPEED100)
        conf |= LYNXI_MAC_CONF_PS | LYNXI_MAC_CONF_FES;
    else if (gmacdev->Speed == SPEED10)
        conf |= LYNXI_MAC_CONF_PS;

    /* legacy synopGMAC 曾写 0x0004（DWMAC4 为 MAC_EXT_CFG），显式清零 */
    synopGMACWriteReg(gmacdev->MacBase, 0x0004U, 0U);
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MAC_CONF, conf);
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MAC_PKT_FILTER, 0U);
}

s32 lynxi_dwmac4_set_mac_addr(synopGMACdevice *gmacdev, u8 *mac_addr)
{
    u32 high;
    u32 low;

    if (gmacdev == RT_NULL || mac_addr == RT_NULL)
        return -1;

    high = ((u32)mac_addr[5] << 8) | mac_addr[4];
    low = ((u32)mac_addr[3] << 24) | ((u32)mac_addr[2] << 16) |
          ((u32)mac_addr[1] << 8) | mac_addr[0];
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MAC_ADDR_HIGH(0), high | LYNXI_MAC_ADDR_HIGH_AE);
    synopGMACWriteReg(gmacdev->MacBase, LYNXI_MAC_ADDR_LOW(0), low);
    return 0;
}

void lynxi_dwmac4_tx_reclaim_all(synopGMACdevice *gmacdev)
{
    u32 i;

    if (gmacdev == RT_NULL)
        return;

    for (i = 0; i < TRANSMIT_DESC_SIZE; i++)
    {
        if (lynxi_dwmac4_tx_owned(gmacdev, i))
            continue;
        if (lynxi_dwmac4_tx_empty(gmacdev, i))
            continue;

        if (gmacdev->BusyTxDesc > 0)
            gmacdev->BusyTxDesc--;
        lynxi_dwmac4_tx_desc_init(&gmacdev->TxHwRing[i]);
    }
}

rt_bool_t lynxi_dwmac4_rx_pending(synopGMACdevice *gmacdev)
{
    u32 i;

    if (gmacdev == RT_NULL)
        return RT_FALSE;

    for (i = 0; i < RECEIVE_DESC_SIZE; i++)
    {
        u32 buf = 0;
        u32 flags = 0;

        if (lynxi_dwmac4_rx_owned(gmacdev, i))
            continue;

        lynxi_dwmac4_rx_read(gmacdev, i, &buf, &flags, RT_NULL);
        if ((flags & DescOwnByDma) != 0U)
            continue;
        /* 完成帧可能 DES0 已清，仅 des3 置 FD|LD */
        if ((flags & (LYNXI_DWC_RDES3_FD | LYNXI_DWC_RDES3_LD)) != 0U)
            return RT_TRUE;
        if (buf != 0U && (flags & ~DescOwnByDma) != 0U)
            return RT_TRUE;
    }
    return RT_FALSE;
}

void lynxi_dwmac4_rx_watchdog(synopGMACdevice *gmacdev)
{
    static u32 last_ch_st;
    static rt_uint32_t tick;
    u32 i;
    u32 ch_st;

    if (gmacdev == RT_NULL || !gmacdev->LinkState)
        return;

    tick++;
    if ((tick % 50U) != 0U)
        return;

    ch_st = dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0));
    if (ch_st != last_ch_st)
    {
        rt_kprintf("gmac: CH_STATUS %08x -> %08x\n", last_ch_st, ch_st);
        last_ch_st = ch_st;
    }
    if (ch_st & (LYNXI_DWC_CH_STS_RPS | LYNXI_DWC_CH_STS_RBU | LYNXI_DWC_CH_STS_FBE))
    {
        static rt_uint32_t last_restart_tick;

        if ((tick - last_restart_tick) >= 50U)
        {
            rt_kprintf("gmac: RX halt st=%08x, restart\n", ch_st);
            lynxi_dwmac4_rx_restart(gmacdev);
            last_restart_tick = tick;
        }
    }

    for (i = 0; i < RECEIVE_DESC_SIZE; i++)
    {
        if (!lynxi_dwmac4_rx_owned(gmacdev, i))
        {
            u32 flags = 0;

            lynxi_dwmac4_rx_read(gmacdev, i, RT_NULL, &flags, RT_NULL);
            if ((flags & (LYNXI_DWC_RDES3_FD | LYNXI_DWC_RDES3_LD)) != 0U)
            {
                static rt_uint32_t rx_log_tick;

                if ((tick - rx_log_tick) >= 50U)
                {
                    rt_kprintf("gmac: rx[%u] des3=%08x ready\n", i, flags);
                    rx_log_tick = tick;
                }
                return;
            }
        }
    }
}

u32 lynxi_dwmac4_map_irq_status(u32 ch_status)
{
    u32 mapped = 0;

    if (ch_status & LYNXI_DWC_CH_STS_AIS)
        mapped |= synopGMACDmaError;
    if (ch_status & LYNXI_DWC_CH_STS_RI)
        mapped |= synopGMACDmaRxNormal;
    if (ch_status & LYNXI_DWC_CH_STS_TI)
        mapped |= synopGMACDmaTxNormal;
    return mapped;
}

void lynxi_dwmac4_rx_restart(synopGMACdevice *gmacdev)
{
    u32 st;
    u32 rx;

    if (gmacdev == RT_NULL)
        return;

    rx = dwc_readl(gmacdev, LYNXI_DWC_CH_RX_CTRL(0));
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_CTRL(0), rx & ~LYNXI_DWC_RX_SR);

    st = dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0));
    dwc_writel(gmacdev, LYNXI_DWC_CH_STATUS(0), st);

    if (gmacdev->BusyRxDesc > 0U)
        lynxi_dwmac4_rx_tail_update(gmacdev, gmacdev->RxTailIdx);

    rx = dwc_readl(gmacdev, LYNXI_DWC_CH_RX_CTRL(0));
    dwc_writel(gmacdev, LYNXI_DWC_CH_RX_CTRL(0), rx | LYNXI_DWC_RX_SR);

    rt_kprintf("gmac: rx_restart tail=%u busy=%u st=%08x\n",
               gmacdev->RxTailIdx, gmacdev->BusyRxDesc,
               dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0)));
}

void lynxi_dwmac4_link_up_refresh(synopGMACdevice *gmacdev)
{
    if (gmacdev == RT_NULL)
        return;

    lynxi_dwmac4_rx_restart(gmacdev);
    lynxi_dwmac4_dma_start(gmacdev);
}

void lynxi_dwmac4_regs_dump(synopGMACdevice *gmacdev)
{
    u32 rx0_des3 = 0;

    if (gmacdev == RT_NULL)
        return;

    if (gmacdev->RxHwRing)
    {
        LynxiHwDesc *d = &gmacdev->RxHwRing[0];

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, d, (int)sizeof(LynxiHwDesc));
        rx0_des3 = LYNXI_DWC_DES3(d);
        rt_kprintf("gmac dwmac4: RxRing[0] des0=%08x des1=%08x des3=%08x\n",
                   LYNXI_DWC_DES0(d), LYNXI_DWC_DES1(d), rx0_des3);
    }

    rt_kprintf("gmac dwmac4: SYSBUS=%08x MAC_CONF=%08x PKT_FILTER=%08x CH_RX_CTRL=%08x CH_STATUS=%08x\n",
               dwc_readl(gmacdev, LYNXI_DWC_SYSBUS_REG),
               synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_CONF),
               synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_PKT_FILTER),
               dwc_readl(gmacdev, LYNXI_DWC_CH_RX_CTRL(0)),
               dwc_readl(gmacdev, LYNXI_DWC_CH_STATUS(0)));
    rt_kprintf("gmac dwmac4: ADDR0=%08x/%08x RxRing[0].des3=%08x\n",
               synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_ADDR_HIGH(0)),
               synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_ADDR_LOW(0)),
               rx0_des3);
    rt_kprintf("gmac dwmac4: hw ring pa tx=%08x%08x rx=%08x%08x tail=%u busy=%u\n",
               lynxi_dwmac4_tx_ring_hi, (u32)gmacdev->TxHwDma,
               lynxi_dwmac4_rx_ring_hi, (u32)gmacdev->RxHwDma,
               gmacdev->RxTailIdx, gmacdev->BusyRxDesc);
    rt_kprintf("gmac dwmac4: RXQ_CTRL0=%08x CH_RX_LIST=%08x CH_TX_LIST=%08x CH_RX_TAIL=%08x CH_TX_TAIL=%08x\n",
               synopGMACReadReg(gmacdev->MacBase, LYNXI_MAC_RXQ_CTRL0),
               dwc_readl(gmacdev, LYNXI_DWC_CH_RX_LIST(0)),
               dwc_readl(gmacdev, LYNXI_DWC_CH_TX_LIST(0)),
               dwc_readl(gmacdev, LYNXI_DWC_CH_RX_TAIL(0)),
               dwc_readl(gmacdev, LYNXI_DWC_CH_TX_TAIL(0)));
}
