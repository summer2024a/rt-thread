/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-XX     Your Name    md/mw memory commands (U-Boot style)
 */

#include <rtthread.h>
#include <finsh.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <board.h>

/*
 * Valid MMU-mapped address ranges (hp232x_mmu.c):
 *   IRAM0/IRAM1, low peripherals, APU NORM/corecfg/NN FIFO.
 */

struct addr_range {
    rt_uint64_t start;
    rt_uint64_t end;
    const char *name;
    rt_bool_t writable;
};

static const struct addr_range valid_ranges[] = {
    { 0x04000000UL, 0x0407FFFFUL, "IRAM0", RT_TRUE },
    { 0x100000000ULL, 0x10007FFFFULL, "IRAM1", RT_TRUE },
    { 0x00000000UL, 0x001FFFFFUL, "PERIPH_BOOT_ROM", RT_FALSE },
    { 0x02000000UL, 0x021FFFFFUL, "PERIPH_REGION_1", RT_TRUE },
    { 0x06000000UL, 0x061FFFFFUL, "PERIPH_REGION_2", RT_TRUE },
    { 0x08000000UL, 0x0FFFFFFFUL, "PERIPH_REGION_3", RT_TRUE },
    { 0x10000000UL, 0x1FFFFFFFUL, "PERIPH_REGION_4", RT_TRUE },
    { 0x1400000000ULL, 0x143FFFFFFFULL, "APU_NORM", RT_TRUE },
    { 0x1500000000ULL, 0x157FFFFFFFULL, "APU_CORECFG", RT_TRUE },
    { 0x1600000000ULL, 0x167FFFFFFFULL, "APU_NN_FIFO", RT_TRUE },
};

#define NUM_VALID_RANGES (sizeof(valid_ranges) / sizeof(valid_ranges[0]))
#define MD_LINE_LEN      16

static rt_bool_t is_valid_address(rt_uint64_t addr, const struct addr_range **range_out)
{
    rt_size_t i;

    for (i = 0; i < NUM_VALID_RANGES; i++)
    {
        if (addr >= valid_ranges[i].start && addr <= valid_ranges[i].end)
        {
            if (range_out)
            {
                *range_out = &valid_ranges[i];
            }
            return RT_TRUE;
        }
    }

    return RT_FALSE;
}

static rt_bool_t is_valid_address_range(rt_uint64_t start_addr, rt_size_t length,
                                        rt_bool_t need_write, const struct addr_range **range_out)
{
    rt_uint64_t end_addr = start_addr + length - 1;
    const struct addr_range *range;

    if (length == 0)
    {
        return RT_TRUE;
    }

    if (end_addr < start_addr)
    {
        rt_kprintf("Error: address range overflow (0x%llx + 0x%x)\n", start_addr, length);
        return RT_FALSE;
    }

    if (!is_valid_address(start_addr, &range))
    {
        rt_kprintf("Error: 0x%llx is not in any MMU-mapped region\n", start_addr);
        return RT_FALSE;
    }

    if (end_addr > range->end)
    {
        rt_kprintf("Error: range crosses %s boundary (end 0x%llx)\n", range->name, range->end);
        return RT_FALSE;
    }

    if (need_write && !range->writable)
    {
        rt_kprintf("Error: region %s is read-only\n", range->name);
        return RT_FALSE;
    }

    if (range_out)
    {
        *range_out = range;
    }

    return RT_TRUE;
}

static rt_uint64_t cmd_parse_addr(const char *str)
{
    return (rt_uint64_t)strtoull(str, RT_NULL, 16);
}

static rt_uint64_t cmd_parse_count(const char *str)
{
    return (rt_uint64_t)strtoull(str, RT_NULL, 0);
}

static void cmd_print_buffer(rt_uint64_t addr, int width, rt_uint64_t count)
{
    rt_uint64_t linelen = MD_LINE_LEN / width;
    rt_uint8_t linebuf[MD_LINE_LEN];
    rt_uint64_t n;

    if (linelen == 0)
    {
        linelen = 1;
    }

    while (count > 0)
    {
        rt_uint64_t thisline = (count < linelen) ? count : linelen;
        rt_uint64_t i;

        rt_kprintf("%012llx:", addr);

        for (i = 0; i < thisline; i++)
        {
            rt_uint64_t cur = addr + i * width;

            switch (width)
            {
            case 1:
                linebuf[i] = *(volatile rt_uint8_t *)cur;
                rt_kprintf(" %02x", linebuf[i]);
                break;
            case 2:
            {
                rt_uint16_t val = *(volatile rt_uint16_t *)cur;
                linebuf[i * 2] = (rt_uint8_t)(val & 0xff);
                linebuf[i * 2 + 1] = (rt_uint8_t)(val >> 8);
                rt_kprintf(" %04x", val);
                break;
            }
            case 4:
            default:
            {
                rt_uint32_t val = *(volatile rt_uint32_t *)cur;
                linebuf[i * 4] = (rt_uint8_t)(val & 0xff);
                linebuf[i * 4 + 1] = (rt_uint8_t)((val >> 8) & 0xff);
                linebuf[i * 4 + 2] = (rt_uint8_t)((val >> 16) & 0xff);
                linebuf[i * 4 + 3] = (rt_uint8_t)(val >> 24);
                rt_kprintf(" %08x", val);
                break;
            }
            }
        }

        for (i = thisline; i < linelen; i++)
        {
            rt_kprintf((width == 4) ? "         " : (width == 2) ? "     " : "   ");
        }

        rt_kprintf("    ");
        n = thisline * width;
        for (i = 0; i < n; i++)
        {
            rt_kprintf("%c", isprint(linebuf[i]) ? linebuf[i] : '.');
        }
        rt_kprintf("\n");

        addr += thisline * width;
        count -= thisline;
    }
}

static void cmd_md_usage(void)
{
    rt_kprintf("Usage: md addr [count]          (32-bit, count default 0x40)\n");
    rt_kprintf("       mdb/mdw/mdl addr [count] (8/16/32-bit)\n");
}

static int cmd_md_width(int argc, char **argv, int width)
{
    rt_uint64_t addr;
    rt_uint64_t count = 0x40;
    rt_size_t bytes;

    if (argc < 2)
    {
        cmd_md_usage();
        return -1;
    }

    addr = cmd_parse_addr(argv[1]);
    if (argc >= 3)
    {
        count = cmd_parse_count(argv[2]);
    }

    bytes = (rt_size_t)(count * width);
    if (!is_valid_address_range(addr, bytes, RT_FALSE, RT_NULL))
    {
        return -1;
    }

    cmd_print_buffer(addr, width, count);
    return 0;
}

static int cmd_md(int argc, char **argv)
{
    return cmd_md_width(argc, argv, 4);
}

static int cmd_mdb(int argc, char **argv)
{
    return cmd_md_width(argc, argv, 1);
}

static int cmd_mdw(int argc, char **argv)
{
    return cmd_md_width(argc, argv, 2);
}

static int cmd_mdl(int argc, char **argv)
{
    return cmd_md_width(argc, argv, 4);
}

static void cmd_mw_usage(void)
{
    rt_kprintf("Usage: mw addr value [count]          (32-bit, count default 1)\n");
    rt_kprintf("       mwb/mww/mwl addr value [count] (8/16/32-bit)\n");
}

static int cmd_mw_width(int argc, char **argv, int width)
{
    rt_uint64_t addr;
    rt_uint64_t value;
    rt_uint64_t count = 1;
    rt_size_t bytes;
    rt_uint64_t i;

    if (argc < 3)
    {
        cmd_mw_usage();
        return -1;
    }

    addr = cmd_parse_addr(argv[1]);
    value = cmd_parse_count(argv[2]);
    if (argc >= 4)
    {
        count = cmd_parse_count(argv[3]);
    }

    bytes = (rt_size_t)(count * width);
    if (!is_valid_address_range(addr, bytes, RT_TRUE, RT_NULL))
    {
        return -1;
    }

    for (i = 0; i < count; i++)
    {
        rt_uint64_t cur = addr + i * width;

        switch (width)
        {
        case 1:
            *(volatile rt_uint8_t *)cur = (rt_uint8_t)(value & 0xff);
            break;
        case 2:
            *(volatile rt_uint16_t *)cur = (rt_uint16_t)(value & 0xffff);
            break;
        case 4:
        default:
            *(volatile rt_uint32_t *)cur = (rt_uint32_t)value;
            break;
        }
    }

    return 0;
}

static int cmd_mw(int argc, char **argv)
{
    return cmd_mw_width(argc, argv, 4);
}

static int cmd_mwb(int argc, char **argv)
{
    return cmd_mw_width(argc, argv, 1);
}

static int cmd_mww(int argc, char **argv)
{
    return cmd_mw_width(argc, argv, 2);
}

static int cmd_mwl(int argc, char **argv)
{
    return cmd_mw_width(argc, argv, 4);
}

MSH_CMD_EXPORT_ALIAS(cmd_md, md, memory display);
MSH_CMD_EXPORT_ALIAS(cmd_mdb, mdb, memory display bytes);
MSH_CMD_EXPORT_ALIAS(cmd_mdw, mdw, memory display words);
MSH_CMD_EXPORT_ALIAS(cmd_mdl, mdl, memory display longs);

MSH_CMD_EXPORT_ALIAS(cmd_mw, mw, memory write);
MSH_CMD_EXPORT_ALIAS(cmd_mwb, mwb, memory write bytes);
MSH_CMD_EXPORT_ALIAS(cmd_mww, mww, memory write words);
MSH_CMD_EXPORT_ALIAS(cmd_mwl, mwl, memory write longs);
