/*
 * Synopsys DesignWare AXI DMAC — channel0 memory-to-memory (contiguous block).
 * Register map aligned with Linux drivers/dma/dw-axi-dmac/dw-axi-dmac.h.
 *
 * Callers must supply DMA-visible buffers (identity-mapped or coherent); this
 * driver writes full 64-bit SAR/DAR when the IP uses 64-bit address registers.
 */

#include <rtthread.h>
#include <string.h>
#include "lynxi.h"
#include "drv_reset.h"

extern void *rt_kmem_v2p(void *vaddr);
#ifndef ARCH_MAP_FAILED
#define ARCH_MAP_FAILED ((void *)0x1ffffffffffffULL)
#endif

#ifdef BSP_USING_DW_AXI_DMA

#define DMAC_CFG                 0x010U
#define DMAC_CHEN                0x018U
#define DMAC_EN                  (1U << 0)

#define CHAN0_OFF                0x100U
#define CH_SAR                   0x000U
#define CH_DAR                   0x008U
#define CH_BLOCK_TS              0x010U
#define CH_CTL_L                 0x018U
#define CH_CTL_H                 0x01CU
#define CH_CFG_L                 0x020U
#define CH_CFG_H                 0x024U
#define CH_LLP                   0x028U
#define CH_INTSTATUS             0x088U
#define CH_INTCLEAR              0x098U

#define DMAC_CHAN_EN_SHIFT       0
#define DMAC_CHAN_EN_WE_SHIFT    8

#define CH_CFG_H_TT_FC_POS           0
#define CH_CFG_H_PRIORITY_POS        17
#define CH_CFG_H_HS_SEL_DST_POS      4
#define CH_CFG_H_HS_SEL_SRC_POS      3
#define CH_CFG_H_SRC_OSR_LMT_POS     23
#define CH_CFG_H_DST_OSR_LMT_POS     27

#define DWAXIDMAC_TT_FC_MEM_TO_MEM_DMAC  0U
#define DWAXIDMAC_HS_SEL_HW              0U
#define DWAXIDMAC_HS_SEL_SW              1U

#define CH_CTL_H_LLI_LAST          (1U << 30)
#define CH_CTL_H_LLI_VALID         (1U << 31)

#define CH_CTL_L_DST_MSIZE_POS     18
#define CH_CTL_L_SRC_MSIZE_POS     14
#define CH_CTL_L_DST_WIDTH_POS     11
#define CH_CTL_L_SRC_WIDTH_POS     8
#define CH_CTL_L_DST_INC_POS       6
#define CH_CTL_L_SRC_INC_POS       4
#define CH_CTL_L_AR_POS            22
#define CH_CTL_L_AW_POS            26

#define DWAXIDMAC_BURST_TRANS_LEN_1      0U
#define DWAXIDMAC_CH_CTL_L_INC           0U
#define DWAXIDMAC_CH_CTL_AR_CACHE        2U
#define DWAXIDMAC_CH_CTL_AW_CACHE        2U

#define DWAXIDMAC_IRQ_BLOCK_TRF    (1U << 0)
#define DWAXIDMAC_IRQ_DMA_TRF      (1U << 1)
/* Errors: bits 5-14 and 16-21 (same grouping as Linux DWAXIDMAC_IRQ_ALL_ERR) */
#define DWAXIDMAC_IRQ_ERR_MASK     (0x003FFFE0U)

/*
 * Align Linux device-tree setting:
 * snps,block-size = <1024 ...> for each channel on this platform.
 */
#define DW_DMA_MAX_BLOCK_BYTES     1024U
#define DW_DMA_POLL_LOOPS          1000000U

static rt_bool_t _dma_ready;

rt_inline void dma_w(rt_uint32_t off, rt_uint32_t v)
{
    *(volatile rt_uint32_t *)(rt_uintptr_t)(DW_AXI_DMA_BASE + off) = v;
}

rt_inline rt_uint32_t dma_r(rt_uint32_t off)
{
    return *(volatile rt_uint32_t *)(rt_uintptr_t)(DW_AXI_DMA_BASE + off);
}

rt_inline void dma_w64(rt_uint32_t off, rt_ubase_t addr)
{
    dma_w(off, (rt_uint32_t)(addr & 0xFFFFFFFFUL));
    dma_w(off + 4U, (rt_uint32_t)((addr >> 32) & 0xFFFFFFFFUL));
}

rt_inline void ch0_w(rt_uint32_t ch_off, rt_uint32_t v)
{
    dma_w(CHAN0_OFF + ch_off, v);
}

rt_inline rt_uint32_t ch0_r(rt_uint32_t ch_off)
{
    return dma_r(CHAN0_OFF + ch_off);
}

static rt_bool_t ch0_hw_enabled(void)
{
    rt_uint32_t v = dma_r(DMAC_CHEN);

    return (v & (1U << DMAC_CHAN_EN_SHIFT)) != 0;
}

static void ch0_disable_write(void)
{
    rt_uint32_t v = dma_r(DMAC_CHEN);

    v &= ~(1U << DMAC_CHAN_EN_SHIFT);
    v |= (1U << DMAC_CHAN_EN_WE_SHIFT);
    dma_w(DMAC_CHEN, v);
}

static rt_err_t ch0_wait_disabled(void)
{
    rt_uint32_t n = DW_DMA_POLL_LOOPS;

    while (ch0_hw_enabled() && n > 0U)
    {
        n--;
    }
    return ch0_hw_enabled() ? -RT_ETIMEOUT : RT_EOK;
}

static void ch0_enable(void)
{
    rt_uint32_t v = dma_r(DMAC_CHEN);

    v |= (1U << DMAC_CHAN_EN_SHIFT);
    v |= (1U << DMAC_CHAN_EN_WE_SHIFT);
    dma_w(DMAC_CHEN, v);
}

static rt_uint32_t build_cfg_h(rt_uint32_t hs_hw)
{
    rt_uint32_t hs_sel_dst = hs_hw ? DWAXIDMAC_HS_SEL_HW : DWAXIDMAC_HS_SEL_SW;
    rt_uint32_t hs_sel_src = hs_hw ? DWAXIDMAC_HS_SEL_HW : DWAXIDMAC_HS_SEL_SW;

    return (DWAXIDMAC_TT_FC_MEM_TO_MEM_DMAC << CH_CFG_H_TT_FC_POS)
           | (0U << CH_CFG_H_PRIORITY_POS)
           | (hs_sel_dst << CH_CFG_H_HS_SEL_DST_POS)
           | (hs_sel_src << CH_CFG_H_HS_SEL_SRC_POS)
           | (15U << CH_CFG_H_SRC_OSR_LMT_POS)
           | (15U << CH_CFG_H_DST_OSR_LMT_POS);
}

static rt_uint32_t build_ctl_lo_byte_burst(void)
{
    return (DWAXIDMAC_BURST_TRANS_LEN_1 << CH_CTL_L_DST_MSIZE_POS)
           | (DWAXIDMAC_BURST_TRANS_LEN_1 << CH_CTL_L_SRC_MSIZE_POS)
           | (0U << CH_CTL_L_DST_WIDTH_POS)
           | (0U << CH_CTL_L_SRC_WIDTH_POS)
           | (DWAXIDMAC_CH_CTL_L_INC << CH_CTL_L_DST_INC_POS)
           | (DWAXIDMAC_CH_CTL_L_INC << CH_CTL_L_SRC_INC_POS)
           | (DWAXIDMAC_CH_CTL_AR_CACHE << CH_CTL_L_AR_POS)
           | (DWAXIDMAC_CH_CTL_AW_CACHE << CH_CTL_L_AW_POS);
}

static rt_err_t ch0_memcpy_one(rt_ubase_t src, rt_ubase_t dst, rt_size_t nbytes, rt_uint32_t hs_hw)
{
    rt_uint32_t loops;
    rt_uint32_t block_ts;
    rt_uint32_t istat;
    rt_err_t werr;

    if (nbytes == 0U || nbytes > DW_DMA_MAX_BLOCK_BYTES)
    {
        return -RT_EINVAL;
    }

    block_ts = (rt_uint32_t)nbytes;
    ch0_disable_write();
    werr = ch0_wait_disabled();
    if (werr != RT_EOK)
    {
        return werr;
    }

    ch0_w(CH_INTCLEAR, 0xFFFFFFFFU);
    ch0_w(CH_CFG_L, 0U);
    ch0_w(CH_CFG_H, build_cfg_h(hs_hw));
    dma_w64(CHAN0_OFF + CH_LLP, 0);

    dma_w64(CHAN0_OFF + CH_SAR, src);
    dma_w64(CHAN0_OFF + CH_DAR, dst);
    ch0_w(CH_BLOCK_TS, block_ts - 1U);

    ch0_w(CH_CTL_L, build_ctl_lo_byte_burst());
    /* single-block shadow register mode, no LLP chain */
    ch0_w(CH_CTL_H, 0U);

    loops = DW_DMA_POLL_LOOPS;
    ch0_enable();
    do
    {
        istat = ch0_r(CH_INTSTATUS);
        if (istat & DWAXIDMAC_IRQ_ERR_MASK)
        {
            ch0_w(CH_INTCLEAR, istat);
            ch0_disable_write();
            (void)ch0_wait_disabled();
            return -RT_ERROR;
        }
        if (istat & (DWAXIDMAC_IRQ_DMA_TRF | DWAXIDMAC_IRQ_BLOCK_TRF))
        {
            break;
        }
    } while (loops-- > 0U);

    if (loops == 0U)
    {
        ch0_disable_write();
        (void)ch0_wait_disabled();
        return -RT_ETIMEOUT;
    }

    ch0_w(CH_INTCLEAR, istat);
    ch0_disable_write();
    werr = ch0_wait_disabled();
    if (werr != RT_EOK)
    {
        return werr;
    }

    return RT_EOK;
}

int lynxi_dw_axi_dma_memcpy(void *dst, const void *src, rt_size_t size)
{
    rt_ubase_t s = (rt_ubase_t)src;
    rt_ubase_t d = (rt_ubase_t)dst;
    rt_ubase_t s_pa;
    rt_ubase_t d_pa;
    rt_size_t done = 0;
    rt_err_t err = RT_EOK;

    if ((dst == RT_NULL) || (src == RT_NULL) || size == 0)
    {
        return -RT_EINVAL;
    }

    if (!_dma_ready)
    {
        rt_memcpy(dst, src, size);
        return RT_EOK;
    }

    while (done < size)
    {
        rt_size_t chunk = size - done;
        rt_uint32_t try_hw = 1U;

        if (chunk > DW_DMA_MAX_BLOCK_BYTES)
        {
            chunk = DW_DMA_MAX_BLOCK_BYTES;
        }

        s_pa = (rt_ubase_t)rt_kmem_v2p((void *)(rt_uintptr_t)(s + done));
        d_pa = (rt_ubase_t)rt_kmem_v2p((void *)(rt_uintptr_t)(d + done));
        if ((s_pa == (rt_ubase_t)ARCH_MAP_FAILED) || (d_pa == (rt_ubase_t)ARCH_MAP_FAILED))
        {
            rt_memcpy((rt_uint8_t *)dst + done, (const rt_uint8_t *)src + done, size - done);
            rt_kprintf("dw_axi_dma: v2p failed, cpu fallback (off=%u)\n", (unsigned)done);
            return RT_EOK;
        }

        /*
         * Keep CPU cache coherent with DMA engine:
         * - clean source lines so DMA reads latest data
         * - invalidate destination lines before/after transfer
         */
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)(rt_uintptr_t)(s + done), (int)chunk);
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)(rt_uintptr_t)(d + done), (int)chunk);

        err = ch0_memcpy_one(s_pa, d_pa, chunk, try_hw);
        if (err != RT_EOK)
        {
            err = ch0_memcpy_one(s_pa, d_pa, chunk, 0U);
        }
        if (err != RT_EOK)
        {
            rt_memcpy((rt_uint8_t *)dst + done, (const rt_uint8_t *)src + done, size - done);
            rt_kprintf("dw_axi_dma: hw memcpy failed (%d), cpu fallback\n", err);
            return RT_EOK;
        }

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)(rt_uintptr_t)(d + done), (int)chunk);
        if (rt_memcmp((const void *)(rt_uintptr_t)(s + done), (const void *)(rt_uintptr_t)(d + done), chunk) != 0)
        {
            rt_memcpy((rt_uint8_t *)dst + done, (const rt_uint8_t *)src + done, size - done);
            rt_kprintf("dw_axi_dma: hw memcpy verify failed, cpu fallback (off=%u len=%u)\n",
                       (unsigned)done, (unsigned)chunk);
            return RT_EOK;
        }
        done += chunk;
    }

    return RT_EOK;
}

static int rt_hw_dw_axi_dma_init(void)
{
    (void)lynxi_reset_pulse(LYNXI_RESET_DMA, 1U);
    ch0_disable_write();
    (void)ch0_wait_disabled();
    dma_w(DMAC_CFG, DMAC_EN);
    _dma_ready = RT_TRUE;
    return RT_EOK;
}
INIT_BOARD_EXPORT(rt_hw_dw_axi_dma_init);

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

static void dma_memcpy_test(int argc, char **argv)
{
    char *src;
    char *dst;
    rt_size_t i;
    rt_size_t n = 4096;

    if (argc > 1)
    {
        int v = atoi(argv[1]);
        if (v > 0 && (rt_size_t)v <= DW_DMA_MAX_BLOCK_BYTES)
        {
            n = (rt_size_t)v;
        }
    }

    src = (char *)rt_malloc(n);
    dst = (char *)rt_malloc(n);
    if ((src == RT_NULL) || (dst == RT_NULL))
    {
        rt_kprintf("dw_axi_dma memcpy test: alloc failed (len=%u)\n", (unsigned)n);
        if (src) rt_free(src);
        if (dst) rt_free(dst);
        return;
    }

    for (i = 0; i < n; i++)
    {
        src[i] = (char)((i ^ 0x5A) & 0xFF);
        dst[i] = 0;
    }

    if (lynxi_dw_axi_dma_memcpy(dst, src, n) != RT_EOK)
    {
        rt_kprintf("dw_axi_dma memcpy test: api error\n");
        rt_free(src);
        rt_free(dst);
        return;
    }
    if (memcmp(src, dst, n) == 0)
    {
        rt_kprintf("dw_axi_dma memcpy test pass (len=%u)\n", (unsigned)n);
    }
    else
    {
        rt_size_t bad = 0;
        while (bad < n && src[bad] == dst[bad])
        {
            bad++;
        }
        rt_kprintf("dw_axi_dma memcpy test fail (len=%u, first_bad=%u, src=%02x dst=%02x)\n",
                   (unsigned)n,
                   (unsigned)bad,
                   (unsigned char)src[bad],
                   (unsigned char)dst[bad]);
    }

    rt_free(src);
    rt_free(dst);
}
MSH_CMD_EXPORT(dma_memcpy_test, test dw axi dma memcpy: dma_memcpy_test [len]);
#endif

#endif /* BSP_USING_DW_AXI_DMA */
