/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017-08-24     chinesebear  first version
 */


#include <rtthread.h>
#include <rtdef.h>
#include <rthw.h>
#include <drivers/dev_pin.h>
#include <string.h>
#include <netif/ethernetif.h>
#include <lwip/netifapi.h>
//#include <lwip/pbuf.h>

#include "lynxi.h"
#ifdef BSP_USING_RESET_CTRL
#include "drv_reset.h"
#endif
#ifdef BSP_USING_SYSCTL_CLK
#include "clock/drv_sysctl_lite.h"
#endif

#include "synopGMAC.h"
#include "mii.c"
#include "synopGMAC_debug.h"

/* synopGMAC.c 内联包含 mii.c；本函数仅在此文件使用，避免单独编译 mii.c 时产生未使用告警 */
static int lynxi_mii_link_ok_majority(struct mii_if_info *mii)
{
    int i, ok = 0;

    for (i = 0; i < 3; i++)
    {
        mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR);
        if (mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR) & BMSR_LSTATUS)
            ok++;
    }
    return ok >= 2;
}

static void lynxi_phy_diag_dump(synopGMACdevice *gmacdev, const char *tag)
{
    u16 phyid1 = 0, phyid2 = 0, bmcr = 0, bmsr = 0, anar = 0, anlpar = 0, gbsr = 0;
    u16 ctrl1000 = 0, estatus = 0;

    if (gmacdev == RT_NULL)
        return;

    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_PHYSID1, &phyid1);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_PHYSID2, &phyid2);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_BMCR, &bmcr);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_BMSR, &bmsr);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_ADVERTISE, &anar);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_LPA, &anlpar);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_STAT1000, &gbsr);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_CTRL1000, &ctrl1000);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_ESTATUS, &estatus);

    rt_kprintf("gmac phy[%s]: addr=%u id=%04x:%04x bmcr=%04x bmsr=%04x anar=%04x anlpar=%04x stat1000=%04x ctl1000=%04x estatus=%04x link=%d aneg_en=%d aneg_done=%d\n",
               tag, gmacdev->PhyBase, phyid1, phyid2, bmcr, bmsr, anar, anlpar, gbsr, ctrl1000, estatus,
               !!(bmsr & BMSR_LSTATUS), !!(bmcr & BMCR_ANENABLE), !!(bmsr & BMSR_ANEGCOMPLETE));
}
#include "ls1c_pin.h"

#if defined(BSP_USING_GMAC) && defined(BSP_USING_RPMSG_NET)
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/inet.h>
#ifdef RT_USING_NETDEV
#include <netdev.h>
#endif

/* 可在 rtconfig.h 中覆盖；未定义时与 RT_LWIP_*（e0）一致 */
#ifndef BSP_ETH0_IPADDR
#define BSP_ETH0_IPADDR   RT_LWIP_IPADDR
#endif
#ifndef BSP_ETH0_MSKADDR
#define BSP_ETH0_MSKADDR  RT_LWIP_MSKADDR
#endif
#ifndef BSP_ETH0_GWADDR
#define BSP_ETH0_GWADDR RT_LWIP_GWADDR
#endif

static void lynxi_eth0_apply_addr_and_sync_netdev(struct eth_device *eth)
{
    struct netif *nf;
    ip4_addr_t ip;
    ip4_addr_t nm;
    ip4_addr_t gw;
#ifdef RT_USING_NETDEV
    struct netdev *nd;
    char name[NETIF_NAMESIZE + 1];
#endif

    if (eth == RT_NULL || eth->netif == RT_NULL)
        return;

    ip.addr = inet_addr(BSP_ETH0_IPADDR);
    nm.addr = inet_addr(BSP_ETH0_MSKADDR);
    gw.addr = inet_addr(BSP_ETH0_GWADDR);
    if (netifapi_netif_set_addr(eth->netif, &ip, &nm, &gw) != ERR_OK)
    {
        rt_kprintf("lynxi: e0 netifapi_netif_set_addr failed\n");
        return;
    }

#ifdef RT_USING_NETDEV
    nf = eth->netif;
    rt_memcpy(name, nf->name, NETIF_NAMESIZE);
    name[NETIF_NAMESIZE] = '\0';
    nd = netdev_get_by_name(name);
    if (nd != RT_NULL)
    {
        nd->ip_addr = nf->ip_addr;
        nd->netmask = nf->netmask;
        nd->gw = nf->gw;
        nd->mtu = nf->mtu;
    }
#endif
}
#endif /* BSP_USING_GMAC && BSP_USING_RPMSG_NET */

#define Gmac_base          ((unsigned int)(rt_uintptr_t)GMAC_BASE)
#define Buffer_Size         2048
#define MAX_ADDR_LEN        6
#define NAMESIZE            16

#define LS1B_GMAC0_IRQ      34
#define LS1C_MAC_IRQ         IRQ_GMAC
#define BUS_SIZE_ALIGN(x) ((x+15)&~15)

#define DEFAULT_MAC_ADDRESS {0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}
#define LYNXI_GMAC_CTRL_REG         0x1250008c
#define LYNXI_GMAC_CTRL_1000M       0x66f
#define LYNXI_GMAC_CTRL_100M        0x65f
#define LYNXI_GMAC_CTRL_10M         0x64f
#define LYNXI_PHY_RESET_PIN         (3 * 32 + 23) /* portd23 */
#define LYNXI_PHY_RESET_ASSERT_MS   10
#define LYNXI_PHY_RESET_DEASSERT_MS 50

u32 regbase = (u32)(rt_uintptr_t)GMAC_BASE;
static u32 GMAC_Power_down;
extern void *plat_alloc_consistent_dmaable_memory(synopGMACdevice *pcidev, u32 size, u32 *addr) ;
extern s32 synopGMAC_check_phy_init(synopGMACPciNetworkAdapter *adapter) ;
extern int init_phy(synopGMACdevice *gmacdev);
dma_addr_t plat_dma_map_single(void *hwdev, void *ptr, u32 size);

void eth_rx_irq(int irqno, void *param);
static void eth_rx_poll_timer(void *param);

struct rt_eth_dev
{
    struct eth_device parent;
    rt_uint8_t dev_addr[MAX_ADDR_LEN];
    char *name;
    int iobase;
    int state;
    int index;
    struct rt_timer link_timer;
    struct rt_timer rx_poll_timer;
    void *priv;
};
static struct rt_eth_dev eth_dev;
static struct rt_semaphore sem_ack, sem_lock;
static rt_bool_t lynxi_gmac_rx_started;
static rt_bool_t lynxi_gmac_link_notified_up = RT_FALSE;

static rt_bool_t lynxi_gmac_netif_ready(struct eth_device *ed)
{
    return (ed != RT_NULL && ed->netif != RT_NULL);
}

/* 仅在软定时器线程调用（RT_TIMER_FLAG_SOFT_TIMER），可安全使用 eth_device_linkchange */
static void lynxi_eth_notify_link(rt_bool_t up)
{
    struct eth_device *ed = &eth_dev.parent;

    if (!lynxi_gmac_netif_ready(ed))
        return;

    if (up == lynxi_gmac_link_notified_up)
        return;

    lynxi_gmac_link_notified_up = up;
    /*
     * 软定时器线程上下文：直接 netifapi，勿走 erx 邮箱（链路 up 时 erx 收包与
     * netifapi 并发易把 erx/tcpip 卡死，表现为 Link is with 1000M 后 shell 假死）。
     */
    if (up)
        netifapi_netif_set_link_up(ed->netif);
    else
        netifapi_netif_set_link_down(ed->netif);
}

static void eth_rx_poll_timer(void *param)
{
    eth_rx_irq(LS1C_MAC_IRQ, param);
}

static void lynxi_gmac_set_ctrl_by_speed(rt_uint32_t speed)
{
    rt_uint32_t value = LYNXI_GMAC_CTRL_100M;

    if (speed == SPEED1000)
    {
        value = LYNXI_GMAC_CTRL_1000M;
    }
    else if (speed == SPEED10)
    {
        value = LYNXI_GMAC_CTRL_10M;
    }

#ifdef BSP_USING_SYSCTL_CLK
    lynxi_sysctl_lite_gmac_ctrl_set(value);
#else
    *(volatile rt_uint32_t *)(rt_uintptr_t)LYNXI_GMAC_CTRL_REG = value;
#endif
}

#define LYNXI_PHY_ID_RTL8211F_OUI    0x001cu
#define MII_PAGESEL                  31
#define RTL8211F_TX_DELAY            0x0100u  /* BIT(8), Linux realtek.c */
#define RTL8211F_RX_DELAY            0x0008u

static int lynxi_phy_write_page(synopGMACdevice *gmacdev, u16 page)
{
    return synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_PAGESEL, page);
}

static int lynxi_phy_write_paged(synopGMACdevice *gmacdev, u16 page, u16 reg, u16 val)
{
    int err = lynxi_phy_write_page(gmacdev, page);

    if (err < 0)
        return err;
    return synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, reg, val);
}

static int lynxi_phy_modify_paged(synopGMACdevice *gmacdev, u16 page, u16 reg,
                                  u16 mask, u16 set)
{
    u16 val;
    int err = lynxi_phy_write_page(gmacdev, page);

    if (err < 0)
        return err;
    err = synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, reg, &val);
    if (err < 0)
        return err;
    val = (val & (u16)~mask) | (set & mask);
    return synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, reg, val);
}

/*
 * Linux lynchip-lite-evb.dts phy-mode=rgmii-id；RTL8211F 需在 PHY 侧开 TX/RX delay
 *（realtek.c rtl8211f_config_init），否则 Host Link detected:no / anlpar=0000。
 */
static void lynxi_rtl8211f_rgmii_id_config(synopGMACdevice *gmacdev)
{
    lynxi_phy_write_paged(gmacdev, 0xd04, 0x10, 0x6c0b);
    lynxi_phy_modify_paged(gmacdev, 0xd08, 0x11, RTL8211F_TX_DELAY, RTL8211F_TX_DELAY);
    lynxi_phy_modify_paged(gmacdev, 0xd08, 0x15, RTL8211F_RX_DELAY, RTL8211F_RX_DELAY);
    lynxi_phy_write_page(gmacdev, 0);
    rt_kprintf("gmac: RTL8211F rgmii-id delay configured\n");
}

static int lynxi_phy_bmcr_soft_reset(synopGMACdevice *gmacdev)
{
    u16 bmcr = 0;
    int err;
    int retry;

    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_BMCR, BMCR_RESET);
    if (err < 0)
    {
        return err;
    }

    for (retry = 0; retry < 12; retry++)
    {
        rt_thread_mdelay(50);
        err = synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_BMCR, &bmcr);
        if (err < 0)
        {
            return err;
        }
        if ((bmcr & BMCR_RESET) == 0)
        {
            return 0;
        }
    }

    return -1;
}

/**
 * This sets up the transmit Descriptor queue in ring or chain mode.
 * This function is tightly coupled to the platform and operating system
 * Device is interested only after the descriptors are setup. Therefore this function
 * is not included in the device driver API. This function should be treated as an
 * example code to design the descriptor structures for ring mode or chain mode.
 * This function depends on the pcidev structure for allocation consistent dma-able memory in case
 * of linux.
 * This limitation is due to the fact that linux uses pci structure to allocate a dmable memory
 * - Allocates the memory for the descriptors.
 * - Initialize the Busy and Next descriptors indices to 0(Indicating first descriptor).
 * - Initialize the Busy and Next descriptors to first descriptor address.
 * - Initialize the last descriptor with the endof ring in case of ring mode.
 * - Initialize the descriptors in chain mode.
 * @param[in] pointer to synopGMACdevice.
 * @param[in] pointer to pci_device structure.
 * @param[in] number of descriptor expected in tx descriptor queue.
 * @param[in] whether descriptors to be created in RING mode or CHAIN mode.
 * \return 0 upon success. Error code upon failure.
 * \note This function fails if allocation fails for required number of descriptors in Ring mode,
 * but in chain mode
 * function returns -ESYNOPGMACNOMEM in the process of descriptor chain creation. once returned from
 * this function
 * user should for gmacdev->TxDescCount to see how many descriptors are there in the chain. Should
 * continue further
 * only if the number of descriptors in the chain meets the requirements
 */
s32 synopGMAC_setup_tx_desc_queue(synopGMACdevice *gmacdev, u32 no_of_desc, u32 desc_mode)
{
    s32 i;

    DmaDesc *first_desc = NULL;

    dma_addr_t dma_addr;
    gmacdev->TxDescCount = 0;

    first_desc = (DmaDesc *)plat_alloc_consistent_dmaable_memory(gmacdev, sizeof(DmaDesc) * no_of_desc, &dma_addr);
    if (first_desc == NULL)
    {
        rt_kprintf("Error in Tx Descriptors memory allocation\n");
        return -ESYNOPGMACNOMEM;
    }

    DEBUG_MES("tx_first_desc_addr = %p\n", first_desc);
    DEBUG_MES("dmaadr = %p\n", dma_addr);
    gmacdev->TxDescCount = no_of_desc;
    gmacdev->TxDesc      = first_desc;
    gmacdev->TxDescDma  = dma_addr;

    for (i = 0; i < gmacdev->TxDescCount; i++)
    {
        synopGMAC_tx_desc_init_ring(gmacdev->TxDesc + i, i == gmacdev->TxDescCount - 1);
#if SYNOP_TOP_DEBUG
        rt_kprintf("\n%02d %08x \n", i, (unsigned int)(gmacdev->TxDesc + i));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i))->status);
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->length));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->buffer1));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->buffer2));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->data1));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->data2));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->dummy1));
        rt_kprintf("%08x ", (unsigned int)((gmacdev->TxDesc + i)->dummy2));
#endif
    }

    gmacdev->TxNext = 0;
    gmacdev->TxBusy = 0;
    gmacdev->TxNextDesc = gmacdev->TxDesc;
    gmacdev->TxBusyDesc = gmacdev->TxDesc;
    gmacdev->BusyTxDesc  = 0;

    return -ESYNOPGMACNOERR;
}

/**
 * This sets up the receive Descriptor queue in ring or chain mode.
 * This function is tightly coupled to the platform and operating system
 * Device is interested only after the descriptors are setup. Therefore this function
 * is not included in the device driver API. This function should be treated as an
 * example code to design the descriptor structures in ring mode or chain mode.
 * This function depends on the pcidev structure for allocation of consistent dma-able memory in
 * case of linux.
 * This limitation is due to the fact that linux uses pci structure to allocate a dmable memory
 * - Allocates the memory for the descriptors.
 * - Initialize the Busy and Next descriptors indices to 0(Indicating first descriptor).
 * - Initialize the Busy and Next descriptors to first descriptor address.
 * - Initialize the last descriptor with the endof ring in case of ring mode.
 * - Initialize the descriptors in chain mode.
 * @param[in] pointer to synopGMACdevice.
 * @param[in] pointer to pci_device structure.
 * @param[in] number of descriptor expected in rx descriptor queue.
 * @param[in] whether descriptors to be created in RING mode or CHAIN mode.
 * \return 0 upon success. Error code upon failure.
 * \note This function fails if allocation fails for required number of descriptors in Ring mode,
 * but in chain mode
 * function returns -ESYNOPGMACNOMEM in the process of descriptor chain creation. once returned from
 * this function
 * user should for gmacdev->RxDescCount to see how many descriptors are there in the chain. Should
 * continue further
 * only if the number of descriptors in the chain meets the requirements
 */
s32 synopGMAC_setup_rx_desc_queue(synopGMACdevice *gmacdev, u32 no_of_desc, u32 desc_mode)
{
    s32 i;
    DmaDesc *first_desc = NULL;

    dma_addr_t dma_addr;

    gmacdev->RxDescCount = 0;
    first_desc = (DmaDesc *)plat_alloc_consistent_dmaable_memory(gmacdev, sizeof(DmaDesc) * no_of_desc, &dma_addr);
    if (first_desc == NULL)
    {
        rt_kprintf("Error in Rx Descriptor Memory allocation in Ring mode\n");
        return -ESYNOPGMACNOMEM;
    }

    DEBUG_MES("rx_first_desc_addr = %p\n", first_desc);
    DEBUG_MES("dmaadr = %p\n", dma_addr);
    gmacdev->RxDescCount = no_of_desc;
    gmacdev->RxDesc      = (DmaDesc *)first_desc;
    gmacdev->RxDescDma   = dma_addr;

    for (i = 0; i < gmacdev->RxDescCount; i++)
    {
        synopGMAC_rx_desc_init_ring(gmacdev->RxDesc + i, i == gmacdev->RxDescCount - 1);

    }

    gmacdev->RxNext = 0;
    gmacdev->RxBusy = 0;
    gmacdev->RxNextDesc = gmacdev->RxDesc;
    gmacdev->RxBusyDesc = gmacdev->RxDesc;

    gmacdev->BusyRxDesc   = 0;

    return -ESYNOPGMACNOERR;
}

void synopGMAC_linux_cable_unplug_function(void *adaptr)
{
    s32 data;
    u16 bmsr;
    synopGMACPciNetworkAdapter *adapter = (synopGMACPciNetworkAdapter *)adaptr;
    synopGMACdevice            *gmacdev = adapter->synopGMACdev;
    /* 与 mii_link_ok_majority 配合：连续 2 个轮询周期判定为同一状态后再更新，避免反复 mac_init/CPR 写导致抖动 */
    static rt_uint32_t link_up_streak;
    static rt_uint32_t link_down_streak;
    static u16 last_bmsr;
    static rt_bool_t bmsr_inited = RT_FALSE;
#define LYNXI_LINK_DEBOUNCE_TICKS  2

    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_BMSR, &bmsr);
    if (!bmsr_inited || bmsr != last_bmsr)
    {
        rt_bool_t last_link = bmsr_inited ? !!(last_bmsr & BMSR_LSTATUS) : RT_FALSE;
        rt_bool_t cur_link = !!(bmsr & BMSR_LSTATUS);

        /* 仅 LSTATUS 变化时打印，避免自协商抖动刷屏占满串口导致 shell 看似卡死 */
        if (!bmsr_inited || last_link != cur_link)
        {
            rt_kprintf("gmac phy[bmsr]: link %d -> %d (bmsr=%04x aneg_done=%d)\n",
                       last_link, cur_link, bmsr, !!(bmsr & BMSR_ANEGCOMPLETE));
        }
        last_bmsr = bmsr;
        bmsr_inited = RT_TRUE;
    }

    int raw_link = lynxi_mii_link_ok_majority(&adapter->mii);

    if (bmsr & BMSR_LSTATUS)
        raw_link = 1;

    if (raw_link)
    {
        link_up_streak++;
        link_down_streak = 0;
    }
    else
    {
        link_down_streak++;
        link_up_streak = 0;
    }

    if (link_up_streak < LYNXI_LINK_DEBOUNCE_TICKS && link_down_streak < LYNXI_LINK_DEBOUNCE_TICKS)
        return;

    if (link_down_streak >= LYNXI_LINK_DEBOUNCE_TICKS)
    {
        if (gmacdev->LinkState)
        {
            rt_kprintf("\r\nNo Link\r\n");
            lynxi_eth_notify_link(RT_FALSE);
        }
        gmacdev->DuplexMode = 0;
        gmacdev->Speed = 0;
        gmacdev->LoopBackMode = 0;
        gmacdev->LinkState = 0;
        return;
    }

    data = synopGMAC_check_phy_init(adapter);

    if (gmacdev->LinkState != data)
    {
        gmacdev->LinkState = data;
        synopGMAC_mac_init(gmacdev);
        lynxi_gmac_set_ctrl_by_speed(gmacdev->Speed);
        lynxi_eth_notify_link(RT_TRUE);
        rt_kprintf("Link is up in %s mode\n", (gmacdev->DuplexMode == FULLDUPLEX) ? "FULL DUPLEX" : "HALF DUPLEX");
        if (gmacdev->Speed == SPEED1000)
            rt_kprintf("Link is with 1000M Speed \r\n");
        if (gmacdev->Speed == SPEED100)
            rt_kprintf("Link is with 100M Speed \n");
        if (gmacdev->Speed == SPEED10)
            rt_kprintf("Link is with 10M Speed \n");
    }
}

s32 synopGMAC_check_phy_init(synopGMACPciNetworkAdapter *adapter)
{
    struct ethtool_cmd cmd;
    synopGMACdevice            *gmacdev = adapter->synopGMACdev;
    static rt_tick_t last_linkdown_diag_tick;

    if (!mii_link_ok(&adapter->mii))
    {
        rt_tick_t now = rt_tick_get();
        if ((now - last_linkdown_diag_tick) >= (2 * RT_TICK_PER_SECOND))
        {
            last_linkdown_diag_tick = now;
            lynxi_phy_diag_dump(gmacdev, "link-down");
        }

        gmacdev->DuplexMode = FULLDUPLEX;
        gmacdev->Speed      =   SPEED100;

        return 0;
    }
    else
    {
        mii_ethtool_gset(&adapter->mii, &cmd);

        gmacdev->DuplexMode = (cmd.duplex == DUPLEX_FULL)  ? FULLDUPLEX : HALFDUPLEX ;
        if (cmd.speed == SPEED_1000)
            gmacdev->Speed      =   SPEED1000;
        else if (cmd.speed == SPEED_100)
            gmacdev->Speed      =   SPEED100;
        else
            gmacdev->Speed      =   SPEED10;
    }

    return gmacdev->Speed | (gmacdev->DuplexMode << 4);
}

static void lynxi_gmac_irq_setup(struct rt_eth_dev *dev);
static void lynxi_gmac_rx_start(void);
void eth_rx_irq(int irqno, void *param);

static rt_err_t eth_init(rt_device_t device)
{
    struct eth_device *eth_device = (struct eth_device *)device;
    RT_ASSERT(eth_device != RT_NULL);

    s32 status = 0;
    u64 dma_addr;
    struct rt_eth_dev *dev = &eth_dev;
    struct synopGMACNetworkAdapter *adapter = dev->priv;
    synopGMACdevice *gmacdev = (synopGMACdevice *)adapter->synopGMACdev;

    /*
     * MacBase/DmaBase/PhyBase and MAC address were programmed in rt_hw_eth_init().
     * Do not call synopGMAC_attach() again: it rt_memset()s the whole device struct
     * and would duplicate PHY scan; more importantly we must keep one consistent state.
     */
    synopGMAC_reset(gmacdev);

    synopGMAC_read_version(gmacdev);

    synopGMAC_set_mdc_clk_div(gmacdev, LYNXI_GMAC4_MDC_CSR_DEFAULT);
    gmacdev->ClockDivMdc = synopGMAC_get_mdc_clk_div(gmacdev);

    /* init_phy / attach 已在 rt_hw_eth_init 完成，此处勿重复 BMCR 复位 */

    DEBUG_MES("tx desc_queue\n");
    synopGMAC_setup_tx_desc_queue(gmacdev, TRANSMIT_DESC_SIZE, RINGMODE);
    synopGMAC_init_tx_desc_base(gmacdev);

    DEBUG_MES("rx desc_queue\n");
    synopGMAC_setup_rx_desc_queue(gmacdev, RECEIVE_DESC_SIZE, RINGMODE);
    synopGMAC_init_rx_desc_base(gmacdev);
    DEBUG_MES("DmaRxBaseAddr = %08x\n", synopGMACReadReg(gmacdev->DmaBase, DmaRxBaseAddr));

//  u32 dmaRx_Base_addr = synopGMACReadReg(gmacdev->DmaBase,DmaRxBaseAddr);
//  rt_kprintf("first_desc_addr = 0x%x\n", dmaRx_Base_addr);

#ifdef ENH_DESC_8W
    synopGMAC_dma_bus_mode_init(gmacdev, DmaBurstLength32 | DmaDescriptorSkip2 | DmaDescriptor8Words);
#else
    //synopGMAC_dma_bus_mode_init(gmacdev, DmaBurstLength4 | DmaDescriptorSkip1);
    synopGMAC_dma_bus_mode_init(gmacdev, DmaBurstLength4 | DmaDescriptorSkip2);
#endif
    synopGMAC_dma_control_init(gmacdev, DmaStoreAndForward | DmaTxSecondFrame | DmaRxThreshCtrl128);

    status = synopGMAC_check_phy_init(adapter);
    synopGMAC_mac_init(gmacdev);
    /* 0x66f/0x65f 仅在链路 up 后由 link_timer 写，probe/eth_init 不写 */

    synopGMAC_pause_control(gmacdev);

#ifdef IPC_OFFLOAD
    synopGMAC_enable_rx_chksum_offload(gmacdev);
    synopGMAC_rx_tcpip_chksum_drop_enable(gmacdev);
#endif

    rt_ubase_t skb;
    do
    {
        skb = (rt_ubase_t)plat_alloc_memory(RX_BUF_SIZE);      //should skb aligned here?
        if (skb == (rt_ubase_t)RT_NULL)
        {
            rt_kprintf("ERROR in skb buffer allocation\n");
            break;
        }

        dma_addr = plat_dma_map_single(gmacdev, (void *)skb, RX_BUF_SIZE);  //获取 skb 的 dma 地址

        status = synopGMAC_set_rx_qptr(gmacdev, dma_addr, RX_BUF_SIZE, (u32)skb, 0, 0, 0);
        if (status < 0)
        {
            rt_kprintf("status < 0!!\n");
            plat_free_memory((void *)skb);
        }
    }
    while (status >= 0 && (status < (RECEIVE_DESC_SIZE - 1)));

    synopGMAC_clear_interrupt(gmacdev);

    synopGMAC_disable_mmc_tx_interrupt(gmacdev, 0xFFFFFFFF);
    synopGMAC_disable_mmc_rx_interrupt(gmacdev, 0xFFFFFFFF);
    synopGMAC_disable_mmc_ipc_rx_interrupt(gmacdev, 0xFFFFFFFF);

    plat_delay(DEFAULT_LOOP_VARIABLE);
    synopGMAC_check_phy_init(adapter);
    synopGMAC_mac_init(gmacdev);

    rt_timer_init(&dev->link_timer, "link_timer",
                  synopGMAC_linux_cable_unplug_function,
                  (void *)adapter,
                  RT_TICK_PER_SECOND,
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
#ifdef RT_USING_GMAC_INT_MODE
    lynxi_gmac_irq_setup(dev);
#else
    rt_timer_init(&dev->rx_poll_timer, "rx_poll_timer",
                  eth_rx_poll_timer,
                  (void *)adapter,
                  1,
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
#endif  /*RT_USING_GMAC_INT_MODE*/

    rt_kprintf("eth_inited!\n");

    return RT_EOK;
}

static rt_err_t eth_open(rt_device_t dev, rt_uint16_t oflag)
{
    rt_kprintf("eth_open!!\n");

    return RT_EOK;
}

static rt_err_t eth_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_ssize_t eth_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_ssize_t eth_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_err_t eth_control(rt_device_t dev, int cmd, void *args)
{
    switch (cmd)
    {
    case NIOCTL_GADDR:
        if (args) rt_memcpy(args, eth_dev.dev_addr, 6);
        else return -RT_ERROR;
        break;

    default :
        break;
    }
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops _gmac_dev_ops =
{
    .init = eth_init,
    .open = eth_open,
    .close = eth_close,
    .read = eth_read,
    .write = eth_write,
    .control = eth_control,
};
#endif

rt_err_t rt_eth_tx(rt_device_t device, struct pbuf *p)
{
    /* lock eth device */
    rt_sem_take(&sem_lock, RT_WAITING_FOREVER);

    DEBUG_MES("in %s\n", __FUNCTION__);

    u32 status;
    rt_ubase_t pbuf;
    u64 dma_addr;
    u32 offload_needed = 0;
    u32 index;
    DmaDesc *dpr = RT_NULL;
    struct rt_eth_dev *dev = (struct rt_eth_dev *) device;
    struct synopGMACNetworkAdapter *adapter;
    synopGMACdevice *gmacdev;
    adapter = (struct synopGMACNetworkAdapter *) dev->priv;
    if (adapter == NULL)
        return -1;

    gmacdev = (synopGMACdevice *) adapter->synopGMACdev;
    if (gmacdev == NULL)
        return -1;

    if (!synopGMAC_is_desc_owned_by_dma(gmacdev->TxNextDesc))
    {

        pbuf = (rt_ubase_t)plat_alloc_memory(p->tot_len);
        //pbuf = (u32)pbuf_alloc(PBUF_LINK, p->len, PBUF_RAM);
        if (pbuf == (rt_ubase_t)0)
        {
            rt_kprintf("===error in alloc bf1\n");
            return -1;
        }

        DEBUG_MES("p->len = %d\n", p->len);
        pbuf_copy_partial(p, (void *)pbuf, p->tot_len, 0);
        dma_addr = plat_dma_map_single(gmacdev, (void *)pbuf, p->tot_len);

        status = synopGMAC_set_tx_qptr(gmacdev, dma_addr, p->tot_len, pbuf, 0, 0, 0, offload_needed, &index, dpr);
        if (status < 0)
        {
            rt_kprintf("%s No More Free Tx Descriptors\n", __FUNCTION__);

            plat_free_memory((void *)pbuf);
            return -16;
        }
    }
    synopGMAC_resume_dma_tx(gmacdev);

    s32 desc_index;
    u32 data1, data2;
    rt_ubase_t data1_addr;
    u32 dma_addr1, dma_addr2;
    u32 length1, length2;
#ifdef ENH_DESC_8W
    u32 ext_status;
    u16 time_stamp_higher;
    u32 time_stamp_high;
    u32 time_stamp_low;
#endif
    do
    {
#ifdef ENH_DESC_8W
        desc_index = synopGMAC_get_tx_qptr(gmacdev, &status, &dma_addr1, &length1, &data1, &dma_addr2, &length2, &data2, &ext_status, &time_stamp_high, &time_stamp_low);
        synopGMAC_TS_read_timestamp_higher_val(gmacdev, &time_stamp_higher);
#else
        desc_index = synopGMAC_get_tx_qptr(gmacdev, &status, &dma_addr1, &length1, &data1, &dma_addr2, &length2, &data2);
#endif
        if (desc_index >= 0 && data1 != 0)
        {
#ifdef  IPC_OFFLOAD
            if (synopGMAC_is_tx_ipv4header_checksum_error(gmacdev, status))
            {
                rt_kprintf("Harware Failed to Insert IPV4 Header Checksum\n");
            }
            if (synopGMAC_is_tx_payload_checksum_error(gmacdev, status))
            {
                rt_kprintf("Harware Failed to Insert Payload Checksum\n");
            }
#endif

            data1_addr = (rt_ubase_t)data1;
            plat_free_memory((void *)data1_addr);  // sw: data1 = buffer1

            if (synopGMAC_is_desc_valid(status))
            {
                adapter->synopGMACNetStats.tx_bytes += length1;
                adapter->synopGMACNetStats.tx_packets++;
            }
            else
            {
                adapter->synopGMACNetStats.tx_errors++;
                adapter->synopGMACNetStats.tx_aborted_errors += synopGMAC_is_tx_aborted(status);
                adapter->synopGMACNetStats.tx_carrier_errors += synopGMAC_is_tx_carrier_error(status);
            }
        }
        adapter->synopGMACNetStats.collisions += synopGMAC_get_tx_collision_count(status);
    }
    while (desc_index >= 0);

    /* unlock eth device */
    rt_sem_release(&sem_lock);
//  rt_kprintf("output %d bytes\n", p->len);
    return RT_EOK;
}

struct pbuf *rt_eth_rx(rt_device_t device)
{
    DEBUG_MES("%s : \n", __FUNCTION__);
    struct rt_eth_dev *dev = &eth_dev;
    struct synopGMACNetworkAdapter *adapter;
    synopGMACdevice *gmacdev;
//  struct PmonInet * pinetdev;
    s32 desc_index;
    u32 data1;
    u32 data2;
    rt_ubase_t data1_addr;
    u32 len;
    u32 status;
    u32 dma_addr1;
    u32 dma_addr2;
    struct pbuf *pbuf = RT_NULL;
    rt_sem_take(&sem_lock, RT_WAITING_FOREVER);

    adapter = (struct synopGMACNetworkAdapter *) dev->priv;
    if (adapter == NULL)
    {
        rt_kprintf("%S : Unknown Device !!\n", __FUNCTION__);
        return NULL;
    }

    gmacdev = (synopGMACdevice *) adapter->synopGMACdev;
    if (gmacdev == NULL)
    {
        rt_kprintf("%s : GMAC device structure is missing\n", __FUNCTION__);
        return NULL;
    }

    /*Handle the Receive Descriptors*/
      desc_index = synopGMAC_get_rx_qptr(gmacdev, &status, &dma_addr1, NULL, &data1, &dma_addr2, NULL, &data2);
      if (desc_index >= 0 && data1 != 0)
        {
          DEBUG_MES("Received Data at Rx Descriptor %d for skb 0x%08x whose status is %08x\n", desc_index, dma_addr1, status);
          if (synopGMAC_is_rx_desc_valid(status) || SYNOP_PHY_LOOPBACK)
            {
                data1_addr = (rt_ubase_t)data1;
                dma_addr1 =  plat_dma_map_single(gmacdev, (void *)data1_addr, RX_BUF_SIZE);
                len =  synopGMAC_get_rx_desc_frame_length(status)-4; //Not interested in Ethernet CRC bytes
                pbuf = pbuf_alloc(PBUF_LINK, len, PBUF_RAM);
                if (pbuf == 0) rt_kprintf("===error in pbuf_alloc\n");
                rt_memcpy(pbuf->payload, (char *)data1_addr, len);
                DEBUG_MES("==get pkg len: %d\n", len);
            }
            else
            {
                rt_kprintf("s: %08x\n", status);
                adapter->synopGMACNetStats.rx_errors++;
                adapter->synopGMACNetStats.collisions       += synopGMAC_is_rx_frame_collision(status);
                adapter->synopGMACNetStats.rx_crc_errors    += synopGMAC_is_rx_crc(status);
                adapter->synopGMACNetStats.rx_frame_errors  += synopGMAC_is_frame_dribbling_errors(status);
                adapter->synopGMACNetStats.rx_length_errors += synopGMAC_is_rx_frame_length_errors(status);
            }
            desc_index = synopGMAC_set_rx_qptr(gmacdev, dma_addr1, RX_BUF_SIZE, (u32)data1, 0, 0, 0);
            if (desc_index < 0)
            {
#if SYNOP_RX_DEBUG
                rt_kprintf("Cannot set Rx Descriptor for data1 %08x\n", (u32)data1);
#endif
                data1_addr = (rt_ubase_t)data1;
                plat_free_memory((void *)data1_addr);
            }
        }
    rt_sem_release(&sem_lock);
    DEBUG_MES("%s : before return \n", __FUNCTION__);
    return pbuf;
}

static int rtl88e1111_config_init(synopGMACdevice *gmacdev)
{
    int err;
    u16 data;

    DEBUG_MES("in %s\n", __FUNCTION__);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x14, &data);
    data = data | 0x82;
    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x14, data);
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x00, &data);
    data = data | 0x8000;
    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x00, data);
#if SYNOP_PHY_LOOPBACK
    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x14, &data);
    data = data | 0x70;
    data = data & 0xffdf;
    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x14, data);
    data = 0x8000;
    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x00, data);
    data = 0x5140;
    err = synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x00, data);
#endif
    if (err < 0)
        return err;
    return 0;
}

int init_phy(synopGMACdevice *gmacdev)
{
    u16 data;

    synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, MII_PHYSID1, &data);
    /*
     * Zephyr KA200：phy_ref 使能后 Port-D gpio-dwapb 硬复位会挂死，改 BMCR 软复位。
     * EVB RTL8211F OUI=0x001c（实板 id=001c:c916）。
     */
    if (data == LYNXI_PHY_ID_RTL8211F_OUI)
    {
        rt_kprintf("gmac: BMCR soft reset (PHYID=0x%04x, skip gpio portd:23)\n", data);
        if (lynxi_phy_bmcr_soft_reset(gmacdev) != 0)
        {
            rt_kprintf("gmac: BMCR soft reset timeout\n");
        }
        lynxi_rtl8211f_rgmii_id_config(gmacdev);
    }
    /*set 88e1111 clock phase delay*/
    if (data == 0x141)
        rtl88e1111_config_init(gmacdev);
#if 0
    else if (data == 0x8201)
    {
        //RTL8201
        data = 0x400;    // set RMII mode
        synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x19, data);
        synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x19, &data);
        TR("phy reg25 is %0x \n", data);

        data = 0x3100;    //set  100M speed
        synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x0, data);
    }
    else if (data == 0x0180 || data == 0x0181)
    {
        //DM9161
        synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x10, &data);
        data |= (1 << 8);  //set RMII mode
        synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x10, data); //set RMII mode
        synopGMAC_read_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x10, &data);
        TR("phy reg16 is 0x%0x \n", data);

        //  synopGMAC_read_phy_reg(gmacdev->MacBase,gmacdev->PhyBase,0x0,&data);
        //  data &= ~(1<<10);
        data = 0x3100;  //set auto-
        //data = 0x0100;    //set  10M speed
        synopGMAC_write_phy_reg(gmacdev->MacBase, gmacdev->PhyBase, 0x0, data);
    }
#endif

    lynxi_phy_diag_dump(gmacdev, "init");

    return 0;
}

u32 synopGMAC_wakeup_filter_config3[] =
{
    0x00000000,
    0x000000FF,
    0x00000000,
    0x00000000,
    0x00000100,
    0x00003200,
    0x7eED0000,
    0x00000000
};


static void lynxi_gmac_irq_setup(struct rt_eth_dev *dev)
{
    RT_ASSERT(dev != RT_NULL);

    rt_hw_interrupt_install(LS1C_MAC_IRQ, eth_rx_irq, dev, "e0_isr");
    /* Linux DTS IRQ_TYPE_LEVEL_HIGH */
    rt_hw_interrupt_set_triger_mode(LS1C_MAC_IRQ, 1);
#if defined(RT_USING_SMP) && defined(BSP_USING_GICV3)
    if (rt_hw_interrupt_set_affinity(LS1C_MAC_IRQ, 0) != RT_EOK)
    {
        rt_kprintf("gmac: irq %d affinity cpu0 failed\n", LS1C_MAC_IRQ);
    }
#endif
    rt_hw_interrupt_mask(LS1C_MAC_IRQ);
    rt_kprintf("gmac: irq %d installed (masked until netdev up)\n", LS1C_MAC_IRQ);
}

static void lynxi_gmac_rx_start(void)
{
    struct synopGMACNetworkAdapter *adapter;
    synopGMACdevice *gmacdev;

    if (lynxi_gmac_rx_started || eth_dev.priv == RT_NULL)
        return;

    adapter = (struct synopGMACNetworkAdapter *)eth_dev.priv;
    gmacdev = adapter->synopGMACdev;
    if (gmacdev == RT_NULL)
        return;

    synopGMAC_enable_interrupt(gmacdev, DmaIntEnable);
    /* 链路由 link_timer+MDIO 轮询；勿开 RGMII 线中断，否则电平 IRQ 风暴卡死 CPU */
    synopGMACSetBits(gmacdev->MacBase, GmacInterruptMask, GmacRgmiiIntMask);
    synopGMAC_enable_dma_rx(gmacdev);
    synopGMAC_enable_dma_tx(gmacdev);

    rt_timer_start(&eth_dev.link_timer);
#ifdef RT_USING_GMAC_INT_MODE
    rt_hw_interrupt_umask(LS1C_MAC_IRQ);
#else
    rt_timer_start(&eth_dev.rx_poll_timer);
#endif
    lynxi_gmac_rx_started = RT_TRUE;
    rt_kprintf("gmac: rx/irq started\n");
}

static int mdio_read(synopGMACPciNetworkAdapter *adapter, int addr, int reg)
{
    synopGMACdevice *gmacdev;
    u16 data;
    gmacdev = adapter->synopGMACdev;

    synopGMAC_read_phy_reg(gmacdev->MacBase, addr, reg, &data);
    return data;
}

static void mdio_write(synopGMACPciNetworkAdapter *adapter, int addr, int reg, int data)
{
    synopGMACdevice *gmacdev;
    gmacdev = adapter->synopGMACdev;
    synopGMAC_write_phy_reg(gmacdev->MacBase, addr, reg, data);
}

void eth_rx_irq(int irqno, void *param)
{
    struct rt_eth_dev *dev = (struct rt_eth_dev *)param;

    RT_UNUSED(irqno);

    if (dev == RT_NULL)
    {
        dev = &eth_dev;
    }

    if (dev->priv == RT_NULL)
    {
        return;
    }

    struct synopGMACNetworkAdapter *adapter = dev->priv;
    synopGMACdevice *gmacdev = (synopGMACdevice *)adapter->synopGMACdev;

    u32 interrupt, dma_status_reg;

    /*
     * Linux IRQ_TYPE_LEVEL_HIGH：入口 mask GIC，处理完并清 MAC/DMA 状态后再 unmask，
     * 否则链路 up 后易 IRQ 风暴占满 CPU，shell 无法响应。
     */
    rt_hw_interrupt_mask(LS1C_MAC_IRQ);

    dma_status_reg = synopGMACReadReg(gmacdev->DmaBase, DmaStatus);
    if (dma_status_reg == 0)
    {
        u32 mac_irq_st = synopGMACReadReg(gmacdev->MacBase, GmacInterruptStatus);

        /* 清 RGMII/线接口挂起位（W1C），避免 LEVEL_HIGH 线一直有效 */
        if (mac_irq_st & GmacRgmiiIntSts)
            synopGMACWriteReg(gmacdev->MacBase, GmacInterruptStatus, GmacRgmiiIntSts);
        /*
         * 部分板级在 DMA CSR5 恒为 0 时仍由 GIC 投递 SPI78；若已有收包描述符则
         * 主动唤醒 erx，避免 e0_isr counter 不增且无 RX 统计。
         */
        if (lynxi_gmac_netif_ready(&eth_dev.parent) && gmacdev->LinkState)
            eth_device_ready(&eth_dev.parent);
        synopGMAC_clear_interrupt(gmacdev);
        synopGMAC_enable_interrupt(gmacdev, DmaIntEnable);
        rt_hw_interrupt_umask(LS1C_MAC_IRQ);
        return;
    }

    //rt_kprintf("dma_status_reg is 0x%x\n", dma_status_reg);
    synopGMAC_disable_interrupt_all(gmacdev);
    (void)synopGMACReadReg(gmacdev->MacBase, GmacStatus);

    if (dma_status_reg & GmacPmtIntr)
    {
        rt_kprintf("%s:: Interrupt due to PMT module\n", __FUNCTION__);
        //synopGMAC_linux_powerup_mac(gmacdev);
    }
    if (dma_status_reg & GmacMmcIntr)
    {
        rt_kprintf("%s:: Interrupt due to MMC module\n", __FUNCTION__);
        DEBUG_MES("%s:: synopGMAC_rx_int_status = %08x\n", __FUNCTION__, synopGMAC_read_mmc_rx_int_status(gmacdev));
        DEBUG_MES("%s:: synopGMAC_tx_int_status = %08x\n", __FUNCTION__, synopGMAC_read_mmc_tx_int_status(gmacdev));
    }

    if (dma_status_reg & GmacLineIntfIntr)
    {
        rt_kprintf("%s:: Interrupt due to GMAC LINE module\n", __FUNCTION__);
    }

    interrupt = synopGMAC_get_interrupt_type(gmacdev);
    //rt_kprintf("%s:Interrupts to be handled: 0x%08x\n",__FUNCTION__,interrupt);
    if (interrupt & synopGMACDmaError)
    {
        u8 mac_addr0[6];
        rt_kprintf("%s::Fatal Bus Error Inetrrupt Seen\n", __FUNCTION__);

        memcpy(mac_addr0, dev->dev_addr, 6);
        synopGMAC_disable_dma_tx(gmacdev);
        synopGMAC_disable_dma_rx(gmacdev);

        synopGMAC_take_desc_ownership_tx(gmacdev);
        synopGMAC_take_desc_ownership_rx(gmacdev);

        synopGMAC_init_tx_rx_desc_queue(gmacdev);

        synopGMAC_reset(gmacdev);

        synopGMAC_set_mac_addr(gmacdev, GmacAddr0High, GmacAddr0Low, mac_addr0);
        synopGMAC_dma_bus_mode_init(gmacdev, DmaFixedBurstEnable | DmaBurstLength8 | DmaDescriptorSkip2);
        synopGMAC_dma_control_init(gmacdev, DmaStoreAndForward);
        synopGMAC_init_rx_desc_base(gmacdev);
        synopGMAC_init_tx_desc_base(gmacdev);
        synopGMAC_mac_init(gmacdev);
        synopGMAC_enable_dma_rx(gmacdev);
        synopGMAC_enable_dma_tx(gmacdev);

    }
    if (interrupt & synopGMACDmaRxNormal)
    {
        //DEBUG_MES("%s:: Rx Normal \n", __FUNCTION__);
        //synop_handle_received_data(netdev);
        if (lynxi_gmac_netif_ready(&eth_dev.parent))
            eth_device_ready(&eth_dev.parent);
    }
    if (interrupt & synopGMACDmaRxAbnormal)
    {
        //rt_kprintf("%s::Abnormal Rx Interrupt Seen\n",__FUNCTION__);
        if (GMAC_Power_down == 0)
        {
            adapter->synopGMACNetStats.rx_over_errors++;
            synopGMACWriteReg(gmacdev->DmaBase, DmaStatus, 0x80);
            synopGMAC_resume_dma_rx(gmacdev);
        }
    }
    if (interrupt & synopGMACDmaRxStopped)
    {
        rt_kprintf("%s::Receiver stopped seeing Rx interrupts\n", __FUNCTION__); //Receiver gone in to stopped state
    }

    if (interrupt & synopGMACDmaTxNormal)
    {
        DEBUG_MES("%s::Finished Normal Transmission \n", __FUNCTION__);
        //          synop_handle_transmit_over(netdev);
    }

    if (interrupt & synopGMACDmaTxAbnormal)
    {
        rt_kprintf("%s::Abnormal Tx Interrupt Seen\n", __FUNCTION__);
    }
    if (interrupt & synopGMACDmaTxStopped)
    {
        TR("%s::Transmitter stopped sending the packets\n", __FUNCTION__);
        if (GMAC_Power_down == 0)    // If Mac is not in powerdown
        {
            synopGMAC_disable_dma_tx(gmacdev);
            synopGMAC_take_desc_ownership_tx(gmacdev);

            synopGMAC_enable_dma_tx(gmacdev);
            //      netif_wake_queue(netdev);
            TR("%s::Transmission Resumed\n", __FUNCTION__);
        }
    }
    synopGMAC_clear_interrupt(gmacdev);
    synopGMAC_enable_interrupt(gmacdev, DmaIntEnable);
    rt_hw_interrupt_umask(LS1C_MAC_IRQ);
}

int rt_hw_eth_init(void)
{
    u64 base_addr = Gmac_base;
    struct synopGMACNetworkAdapter *synopGMACadapter;
    static u8 mac_addr0[6] = DEFAULT_MAC_ADDRESS;

    rt_sem_init(&sem_ack, "tx_ack", 1, RT_IPC_FLAG_FIFO);
    rt_sem_init(&sem_lock, "eth_lock", 1, RT_IPC_FLAG_FIFO);

#ifdef BSP_GMAC_INIT_PINMUX
    int index;

    for (index = 21; index <= 30; index++)
    {
        pin_set_purpose(index, PIN_PURPOSE_OTHER);
        pin_set_remap(index, PIN_REMAP_DEFAULT);
    }
    pin_set_purpose(35, PIN_PURPOSE_OTHER);
    pin_set_remap(35, PIN_REMAP_DEFAULT);
#endif
#ifdef BSP_USING_SYSCTL_CLK
    lynxi_sysctl_lite_gmac_probe_clocks();
    rt_kprintf("gmac: probe clocks CPR+0x8c=0x%08x (gates~0x207; 0x66f 为 boot/链路后速率字)\n",
               *(volatile rt_uint32_t *)(rt_uintptr_t)LYNXI_GMAC_CTRL_REG);
#else
#ifdef BSP_USING_RESET_CTRL
    lynxi_reset_pulse(LYNXI_RESET_ETH, 1);
#endif
#endif

    memset(&eth_dev, 0, sizeof(eth_dev));
    synopGMACadapter = (struct synopGMACNetworkAdapter *)plat_alloc_memory(sizeof(struct synopGMACNetworkAdapter));
    if (!synopGMACadapter)
    {
        rt_kprintf("Error in Memory Allocataion, Founction : %s \n", __FUNCTION__);
        return -RT_ENOMEM;
    }
    memset((char *)synopGMACadapter, 0, sizeof(struct synopGMACNetworkAdapter));

    synopGMACadapter->synopGMACdev    = NULL;

    synopGMACadapter->synopGMACdev = (synopGMACdevice *) plat_alloc_memory(sizeof(synopGMACdevice));
    if (!synopGMACadapter->synopGMACdev)
    {
        rt_kprintf("Error in Memory Allocataion, Founction : %s \n", __FUNCTION__);
        plat_free_memory(synopGMACadapter);
        return -RT_ENOMEM;
    }
    memset((char *)synopGMACadapter->synopGMACdev, 0, sizeof(synopGMACdevice));
    /*
     * Attach the device to MAC struct This will configure all the required base addresses
     * such as Mac base, configuration base, phy base address(out of 32 possible phys)
     * */
    if (synopGMAC_attach(synopGMACadapter->synopGMACdev, (regbase + MACBASE), regbase + DMABASE, DEFAULT_PHY_BASE, mac_addr0) != 0)
    {
        plat_free_memory(synopGMACadapter->synopGMACdev);
        plat_free_memory(synopGMACadapter);
        return -RT_ERROR;
    }

    init_phy(synopGMACadapter->synopGMACdev);
    synopGMAC_reset(synopGMACadapter->synopGMACdev);

    /* MII setup */
    synopGMACadapter->mii.phy_id_mask = 0x1F;
    synopGMACadapter->mii.reg_num_mask = 0x1F;
    synopGMACadapter->mii.dev = synopGMACadapter;
    synopGMACadapter->mii.mdio_read = mdio_read;
    synopGMACadapter->mii.mdio_write = mdio_write;
    synopGMACadapter->mii.phy_id = synopGMACadapter->synopGMACdev->PhyBase;
    synopGMACadapter->mii.supports_gmii = mii_check_gmii_support(&synopGMACadapter->mii);

    eth_dev.iobase = base_addr;
    eth_dev.name = "e0";
    eth_dev.priv = synopGMACadapter;
    eth_dev.dev_addr[0] = mac_addr0[0];
    eth_dev.dev_addr[1] = mac_addr0[1];
    eth_dev.dev_addr[2] = mac_addr0[2];
    eth_dev.dev_addr[3] = mac_addr0[3];
    eth_dev.dev_addr[4] = mac_addr0[4];
    eth_dev.dev_addr[5] = mac_addr0[5];

    eth_dev.parent.parent.type          = RT_Device_Class_NetIf;
#ifdef RT_USING_DEVICE_OPS
    eth_dev.parent.parent.ops           = &_gmac_dev_ops;
#else
    eth_dev.parent.parent.init          = eth_init;
    eth_dev.parent.parent.open          = eth_open;
    eth_dev.parent.parent.close         = eth_close;
    eth_dev.parent.parent.read          = eth_read;
    eth_dev.parent.parent.write         = eth_write;
    eth_dev.parent.parent.control       = eth_control;
#endif
    eth_dev.parent.parent.user_data     = RT_NULL;

    eth_dev.parent.eth_tx            = rt_eth_tx;
    eth_dev.parent.eth_rx            = rt_eth_rx;

    eth_device_init(&(eth_dev.parent), "e0");

#if defined(BSP_USING_GMAC) && defined(BSP_USING_RPMSG_NET)
    lynxi_eth0_apply_addr_and_sync_netdev(&eth_dev.parent);
#endif

    /* netif 注册完成后再开 DMA/IRQ，避免 tcpip 线程注册期间 ISR→rt_schedule 崩溃 */
    lynxi_gmac_rx_start();

    return 0;
}

/* 在 main() 中调用（SMP 从核已启动后再注册 netif，避免 rt_schedule current_thread==NULL） */

