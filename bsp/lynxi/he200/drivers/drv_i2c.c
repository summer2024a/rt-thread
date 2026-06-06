/*
 * DesignWare APB I2C master driver (polling) for HE200.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/misc.h>
#include "lynxi.h"
#include "drv_i2c.h"
#include "drv_reset.h"

#ifdef RT_USING_I2C

#define DW_IC_CON                0x00
#define DW_IC_TAR                0x04
#define DW_IC_DATA_CMD           0x10
#define DW_IC_SS_SCL_HCNT        0x14
#define DW_IC_SS_SCL_LCNT        0x18
#define DW_IC_FS_SCL_HCNT        0x1C
#define DW_IC_FS_SCL_LCNT        0x20
#define DW_IC_INTR_STAT          0x2C
#define DW_IC_CLR_INTR           0x40
#define DW_IC_CLR_TX_ABRT        0x54
#define DW_IC_CLR_STOP_DET       0x60
#define DW_IC_ENABLE             0x6C
#define DW_IC_STATUS             0x70
#define DW_IC_TXFLR              0x74
#define DW_IC_RXFLR              0x78
#define DW_IC_ENABLE_STATUS      0x9C

#define DW_IC_CON_MASTER         RT_BIT(0)
#define DW_IC_CON_SPEED_FAST     (2U << 1)
#define DW_IC_CON_RESTART_EN     RT_BIT(5)
#define DW_IC_CON_SLAVE_DISABLE  RT_BIT(6)
#define DW_IC_DATA_CMD_READ      RT_BIT(8)
#define DW_IC_DATA_CMD_STOP      RT_BIT(9)
#define DW_IC_DATA_CMD_RESTART   RT_BIT(10)
#define DW_IC_STATUS_ACTIVITY    RT_BIT(0)
#define DW_IC_STATUS_TFE         RT_BIT(2)
#define DW_IC_INTR_TX_ABRT       RT_BIT(6)
#define DW_IC_ENABLE_STATUS_EN   RT_BIT(0)

#define DW_I2C_TIMEOUT_LOOP      1000000U
#define DW_I2C_FIFO_DEPTH        16U

struct he200_i2c_bus
{
    struct rt_i2c_bus_device bus;
    rt_base_t base;
    enum lynxi_reset_line reset_line;
};

static struct he200_i2c_bus g_i2c_buses[4];

rt_inline rt_uint32_t reg_read(rt_base_t base, rt_uint32_t off)
{
    return HWREG32(base + off);
}

rt_inline void reg_write(rt_base_t base, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(base + off) = val;
}

static rt_err_t wait_idle(struct he200_i2c_bus *bus)
{
    rt_uint32_t t;

    for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
    {
        if ((reg_read(bus->base, DW_IC_STATUS) & DW_IC_STATUS_ACTIVITY) == 0)
        {
            return RT_EOK;
        }
    }
    return -RT_ETIMEOUT;
}

static rt_err_t wait_tfe(struct he200_i2c_bus *bus)
{
    rt_uint32_t t;

    for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
    {
        if (reg_read(bus->base, DW_IC_INTR_STAT) & DW_IC_INTR_TX_ABRT)
        {
            (void)reg_read(bus->base, DW_IC_CLR_TX_ABRT);
            return -RT_ERROR;
        }
        if (reg_read(bus->base, DW_IC_STATUS) & DW_IC_STATUS_TFE)
        {
            return RT_EOK;
        }
    }
    return -RT_ETIMEOUT;
}

static rt_err_t send_cmd(struct he200_i2c_bus *bus, rt_uint32_t cmd)
{
    rt_uint32_t t;

    for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
    {
        if (reg_read(bus->base, DW_IC_TXFLR) < DW_I2C_FIFO_DEPTH)
        {
            reg_write(bus->base, DW_IC_DATA_CMD, cmd);
            return RT_EOK;
        }
    }
    return -RT_ETIMEOUT;
}

static rt_err_t dw_i2c_disable(struct he200_i2c_bus *bus)
{
    rt_uint32_t t;

    reg_write(bus->base, DW_IC_ENABLE, 0);
    for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
    {
        if ((reg_read(bus->base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN) == 0)
        {
            return RT_EOK;
        }
    }
    return -RT_ETIMEOUT;
}

static rt_err_t dw_i2c_enable(struct he200_i2c_bus *bus)
{
    rt_uint32_t t;

    reg_write(bus->base, DW_IC_ENABLE, 1);
    for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
    {
        if (reg_read(bus->base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN)
        {
            return RT_EOK;
        }
    }
    return -RT_ETIMEOUT;
}

static rt_err_t xfer_write(struct he200_i2c_bus *bus, struct rt_i2c_msg *msg, rt_bool_t rs, rt_bool_t sp)
{
    rt_uint32_t n;

    if (msg->len == 0)
    {
        rt_uint32_t c = 0;

        if (rs)
        {
            c |= DW_IC_DATA_CMD_RESTART;
        }
        if (sp)
        {
            c |= DW_IC_DATA_CMD_STOP;
        }
        return send_cmd(bus, c);
    }

    for (n = 0; n < msg->len; n++)
    {
        rt_uint32_t c = msg->buf[n];

        if (n == 0 && rs)
        {
            c |= DW_IC_DATA_CMD_RESTART;
        }
        if (n == msg->len - 1 && sp)
        {
            c |= DW_IC_DATA_CMD_STOP;
        }
        if (send_cmd(bus, c) != RT_EOK)
        {
            return -RT_ETIMEOUT;
        }
    }
    return RT_EOK;
}

static rt_err_t xfer_read(struct he200_i2c_bus *bus, struct rt_i2c_msg *msg, rt_bool_t rs, rt_bool_t sp)
{
    rt_uint32_t n;

    for (n = 0; n < msg->len; n++)
    {
        rt_uint32_t c = DW_IC_DATA_CMD_READ;
        rt_uint32_t t;

        if (n == 0 && rs)
        {
            c |= DW_IC_DATA_CMD_RESTART;
        }
        if (n == msg->len - 1 && sp)
        {
            c |= DW_IC_DATA_CMD_STOP;
        }
        if (send_cmd(bus, c) != RT_EOK)
        {
            return -RT_ETIMEOUT;
        }

        for (t = 0; t < DW_I2C_TIMEOUT_LOOP; t++)
        {
            if (reg_read(bus->base, DW_IC_INTR_STAT) & DW_IC_INTR_TX_ABRT)
            {
                (void)reg_read(bus->base, DW_IC_CLR_TX_ABRT);
                return -RT_ERROR;
            }
            if (reg_read(bus->base, DW_IC_RXFLR) > 0)
            {
                msg->buf[n] = (rt_uint8_t)(reg_read(bus->base, DW_IC_DATA_CMD) & 0xff);
                break;
            }
        }
        if (t >= DW_I2C_TIMEOUT_LOOP)
        {
            return -RT_ETIMEOUT;
        }
    }
    return RT_EOK;
}

static rt_ssize_t master_xfer(struct rt_i2c_bus_device *dev, struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct he200_i2c_bus *bus = rt_container_of(dev, struct he200_i2c_bus, bus);
    rt_uint32_t k;

    if (!msgs || !num)
    {
        return -RT_EINVAL;
    }

    for (k = 0; k < num; k++)
    {
        rt_err_t err;

        if (wait_idle(bus) != RT_EOK)
        {
            return -RT_ETIMEOUT;
        }

        reg_write(bus->base, DW_IC_TAR, msgs[k].addr & 0x3ffU);
        if (msgs[k].flags & RT_I2C_RD)
        {
            err = xfer_read(bus, &msgs[k], k != 0, k == num - 1);
        }
        else
        {
            err = xfer_write(bus, &msgs[k], k != 0, k == num - 1);
        }
        if (err != RT_EOK)
        {
            return err;
        }
    }

    if (wait_tfe(bus) != RT_EOK)
    {
        return -RT_ERROR;
    }
    (void)reg_read(bus->base, DW_IC_CLR_STOP_DET);
    (void)reg_read(bus->base, DW_IC_CLR_INTR);

    return (rt_ssize_t)num;
}

static rt_ssize_t slave_xfer(struct rt_i2c_bus_device *dev, struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    RT_UNUSED(dev);
    RT_UNUSED(msgs);
    RT_UNUSED(num);
    return -RT_ENOSYS;
}

static rt_err_t bus_ctrl(struct rt_i2c_bus_device *dev, int cmd, void *args)
{
    RT_UNUSED(dev);
    RT_UNUSED(cmd);
    RT_UNUSED(args);
    return RT_EOK;
}

static const struct rt_i2c_bus_device_ops he200_i2c_ops =
{
    .master_xfer = master_xfer,
    .slave_xfer = slave_xfer,
    .i2c_bus_control = bus_ctrl,
};

static rt_err_t hw_init(struct he200_i2c_bus *bus)
{
    rt_err_t e;

    e = lynxi_reset_pulse(bus->reset_line, 1);
    if (e != RT_EOK)
    {
        return e;
    }

    if (dw_i2c_disable(bus) != RT_EOK)
    {
        return -RT_ETIMEOUT;
    }

    reg_write(bus->base, DW_IC_CON,
              DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE | DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_FAST);
    reg_write(bus->base, DW_IC_SS_SCL_HCNT, 250);
    reg_write(bus->base, DW_IC_SS_SCL_LCNT, 300);
    reg_write(bus->base, DW_IC_FS_SCL_HCNT, 60);
    reg_write(bus->base, DW_IC_FS_SCL_LCNT, 130);
    (void)reg_read(bus->base, DW_IC_CLR_INTR);

    return dw_i2c_enable(bus);
}

static rt_err_t register_one(struct he200_i2c_bus *bus, const char *name, rt_base_t base,
                             enum lynxi_reset_line reset_line)
{
    rt_err_t e;

    bus->base = base;
    bus->reset_line = reset_line;
    bus->bus.ops = &he200_i2c_ops;

    e = hw_init(bus);
    if (e != RT_EOK)
    {
        return e;
    }
    return rt_i2c_bus_device_register(&bus->bus, name);
}

int rt_hw_i2c_init(void)
{
    rt_err_t e = RT_EOK;

#ifdef BSP_USING_I2C0
    e = register_one(&g_i2c_buses[0], "i2c0", I2C0_BASE, LYNXI_RESET_I2C0);
    if (e != RT_EOK)
    {
        return (int)e;
    }
#endif
#ifdef BSP_USING_I2C1
    e = register_one(&g_i2c_buses[1], "i2c1", I2C1_BASE, LYNXI_RESET_I2C1);
    if (e != RT_EOK)
    {
        return (int)e;
    }
#endif
#ifdef BSP_USING_I2C2
    e = register_one(&g_i2c_buses[2], "i2c2", I2C2_BASE, LYNXI_RESET_I2C2);
    if (e != RT_EOK)
    {
        return (int)e;
    }
#endif
#ifdef BSP_USING_I2C3
    e = register_one(&g_i2c_buses[3], "i2c3", I2C3_BASE, LYNXI_RESET_I2C3);
    if (e != RT_EOK)
    {
        return (int)e;
    }
#endif

    return (int)e;
}
INIT_DEVICE_EXPORT(rt_hw_i2c_init);

#ifdef FINSH_USING_MSH
#include <stdlib.h>

static int i2c_probe(int argc, char *argv[])
{
    struct rt_i2c_bus_device *ibus;
    struct rt_i2c_msg msg;
    int addr;
    int found = 0;

    if (argc < 2)
    {
        rt_kprintf("Usage: i2c_probe i2c0|i2c1|i2c2|i2c3\n");
        return -RT_EINVAL;
    }

    ibus = rt_i2c_bus_device_find(argv[1]);
    if (!ibus)
    {
        rt_kprintf("Bus %s not found\n", argv[1]);
        return -RT_ERROR;
    }

    msg.flags = RT_I2C_WR;
    msg.buf = RT_NULL;
    msg.len = 0;
    for (addr = 0x03; addr <= 0x77; addr++)
    {
        msg.addr = (rt_uint16_t)addr;
        if (rt_i2c_transfer(ibus, &msg, 1) == 1)
        {
            rt_kprintf("Found device at 0x%02X\n", addr);
            found++;
        }
    }
    rt_kprintf("Scan complete, found %d device(s)\n", found);
    return 0;
}
MSH_CMD_EXPORT(i2c_probe, scan I2C bus (address probe));
#endif /* FINSH_USING_MSH */

#endif /* RT_USING_I2C */
