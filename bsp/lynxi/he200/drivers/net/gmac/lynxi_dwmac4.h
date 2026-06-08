/*
 * Lynxi KA200 DWMAC4 (DWC Ethernet v4.x) DMA glue for synopGMAC legacy driver.
 */
#ifndef LYNXI_DWMAC4_H
#define LYNXI_DWMAC4_H

#include "synopGMAC_Dev.h"

#define LYNXI_DWC_DMA_MODE          0x0000U
#define LYNXI_DWC_SYSBUS_REG        0x0004U
#define LYNXI_DWC_CH_TX_CTRL(n)     (0x0104U + (0x80U * (n)))
#define LYNXI_DWC_CH_RX_CTRL(n)     (0x0108U + (0x80U * (n)))
#define LYNXI_DWC_CH_TX_LIST_H(n)   (0x0110U + (0x80U * (n)))
#define LYNXI_DWC_CH_RX_LIST_H(n)   (0x0118U + (0x80U * (n)))
#define LYNXI_DWC_CH_TX_LIST(n)     (0x0114U + (0x80U * (n)))
#define LYNXI_DWC_CH_RX_LIST(n)     (0x011CU + (0x80U * (n)))
#define LYNXI_DWC_CH_TX_RING(n)     (0x012CU + (0x80U * (n)))
#define LYNXI_DWC_CH_RX_RING(n)     (0x0130U + (0x80U * (n)))
#define LYNXI_DWC_CH_IRQ_EN(n)      (0x0134U + (0x80U * (n)))
#define LYNXI_DWC_CH_RX_TAIL(n)     (0x0128U + (0x80U * (n)))
#define LYNXI_DWC_CH_TX_TAIL(n)     (0x0120U + (0x80U * (n)))
#define LYNXI_DWC_CH_STATUS(n)      (0x0160U + (0x80U * (n)))

#define LYNXI_DWC_SYSBUS_AAL        0x00001000U
#define LYNXI_DWC_SYSBUS_EAME       0x00000800U
#define LYNXI_DWC_SYSBUS_FB         0x00000001U
#define LYNXI_DWC_SYSBUS_AAL_FB     (LYNXI_DWC_SYSBUS_AAL | LYNXI_DWC_SYSBUS_FB)
#define LYNXI_DWC_SYSBUS_INIT       (LYNXI_DWC_SYSBUS_AAL | LYNXI_DWC_SYSBUS_EAME | LYNXI_DWC_SYSBUS_FB)
#define LYNXI_DWC_TX_ST             0x00000001U
#define LYNXI_DWC_RX_SR             0x00000001U
#define LYNXI_DWC_CH_IRQ_RIE        0x00000040U
#define LYNXI_DWC_CH_IRQ_TIE        0x00000001U
#define LYNXI_DWC_CH_IRQ_NIE        0x00008000U
#define LYNXI_DWC_CH_IRQ_AIE        0x00004000U
#define LYNXI_DWC_CH_STS_RI         0x00000040U
#define LYNXI_DWC_CH_STS_TI         0x00000001U
#define LYNXI_DWC_CH_STS_RBU        0x00000080U
#define LYNXI_DWC_CH_STS_RPS        0x00000100U
#define LYNXI_DWC_CH_STS_FBE        0x00001000U
#define LYNXI_DWC_CH_STS_AIS        0x00004000U

#define LYNXI_DWC_TDES2_IOC         0x80000000U
#define LYNXI_DWC_RDES3_BUF1V       0x01000000U
#define LYNXI_DWC_RDES3_IOC         0x40000000U
/* DWMAC4 TDES3/RDES3 位布局（勿用 legacy DescTxFirst/Last，enh/normal 枚举位不同） */
#define LYNXI_DWC_TDES3_FD          0x20000000U
#define LYNXI_DWC_TDES3_LD          0x10000000U
#define LYNXI_DWC_RDES3_FD          0x20000000U
#define LYNXI_DWC_RDES3_LD          0x10000000U

/* DWMAC4 MAC register offsets (differs from legacy synopGMAC) */
#define LYNXI_MAC_CONF              0x0000U
#define LYNXI_MAC_PKT_FILTER        0x0008U
#define LYNXI_MAC_ADDR_HIGH(n)      (0x0300U + 8U * (n))
#define LYNXI_MAC_ADDR_LOW(n)       (0x0304U + 8U * (n))
#define LYNXI_MAC_ADDR_HIGH_AE      0x80000000U
#define LYNXI_MAC_CONF_RE           0x00000001U
#define LYNXI_MAC_CONF_TE           0x00000002U
#define LYNXI_MAC_CONF_DM           0x00002000U
#define LYNXI_MAC_CONF_PS           0x00008000U
#define LYNXI_MAC_CONF_FES          0x00004000U
#define LYNXI_MAC_CONF_CST          0x00200000U
#define LYNXI_MAC_PKT_FILTER_PR     0x00000001U
#define LYNXI_MAC_PKT_FILTER_PM     0x00000010U
#define LYNXI_MAC_VERSION           0x0110U

/* MAC RX/TX queue（DWMAC4 须在 MAC_RXQ_CTRL0 使能 RXQ0，否则 MTL/DMA 不收包） */
#define LYNXI_MAC_RXQ_CTRL0         0x00a0U
#define LYNXI_MAC_RXQ0_DCB_EN       0x00000002U  /* queue0 bit1: DCB enabled */

/* MTL（相对 MacBase，与 Linux dwmac4.h / Zephyr eth_dwmac_priv.h 一致） */
#define LYNXI_MTL_RXQ_DMA_MAP0      0x0c30U
#define LYNXI_MTL_CHAN0_TX_OP       0x0d00U
#define LYNXI_MTL_CHAN0_RX_OP       0x0d30U
#define LYNXI_MTL_OP_TSF            0x00000002U  /* bit1: TX store-and-forward */
#define LYNXI_MTL_OP_TXQEN          0x00000008U  /* bit3: enable TXQ */
#define LYNXI_MTL_OP_RSF            0x00000020U  /* bit5: RX store-and-forward */

#define LYNXI_DWC_DES0(d)           ((d)->des0)
#define LYNXI_DWC_DES1(d)           ((d)->des1)
#define LYNXI_DWC_DES2(d)           ((d)->des2)
#define LYNXI_DWC_DES3(d)           ((d)->des3)

void lynxi_dwmac4_desc_flush(LynxiHwDesc *desc);

void lynxi_dwmac4_dma_ring_init(synopGMACdevice *gmacdev);
void lynxi_dwmac4_dma_start(synopGMACdevice *gmacdev);
void lynxi_dwmac4_dma_stop(synopGMACdevice *gmacdev);
void lynxi_dwmac4_enable_interrupt(synopGMACdevice *gmacdev);
void lynxi_dwmac4_disable_interrupt_all(synopGMACdevice *gmacdev);
u32 lynxi_dwmac4_read_clear_irq(synopGMACdevice *gmacdev);
u32 lynxi_dwmac4_get_interrupt_type(synopGMACdevice *gmacdev);
void lynxi_dwmac4_tx_tail_update(synopGMACdevice *gmacdev, u32 index);
void lynxi_dwmac4_rx_tail_update(synopGMACdevice *gmacdev, u32 index);

void lynxi_dwmac4_tx_desc_init(LynxiHwDesc *desc);
void lynxi_dwmac4_rx_desc_init(LynxiHwDesc *desc);
void lynxi_dwmac4_tx_submit(synopGMACdevice *g, u32 idx, u32 buf, u32 len, rt_ubase_t va);
void lynxi_dwmac4_rx_submit(synopGMACdevice *g, u32 idx, u32 buf, rt_ubase_t va);
void lynxi_dwmac4_tx_read(synopGMACdevice *g, u32 idx, u32 *buf, u32 *len, u32 *flags, rt_ubase_t *va);
void lynxi_dwmac4_rx_read(synopGMACdevice *g, u32 idx, u32 *buf, u32 *flags, rt_ubase_t *va);

rt_bool_t lynxi_dwmac4_tx_owned(synopGMACdevice *g, u32 idx);
rt_bool_t lynxi_dwmac4_tx_empty(synopGMACdevice *g, u32 idx);
u32 lynxi_dwmac4_tx_in_use(synopGMACdevice *gmacdev);
rt_bool_t lynxi_dwmac4_rx_owned(synopGMACdevice *g, u32 idx);
rt_bool_t lynxi_dwmac4_rx_empty(synopGMACdevice *g, u32 idx);
void lynxi_dwmac4_take_ownership(synopGMACdevice *g, u32 idx);

u32 lynxi_dwmac4_rx_frame_length(u32 flags);
rt_bool_t lynxi_dwmac4_is_rx_valid(u32 flags);
rt_bool_t lynxi_dwmac4_is_tx_valid(u32 flags);
rt_bool_t lynxi_dwmac4_is_last_index(u32 idx, u32 count);

void lynxi_dwmac4_mac_apply(synopGMACdevice *gmacdev);
s32 lynxi_dwmac4_set_mac_addr(synopGMACdevice *gmacdev, u8 *mac_addr);
rt_bool_t lynxi_dwmac4_rx_pending(synopGMACdevice *gmacdev);
u32 lynxi_dwmac4_map_irq_status(u32 ch_status);
void lynxi_dwmac4_regs_dump(synopGMACdevice *gmacdev);
void *lynxi_dwmac4_alloc_nc(u32 size, rt_uint64_t *dma);
rt_bool_t lynxi_dwmac4_buf_is_nc(const void *ptr);
void lynxi_dwmac4_set_ring_pa(rt_bool_t tx, rt_uint64_t pa);
void lynxi_dwmac4_rx_restart(synopGMACdevice *gmacdev);
void lynxi_dwmac4_link_up_refresh(synopGMACdevice *gmacdev);
void lynxi_dwmac4_tx_reclaim_all(synopGMACdevice *gmacdev);
void lynxi_dwmac4_rx_watchdog(synopGMACdevice *gmacdev);

rt_bool_t lynxi_dwmac4_swr_succeeded(void);
s32 lynxi_dwmac4_early_bus_reset(u32 mac_base, u32 dma_base);

#endif
