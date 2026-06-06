#include <rtthread.h>
#include <rtdevice.h>
#include <time.h>
#include "lynxi.h"
#include "drv_reset.h"

#ifdef BSP_USING_RTC

#define DW_RTC_CCVR     0x00
#define DW_RTC_CMR      0x04
#define DW_RTC_CLR      0x08
#define DW_RTC_CCR      0x0C
#define DW_RTC_STAT     0x10
#define DW_RTC_EOI      0x18
#define DW_RTC_CPSR     0x20
#define DW_RTC_CPCVR    0x24

/* Linux rtc-lynxi.c uses bit2 as counter enable. */
#define DW_RTC_CCR_EN   (1U << 2)
#define DW_RTC_CPSR_32K (32768U)

static rt_rtc_dev_t _rtc_dev;

static inline rt_uint32_t rtc_read(rt_uint32_t off)
{
    return *(volatile rt_uint32_t *)(rt_uintptr_t)(RTC_BASE + off);
}

static inline void rtc_write(rt_uint32_t off, rt_uint32_t val)
{
    *(volatile rt_uint32_t *)(rt_uintptr_t)(RTC_BASE + off) = val;
}

static rt_err_t _rtc_init(void)
{
    /*
     * Align Linux behavior:
     * 1) keep RTC deasserted
     * 2) program CPSR (board uses 32k source)
     * 3) enable counter by CCR.EN(bit2)
     */
    (void)lynxi_reset_deassert(LYNXI_RESET_RTC);
    rt_hw_us_delay(10U);
    rtc_write(DW_RTC_CPSR, DW_RTC_CPSR_32K);
    (void)rtc_read(DW_RTC_CPSR);
    rtc_write(DW_RTC_CCR, DW_RTC_CCR_EN);
    (void)rtc_read(DW_RTC_CCR);
    return RT_EOK;
}

static rt_err_t _rtc_get_secs(time_t *sec)
{
    *sec = (time_t)rtc_read(DW_RTC_CCVR);
    return RT_EOK;
}

static rt_err_t _rtc_set_secs(time_t *sec)
{
    rtc_write(DW_RTC_CCR, 0U);
    rtc_write(DW_RTC_CLR, (rt_uint32_t)(*sec));
    (void)rtc_read(DW_RTC_CLR); /* force write completion */
    rtc_write(DW_RTC_CCR, DW_RTC_CCR_EN);
    return RT_EOK;
}

static const struct rt_rtc_ops _rtc_ops =
{
    .init = _rtc_init,
    .get_secs = _rtc_get_secs,
    .set_secs = _rtc_set_secs,
    .get_alarm = RT_NULL,
    .set_alarm = RT_NULL,
    .get_timeval = RT_NULL,
    .set_timeval = RT_NULL,
};

int rt_hw_rtc_init(void)
{
    _rtc_dev.ops = &_rtc_ops;
    return rt_hw_rtc_register(&_rtc_dev, "rtc", RT_DEVICE_FLAG_RDWR, RT_NULL);
}
INIT_BOARD_EXPORT(rt_hw_rtc_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

static void rtc_dump(void)
{
    rt_uint32_t cpcvr0;
    rt_uint32_t cpcvr1;

    rt_kprintf("RTC_CCVR : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CCVR));
    rt_kprintf("RTC_CMR  : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CMR));
    rt_kprintf("RTC_CLR  : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CLR));
    rt_kprintf("RTC_CCR  : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CCR));
    rt_kprintf("RTC_STAT : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_STAT));
    rt_kprintf("RTC_CPSR : 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CPSR));
    rt_kprintf("RTC_CPCVR: 0x%08x\n", (unsigned int)rtc_read(DW_RTC_CPCVR));
    cpcvr0 = rtc_read(DW_RTC_CPCVR);
    rt_hw_us_delay(200U);
    cpcvr1 = rtc_read(DW_RTC_CPCVR);
    rt_kprintf("RTC_RUN  : %s (cpcvr %08x -> %08x)\n",
               (cpcvr0 != cpcvr1) ? "yes" : "no",
               (unsigned int)cpcvr0,
               (unsigned int)cpcvr1);
}
MSH_CMD_EXPORT(rtc_dump, dump RTC registers);
#endif

#endif /* BSP_USING_RTC */
