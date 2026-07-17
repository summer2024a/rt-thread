/*
 * biz_finsh_cmds.c — hp640-style MSH debug commands (one module at a time).
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_FINSH

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>
#include "biz_exec_handlers.h"
#include "biz_emmc.h"
#include "drv_emmc.h"
#include "drv_flash.h"
#include "biz_log.h"
#include "biz_config.h"
#include "biz_host_proto.h"
#include "biz_error_code.h"
#include "biz_subsys.h"
#include "board.h"
#include "drv_i2c.h"
#ifdef RT_USING_RYM
#include <ymodem.h>
#endif
#ifdef RT_USING_ZMODEM
#include "zdef.h"
#endif

#ifdef BIZ_MOD_EXEC_SELFTEST
static int cmd_self_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return biz_self_test_report();
}
MSH_CMD_EXPORT_ALIAS(cmd_self_test, self_test, self_test setup test);
#endif

#ifdef BIZ_MOD_EXEC_STRESS
static int cmd_stress(int argc, char **argv)
{
    HP640_Task task;

    memset(&task, 0, sizeof(task));
    task.type = 3;
    task.blk_cnt = 1;
    task.times = 1000;
    task.src_addr = 0x800000UL;

    if (argc > 1)
    {
        if (strcmp(argv[1], "r") == 0)
            task.type = 1;
        else if (strcmp(argv[1], "w") == 0)
            task.type = 2;
        else if (strcmp(argv[1], "rw") == 0)
            task.type = 3;
    }
    if (argc > 2)
        task.blk_cnt = (unsigned short)strtoul(argv[2], RT_NULL, 10);
    if (argc > 3)
        task.times = strtoul(argv[3], RT_NULL, 10);
    if (argc > 4)
        task.src_addr = strtoul(argv[4], RT_NULL, 16);

    return biz_exec_stress(&task);
}
MSH_CMD_EXPORT_ALIAS(cmd_stress, stress, stress r/w/rw blk_cnt times emmc_addr);
#endif

#ifdef BIZ_MOD_PCIE
static int cmd_pcie(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("pcie <mode|dma> ...\n");
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "mode") == 0)
    {
        if (argc < 3)
            return -RT_ERROR;
        if (strcmp(argv[2], "rc") == 0 || strcmp(argv[2], "RC") == 0)
            return drv_pcie_set_mode(HP640_PCIE_MODE_RC);
        if (strcmp(argv[2], "ep") == 0 || strcmp(argv[2], "EP") == 0)
            return drv_pcie_set_mode(HP640_PCIE_MODE_EP);
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "dma") == 0)
    {
        uint32_t size = 0x40000U;
        unsigned long times = 1;

        if (argc > 2)
            size = (uint32_t)strtoul(argv[2], RT_NULL, 16);
        if (argc > 3)
            times = strtoul(argv[3], RT_NULL, 10);
        return drv_pcie_dma_test(size, times);
    }

    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(cmd_pcie, pcie, pcie mode or dma test);
#endif

#ifdef BIZ_MOD_EMMC_DLL
static int cmd_emmc_dll(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return biz_exec_emmc_dll_scan();
}
MSH_CMD_EXPORT_ALIAS(cmd_emmc_dll, emmc_dll, eMMC DLL scan);
#endif

static int cmd_heart_beat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Soft clock setup if BSP_BIZ_SKIP_THREADS deferred drv_emmc_init */
    (void)drv_emmc_init();
    return biz_emmc_report_heartbeat();
}
MSH_CMD_EXPORT_ALIAS(cmd_heart_beat, heart_beat, report heart-beat to eMMC);

/*
 * Manual biz bring-up — compiled only when BSP_BIZ_SKIP_THREADS
 * (auto-start off). Production leaves the macro unset → no msh start.
 *   biz start [cpu]     — emmc + emmc_biz on cpu (default 1)
 *   biz upgrade [cpu]   — flash init + worker + biz start (Host path)
 *   biz status          — always available
 */
static int cmd_biz(int argc, char **argv)
{
    if (argc < 2)
    {
#ifdef BSP_BIZ_SKIP_THREADS
        rt_kprintf("biz start [cpu]    — start emmc_biz (default cpu=1)\n");
        rt_kprintf("biz upgrade [cpu]  — flash init+worker + biz start\n");
#endif
        rt_kprintf("biz status\n");
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "status") == 0)
    {
        rt_kprintf("emmc_hw=%d emmc_biz=%d\n",
                   biz_emmc_hw_is_ready(), biz_emmc_biz_is_running());
#ifdef BSP_FLASH_DEFER_INIT
        rt_kprintf("BSP_FLASH_DEFER_INIT=1\n");
#endif
#ifdef BSP_BIZ_SKIP_THREADS
        rt_kprintf("BSP_BIZ_SKIP_THREADS=1\n");
#endif
        return 0;
    }

#ifdef BSP_BIZ_SKIP_THREADS
    {
        int cpu = 1;

        if (argc > 2)
            cpu = (int)strtol(argv[2], RT_NULL, 0);

        if (strcmp(argv[1], "start") == 0)
            return biz_emmc_biz_start_on_cpu(cpu);

        if (strcmp(argv[1], "upgrade") == 0)
        {
            if (drv_flash_bringup() != 0)
            {
                rt_kprintf("biz upgrade: flash init failed\n");
                return -RT_ERROR;
            }
            if (biz_flash_worker_ensure() != 0)
            {
                rt_kprintf("biz upgrade: flash worker failed\n");
                return -RT_ERROR;
            }
            return biz_emmc_biz_start_on_cpu(cpu);
        }
    }
#endif /* BSP_BIZ_SKIP_THREADS */

    rt_kprintf("biz: unknown '%s'\n", argv[1]);
    return -RT_ERROR;
}
#ifdef BSP_BIZ_SKIP_THREADS
MSH_CMD_EXPORT_ALIAS(cmd_biz, biz, biz start|upgrade|status);
#else
MSH_CMD_EXPORT_ALIAS(cmd_biz, biz, biz status);
#endif

#ifdef BSP_I2C_DEFER
/*
 * Manual MCU I2C bring-up — only when BSP_I2C_DEFER.
 * Workaround: HP2320 MCU may hang KA200 I2C after addr query + reset.
 */
static int cmd_i2c(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("i2c start   — init DW I2C0 slave + i2c_mcu/mcu_err threads\n");
        rt_kprintf("i2c status\n");
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "status") == 0)
    {
        rt_kprintf("i2c_ready=%d addr=0x%02x BSP_I2C_DEFER=1\n",
                   drv_i2c_is_ready(),
                   drv_i2c_is_ready() ? drv_i2c_mcu_get_addr() : 0);
        return 0;
    }

    if (strcmp(argv[1], "start") == 0)
        return biz_i2c_ensure();

    rt_kprintf("i2c: unknown '%s'\n", argv[1]);
    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(cmd_i2c, i2c, i2c start|status);
#endif /* BSP_I2C_DEFER */

#ifdef BIZ_MOD_FLASH_UPGRADE
static void flash_dump_hex(uint32_t addr, const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        if ((i % 16) == 0)
            rt_kprintf("\n%08x: ", addr + (uint32_t)i);
        rt_kprintf("%02x ", buf[i]);
    }
    rt_kprintf("\n");
}

static int flash_cmd_check_bootcode(uint32_t addr, int len, int is_write)
{
    if (!drv_flash_bootcode_overlap(addr, (uint32_t)len, is_write))
        return 0;

    rt_kprintf("flash: refuse %s @0x%x len=%d (XIP bootcode [0,0x%x) or erase overlap)\n",
               is_write ? "write/erase" : "op",
               addr, len, (unsigned)DRV_FLASH_XIP_BOOTCODE_END);
    rt_kprintf("flash: use test addr 0x%x like hp640\n", (unsigned)DRV_FLASH_TEST_ADDR);
    return -RT_ERROR;
}

#if defined(RT_USING_RYM) || defined(RT_USING_ZMODEM)
enum flash_update_proto
{
    FLASH_UPDATE_ZMODEM = 0,
    FLASH_UPDATE_YMODEM = 1,
};

#ifdef RT_USING_RYM
struct flash_update_ctx
{
    struct rym_ctx parent;
    uint8_t *buf;
    uint32_t capacity;
    uint32_t offset;
};

static enum rym_code flash_update_on_begin(struct rym_ctx *ctx,
                                           rt_uint8_t *buf, rt_size_t len)
{
    struct flash_update_ctx *c = (struct flash_update_ctx *)ctx;
    (void)buf;
    (void)len;
    c->offset = 0;
    return RYM_CODE_ACK;
}

static enum rym_code flash_update_on_data(struct rym_ctx *ctx,
                                          rt_uint8_t *buf, rt_size_t len)
{
    struct flash_update_ctx *c = (struct flash_update_ctx *)ctx;

    if (c->offset + (uint32_t)len > c->capacity)
        return RYM_CODE_CAN;
    memcpy(c->buf + c->offset, buf, len);
    c->offset += (uint32_t)len;
    return RYM_CODE_ACK;
}

static enum rym_code flash_update_on_end(struct rym_ctx *ctx,
                                         rt_uint8_t *buf, rt_size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return RYM_CODE_ACK;
}
#endif

static int flash_cmd_update(uint32_t flash_addr, uint32_t range, int proto)
{
    rt_device_t cons;
    int wret;
    uint32_t recv_len = 0;
    uint8_t *scratch = (uint8_t *)(uintptr_t)IRAM1_HOST_SCRATCH_START;

    if (range == 0 || range > (uint32_t)IRAM1_HOST_SCRATCH_SIZE)
    {
        rt_kprintf("flash update: range must be 1..0x%x\n",
                   (unsigned)IRAM1_HOST_SCRATCH_SIZE);
        return -RT_ERROR;
    }

    if (!drv_flash_is_known() && drv_flash_bringup() != 0)
    {
        rt_kprintf("flash update: bringup failed\n");
        return -RT_ERROR;
    }

    if (drv_flash_bootcode_overlap(flash_addr, range, 1))
        rt_kprintf("flash update: WARNING writing boot region @0x%x len=0x%x\n",
                   flash_addr, range);

    memset(scratch, 0, range);

    cons = rt_console_get_device();
    if (cons == RT_NULL)
        cons = rt_device_find(RT_CONSOLE_DEVICE_NAME);
    if (cons == RT_NULL)
    {
        rt_kprintf("flash update: no console device\n");
        return -RT_ERROR;
    }

#ifdef RT_USING_ZMODEM
    if (proto == FLASH_UPDATE_ZMODEM)
    {
        rt_err_t zret;

        rt_kprintf("flash update: Zmodem → scratch@0x%llx then NOR@0x%x len=0x%x\n",
                   (unsigned long long)IRAM1_HOST_SCRATCH_START,
                   flash_addr, range);
        rt_kprintf("flash update: start Zmodem send (sz / SecureCRT Zmodem)...\n");

        zret = zm_recv_to_mem(cons, scratch, range, &recv_len);
        if (zret != RT_EOK)
        {
            rt_kprintf("flash update: Zmodem fail err=%d recv=0x%x\n",
                       (int)zret, recv_len);
            return (int)zret;
        }
        rt_kprintf("flash update: Zmodem OK recv=0x%x, programming...\n",
                   recv_len);
    }
    else
#endif
#ifdef RT_USING_RYM
    if (proto == FLASH_UPDATE_YMODEM)
    {
        struct flash_update_ctx uctx;
        rt_err_t yret;

        rt_kprintf("flash update: Ymodem → scratch@0x%llx then NOR@0x%x len=0x%x\n",
                   (unsigned long long)IRAM1_HOST_SCRATCH_START,
                   flash_addr, range);
        rt_kprintf("flash update: start Ymodem send from host...\n");

        memset(&uctx, 0, sizeof(uctx));
        uctx.buf = scratch;
        uctx.capacity = range;
        uctx.offset = 0;

        yret = rym_recv_on_device(&uctx.parent, cons,
                                  RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX,
                                  flash_update_on_begin, flash_update_on_data,
                                  flash_update_on_end, 60);
        if (yret != RT_EOK)
        {
            rt_kprintf("flash update: Ymodem fail stage=%d err=%d recv=0x%x\n",
                       (int)uctx.parent.stage, (int)yret, uctx.offset);
            return (int)yret;
        }
        recv_len = uctx.offset;
        rt_kprintf("flash update: Ymodem OK recv=0x%x, programming...\n",
                   recv_len);
    }
    else
#endif
    {
        rt_kprintf("flash update: protocol not built-in\n");
        return -RT_ERROR;
    }

    /* Same as hp640: program full <range> (pre-cleared scratch). */
    wret = drv_flash_write(scratch, (int)range, flash_addr);
    if (wret != 0)
    {
        rt_kprintf("flash update: write fail ret=%d\n", wret);
        return wret;
    }

    rt_kprintf("flash update: OK NOR@0x%x len=0x%x (recv=0x%x)\n",
               flash_addr, range, recv_len);
    return 0;
}
#endif /* RYM || ZMODEM */

static int cmd_flash(int argc, char **argv)
{
    uint32_t addr = DRV_FLASH_TEST_ADDR;
    int len = 16;

    if (argc < 2)
    {
        rt_kprintf("flash info | status\n");
        rt_kprintf("flash ssi_ctrl [val]     — read/write 0x12600024\n");
        rt_kprintf("flash open_flash         — inv+B8+98 (hp640)\n");
        rt_kprintf("flash bind               — set SSI VA from strap (no MMIO)\n");
        rt_kprintf("flash peek               — read CTRL0/SR (may SError)\n");
        rt_kprintf("flash jedec              — JEDEC id\n");
#ifdef BSP_FLASH_DEFER_INIT
        rt_kprintf("flash init               — open+bind+jedec (deferred boot)\n");
        rt_kprintf("flash worker             — start CPU0 flash bounce worker\n");
#endif
#ifdef RT_USING_ZMODEM
        rt_kprintf("flash update <addr> <range>  — Zmodem then write NOR\n");
#endif
#ifdef RT_USING_RYM
        rt_kprintf("flash updatey <addr> <range> — Ymodem then write NOR (hp640)\n");
#endif
        rt_kprintf("flash read|write|erase|test|ssi_probe ...\n");
        rt_kprintf("bootcode protected: [0, 0x%x) (except update)\n",
                   (unsigned)DRV_FLASH_XIP_BOOTCODE_END);
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "info") == 0 || strcmp(argv[1], "status") == 0)
    {
        drv_flash_dbg_status();
        rt_kprintf("flash bootcode_end=0x%x test=0x%x\n",
                   (unsigned)DRV_FLASH_XIP_BOOTCODE_END,
                   (unsigned)DRV_FLASH_TEST_ADDR);
        return 0;
    }

    if (strcmp(argv[1], "ssi_ctrl") == 0)
    {
        uint32_t v;

        if (argc > 2)
        {
            v = (uint32_t)strtoul(argv[2], RT_NULL, 0);
            v = drv_flash_dbg_ssi_ctrl(1, v);
            rt_kprintf("[flash] ssi_ctrl wrote, now=0x%08x\n", v);
        }
        else
        {
            v = drv_flash_dbg_ssi_ctrl(0, 0);
            rt_kprintf("[flash] ssi_ctrl=0x%08x\n", v);
        }
        return 0;
    }

    if (strcmp(argv[1], "open_flash") == 0)
    {
        drv_flash_dbg_open_flash();
        return 0;
    }

    if (strcmp(argv[1], "bind") == 0)
    {
        drv_flash_dbg_bind();
        return 0;
    }

    if (strcmp(argv[1], "peek") == 0)
        return drv_flash_dbg_peek();

    if (strcmp(argv[1], "jedec") == 0)
        return drv_flash_dbg_jedec();

#ifdef BSP_FLASH_DEFER_INIT
    if (strcmp(argv[1], "init") == 0)
        return drv_flash_dbg_init_now();

    if (strcmp(argv[1], "worker") == 0)
    {
        if (biz_flash_worker_ensure() != 0)
            return -RT_ERROR;
        return 0;
    }
#endif /* BSP_FLASH_DEFER_INIT */

    if (strcmp(argv[1], "ssi_probe") == 0)
    {
        int cpu = 1;

        if (argc > 2)
            cpu = (int)strtoul(argv[2], RT_NULL, 0);
        rt_kprintf("flash ssi_probe cpu=%d (need bind; may SError)\n", cpu);
        return drv_flash_ssi_cpu_probe(cpu);
    }

#if defined(RT_USING_ZMODEM) || defined(RT_USING_RYM)
    if (strcmp(argv[1], "update") == 0 || strcmp(argv[1], "updatey") == 0)
    {
        uint32_t flash_addr;
        uint32_t range;
        int proto;

        if (argc != 4)
        {
            rt_kprintf("flash update[y] <flash_address> <flash_range>\n");
            return -RT_ERROR;
        }
        flash_addr = (uint32_t)strtoul(argv[2], RT_NULL, 16);
        range = (uint32_t)strtoul(argv[3], RT_NULL, 16);
#ifdef RT_USING_ZMODEM
        proto = (strcmp(argv[1], "updatey") == 0) ?
                FLASH_UPDATE_YMODEM : FLASH_UPDATE_ZMODEM;
#else
        proto = FLASH_UPDATE_YMODEM;
#endif
#ifndef RT_USING_RYM
        if (proto == FLASH_UPDATE_YMODEM)
        {
            rt_kprintf("flash updatey: Ymodem not enabled\n");
            return -RT_ERROR;
        }
#endif
        return flash_cmd_update(flash_addr, range, proto);
    }
#endif

    if (argc > 2)
        addr = (uint32_t)strtoul(argv[2], RT_NULL, 16);
    if (argc > 3)
        len = (int)strtoul(argv[3], RT_NULL, 0);

    if (len <= 0)
        return -RT_ERROR;

    if (strcmp(argv[1], "read") == 0)
    {
        static uint8_t buf[512] __attribute__((aligned(64)));
        int ret;

        if (len > (int)sizeof(buf))
            len = (int)sizeof(buf);
        ret = drv_flash_read(buf, len, addr);
        if (ret == 0)
            flash_dump_hex(addr, buf, len);
        return ret;
    }

    if (strcmp(argv[1], "erase") == 0)
    {
        if (flash_cmd_check_bootcode(addr, len, 1) != 0)
            return -RT_ERROR;
        return drv_flash_erase(addr, len);
    }

    if (strcmp(argv[1], "write") == 0)
    {
        static uint8_t pat[512] __attribute__((aligned(64)));
        int ret;

        if (flash_cmd_check_bootcode(addr, len, 1) != 0)
            return -RT_ERROR;
        if (len > (int)sizeof(pat))
            len = (int)sizeof(pat);
        for (int i = 0; i < len; i++)
            pat[i] = (uint8_t)(0xA0 + i);
        ret = drv_flash_write(pat, len, addr);
        if (ret == 0)
            rt_kprintf("flash write OK addr=0x%x len=%d\n", addr, len);
        return ret;
    }

    if (strcmp(argv[1], "test") == 0)
    {
        static uint8_t wr[256] __attribute__((aligned(64)));
        static uint8_t rd[256] __attribute__((aligned(64)));
        int ret;
        unsigned long cost;

        if (flash_cmd_check_bootcode(addr, len, 1) != 0)
            return -RT_ERROR;
        if (len > (int)sizeof(wr))
            len = (int)sizeof(wr);

        for (int i = 0; i < len; i++)
            wr[i] = (uint8_t)(0x5A + i);

        cost = rt_tick_get();
        ret = drv_flash_write(wr, len, addr);
        if (ret != 0)
        {
            rt_kprintf("flash test: write fail ret=%d\n", ret);
            return ret;
        }

        rt_thread_mdelay(50);
        memset(rd, 0, sizeof(rd));
        ret = drv_flash_read(rd, len, addr);
        cost = rt_tick_get() - cost;
        if (ret != 0)
        {
            rt_kprintf("flash test: readback fail ret=%d\n", ret);
            return ret;
        }

        for (int i = 0; i < len; i++)
        {
            if (rd[i] != wr[i])
            {
                rt_kprintf("flash test: verify fail @+%d wr=0x%02x rd=0x%02x\n",
                           i, wr[i], rd[i]);
                flash_dump_hex(addr, rd, len);
                return BIZ_ERR_FLASH_WRITE_CHECK;
            }
        }

        rt_kprintf("flash test OK addr=0x%x len=%d ticks=%lu\n", addr, len, cost);
        flash_dump_hex(addr, rd, len);
        return 0;
    }

    return -RT_ERROR;
}
MSH_CMD_EXPORT_ALIAS(cmd_flash, flash, flash read/write/erase/update/test);
#endif

static int cmd_log(int argc, char **argv)
{
    int lv;

    if (argc < 2)
    {
        rt_kprintf("log level=%u (%s)  [error=3 warn=4 info=6 debug=7]\n",
                   (unsigned)biz_log_get_level(),
                   biz_log_level_name(biz_log_get_level()));
        rt_kprintf("usage: log <error|warn|info|debug|N>\n");
        return 0;
    }

    lv = biz_log_parse_level(argv[1]);
    if (lv < 0)
    {
        rt_kprintf("log: bad level '%s' (use error|warn|info|debug|N)\n",
                   argv[1]);
        return -RT_ERROR;
    }
    biz_log_set_level((unsigned char)lv);
    rt_kprintf("log level=%u (%s)\n",
               (unsigned)biz_log_get_level(),
               biz_log_level_name(biz_log_get_level()));
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log, log, set or show log level (error/warn/info/debug/N));

#ifdef BSP_BIZ_PHASE_STATS
#include "biz_emmc.h"

static int cmd_phase(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "reset"))
    {
        biz_phase_reset();
        rt_kprintf("[phase] reset\n");
        return 0;
    }
    biz_phase_dump();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_phase, phase, phase [reset] — show/reset KA timing stats);
#endif

static int cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("hp232x drv BSP 1.0 (hp640 SPL port)\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ver, ver, show driver version);

#endif /* BIZ_MOD_FINSH */
