/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-XX     Your Name    First version of register/memory commands
 */

#define DBG_TAG "cmd"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>
#include <rtthread.h>
#include <finsh.h>
#include <stdlib.h>
#include <string.h>

#include <board.h>

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>
#endif

/**
 * @brief reg_read - 读取物理地址的内存值
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 物理地址 (16 进制，如 0x08000000)
 *        argv[2]: 可选，数据宽度 (8/16/32), 默认 32
 *
 * @return 0: 成功 -1: 失败
 */
static int cmd_reg_read(int argc, char **argv)
{
    rt_uintptr_t addr;
    rt_uint32_t width = 32;
    rt_uint32_t value = 0;

    if (argc < 2)
    {
        rt_kprintf("Usage: %s read <address> [width]\n", argv[0]);
        rt_kprintf("  address: physical address in hex (e.g., 0x08000000)\n");
        rt_kprintf("  width: data width in bits (8/16/32), default 32\n");
        return -1;
    }

    /* 解析地址 */
    addr = (rt_uintptr_t)strtoul(argv[1], RT_NULL, 16);

    /* 解析宽度 */
    if (argc >= 3)
    {
        width = strtoul(argv[2], RT_NULL, 10);
        if (width != 8 && width != 16 && width != 32)
        {
            rt_kprintf("Error: Invalid width %d. Must be 8, 16 or 32.\n", width);
            return -1;
        }
    }

    /* 验证地址是否在有效 DDR 范围内 */
    if (addr >= MEM_PADDR_START && addr <= (MEM_PADDR_START + MEM_CACHE_SZ - 1))
    {
        /* DDR 区域 */
    }
    else if (addr >= 0x04000000UL && addr <= 0x040FFFFFUL)
    {
        /* IRAM 区域 */
    }
    else if (addr >= 0x08000000UL && addr <= 0x1BFFFFFFUL)
    {
        /* 外设区域 */
    }
    else
    {
        rt_kprintf("Warning: Address 0x%lx may not be mapped.\n", addr);
    }

    /* 根据宽度读取数据 */
    switch (width)
    {
        case 8:
            value = *(volatile rt_uint8_t *)addr;
            rt_kprintf("0x%08lx (8-bit): 0x%02x\n", addr, (rt_uint8_t)value);
            break;

        case 16:
            value = *(volatile rt_uint16_t *)addr;
            rt_kprintf("0x%08lx (16-bit): 0x%04x\n", addr, (rt_uint16_t)value);
            break;

        case 32:
        default:
            value = *(volatile rt_uint32_t *)addr;
            rt_kprintf("0x%08lx (32-bit): 0x%08x\n", addr, value);
            break;
    }

    return 0;
}

/**
 * @brief reg_write - 写入物理地址的内存值
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 物理地址 (16 进制，如 0x08000000)
 *        argv[2]: 要写入的值 (16 进制)
 *        argv[3]: 可选，数据宽度 (8/16/32), 默认 32
 *
 * @return 0: 成功 -1: 失败
 */
static int cmd_reg_write(int argc, char **argv)
{
    rt_uintptr_t addr;
    rt_uint32_t width = 32;
    rt_uint32_t value = 0;

    if (argc < 3)
    {
        rt_kprintf("Usage: %s write <address> <value> [width]\n", argv[0]);
        rt_kprintf("  address: physical address in hex (e.g., 0x08000000)\n");
        rt_kprintf("  value: value to write in hex\n");
        rt_kprintf("  width: data width in bits (8/16/32), default 32\n");
        return -1;
    }

    /* 解析地址 */
    addr = (rt_uintptr_t)strtoul(argv[1], RT_NULL, 16);

    /* 解析值 */
    value = (rt_uint32_t)strtoul(argv[2], RT_NULL, 16);

    /* 解析宽度 */
    if (argc >= 4)
    {
        width = strtoul(argv[3], RT_NULL, 10);
        if (width != 8 && width != 16 && width != 32)
        {
            rt_kprintf("Error: Invalid width %d. Must be 8, 16 or 32.\n", width);
            return -1;
        }
    }

    /* 验证地址是否在有效范围内 */
    if (addr >= MEM_PADDR_START && addr <= (MEM_PADDR_START + MEM_CACHE_SZ - 1))
    {
        /* DDR 区域 - 允许写入 */
    }
    else if (addr >= 0x04000000UL && addr <= 0x040FFFFFUL)
    {
        /* IRAM 区域 - 允许写入 */
    }
    else if (addr >= 0x08000000UL && addr <= 0x1BFFFFFFUL)
    {
        /* 外设区域 - 允许写入 */
        rt_kprintf("Warning: Writing to peripheral region at 0x%lx\n", addr);
    }
    else
    {
        rt_kprintf("Error: Address 0x%lx is not in a valid writable region.\n", addr);
        return -1;
    }

    /* 根据宽度写入数据 */
    switch (width)
    {
        case 8:
            *(volatile rt_uint8_t *)addr = (rt_uint8_t)(value & 0xFF);
            rt_kprintf("Write 0x%02x to 0x%08lx (8-bit)\n", (rt_uint8_t)(value & 0xFF), addr);
            break;

        case 16:
            *(volatile rt_uint16_t *)addr = (rt_uint16_t)(value & 0xFFFF);
            rt_kprintf("Write 0x%04x to 0x%08lx (16-bit)\n", (rt_uint16_t)(value & 0xFFFF), addr);
            break;

        case 32:
        default:
            *(volatile rt_uint32_t *)addr = value;
            rt_kprintf("Write 0x%08x to 0x%08lx (32-bit)\n", value, addr);
            break;
    }

    return 0;
}

/**
 * @brief mem_dump - 显示一片内存区域的值
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 起始地址 (16 进制)
 *        argv[2]: 长度 (字节数，16 进制或十进制)
 *        argv[3]: 可选，数据宽度 (8/16/32), 默认 32
 *
 * @return 0: 成功 -1: 失败
 */
static int cmd_mem_dump(int argc, char **argv)
{
    rt_uintptr_t start_addr;
    rt_size_t length;
    rt_uint32_t width = 32;
    rt_uintptr_t i;
    rt_uint32_t count;

    if (argc < 3)
    {
        rt_kprintf("Usage: %s dump <start_addr> <length> [width]\n", argv[0]);
        rt_kprintf("  start_addr: starting physical address in hex\n");
        rt_kprintf("  length: number of bytes to dump (hex or decimal)\n");
        rt_kprintf("  width: data width in bits (8/16/32), default 32\n");
        return -1;
    }

    /* 解析起始地址 */
    start_addr = (rt_uintptr_t)strtoul(argv[1], RT_NULL, 16);

    /* 解析长度 */
    length = (rt_size_t)strtoul(argv[2], RT_NULL, 0);

    /* 解析宽度 */
    if (argc >= 4)
    {
        width = strtoul(argv[3], RT_NULL, 10);
        if (width != 8 && width != 16 && width != 32)
        {
            rt_kprintf("Error: Invalid width %d. Must be 8, 16 or 32.\n", width);
            return -1;
        }
    }

    /* 计算显示行数 */
    switch (width)
    {
        case 8:
            count = length;
            break;
        case 16:
            count = (length + 1) / 2;
            start_addr &= ~1; /* 2 字节对齐 */
            break;
        case 32:
        default:
            count = (length + 3) / 4;
            start_addr &= ~3; /* 4 字节对齐 */
            break;
    }

    rt_kprintf("Memory dump from 0x%lx, length %d bytes, width %d-bit:\n", 
               start_addr, length, width);

    /* 按行显示 */
    for (i = 0; i < count; i++)
    {
        if (i % 4 == 0)
        {
            rt_kprintf("\n0x%08lx:", start_addr + i * (width / 8));
        }

        switch (width)
        {
            case 8:
                rt_kprintf(" %02x", *(volatile rt_uint8_t *)(start_addr + i));
                break;

            case 16:
                rt_kprintf(" %04x", *(volatile rt_uint16_t *)(start_addr + i * 2));
                break;

            case 32:
            default:
                rt_kprintf(" %08x", *(volatile rt_uint32_t *)(start_addr + i * 4));
                break;
        }
    }
    rt_kprintf("\n");

    return 0;
}

/**
 * @brief mem_cmp - 内存区域比较
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 第一个地址
 *        argv[2]: 第二个地址
 *        argv[3]: 长度 (字节数)
 *
 * @return 0: 相同 -1: 不同
 */
static int cmd_mem_cmp(int argc, char **argv)
{
    rt_uintptr_t addr1, addr2;
    rt_size_t length;
    rt_uintptr_t i;
    rt_uint8_t val1, val2;

    if (argc < 4)
    {
        rt_kprintf("Usage: %s cmp <addr1> <addr2> <length>\n", argv[0]);
        rt_kprintf("  addr1: first physical address in hex\n");
        rt_kprintf("  addr2: second physical address in hex\n");
        rt_kprintf("  length: number of bytes to compare\n");
        return -1;
    }

    /* 解析地址和长度 */
    addr1 = (rt_uintptr_t)strtoul(argv[1], RT_NULL, 16);
    addr2 = (rt_uintptr_t)strtoul(argv[2], RT_NULL, 16);
    length = (rt_size_t)strtoul(argv[3], RT_NULL, 0);

    rt_kprintf("Comparing 0x%lx with 0x%lx, length %d bytes...\n", 
               addr1, addr2, length);

    /* 逐字节比较 */
    for (i = 0; i < length; i++)
    {
        val1 = *(volatile rt_uint8_t *)(addr1 + i);
        val2 = *(volatile rt_uint8_t *)(addr2 + i);

        if (val1 != val2)
        {
            rt_kprintf("Difference at offset 0x%lx:\n", i);
            rt_kprintf("  0x%08lx: 0x%02x\n", addr1 + i, val1);
            rt_kprintf("  0x%08lx: 0x%02x\n", addr2 + i, val2);
            return -1;
        }
    }

    rt_kprintf("Memory regions are identical.\n");
    return 0;
}

/**
 * @brief mem_fill - 填充内存区域
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 起始地址
 *        argv[2]: 长度
 *        argv[3]: 填充值
 *
 * @return 0: 成功 -1: 失败
 */
static int cmd_mem_fill(int argc, char **argv)
{
    rt_uintptr_t addr;
    rt_size_t length;
    rt_uint32_t value;
    rt_uintptr_t i;

    if (argc < 4)
    {
        rt_kprintf("Usage: %s fill <address> <length> <value>\n", argv[0]);
        rt_kprintf("  address: starting physical address in hex\n");
        rt_kprintf("  length: number of bytes to fill\n");
        rt_kprintf("  value: value to fill (in hex)\n");
        return -1;
    }

    /* 解析参数 */
    addr = (rt_uintptr_t)strtoul(argv[1], RT_NULL, 16);
    length = (rt_size_t)strtoul(argv[2], RT_NULL, 0);
    value = (rt_uint32_t)strtoul(argv[3], RT_NULL, 16);

    rt_kprintf("Filling 0x%lx bytes with 0x%08x starting at 0x%lx...\n", 
               length, value, addr);

    /* 填充内存 */
    for (i = 0; i < length; i++)
    {
        *(volatile rt_uint8_t *)(addr + i) = (rt_uint8_t)(value & 0xFF);
    }

    rt_kprintf("Fill completed.\n");
    return 0;
}

/**
 * @brief 显示帮助信息
 */
static void cmd_show_help(const char *cmd_name)
{
    rt_kprintf("\n");
    rt_kprintf("%s - Register and Memory Operations Tool\n", cmd_name);
    rt_kprintf("\n");
    rt_kprintf("Usage:\n");
    rt_kprintf("  %s read <address> [width]          - Read register value\n", cmd_name);
    rt_kprintf("  %s write <address> <value> [width] - Write register value\n", cmd_name);
    rt_kprintf("  %s dump <start_addr> <length> [width] - Dump memory region\n", cmd_name);
    rt_kprintf("  %s cmp <addr1> <addr2> <length>    - Compare memory regions\n", cmd_name);
    rt_kprintf("  %s fill <address> <length> <value> - Fill memory region\n", cmd_name);
    rt_kprintf("\n");
    rt_kprintf("Parameters:\n");
    rt_kprintf("  address: Physical address in hexadecimal (e.g., 0x08000000)\n");
    rt_kprintf("  value:   Value in hexadecimal\n");
    rt_kprintf("  width:   Data width in bits: 8, 16, or 32 (default: 32)\n");
    rt_kprintf("  length:  Number of bytes in hexadecimal or decimal\n");
    rt_kprintf("\n");
    rt_kprintf("Examples:\n");
    rt_kprintf("  %s read 0x08000000         - Read 32-bit from address 0x08000000\n", cmd_name);
    rt_kprintf("  %s write 0x08000000 0x12345678 - Write 32-bit value\n", cmd_name);
    rt_kprintf("  %s dump 0x08000000 256     - Dump 256 bytes starting at 0x08000000\n", cmd_name);
    rt_kprintf("  %s read 0x08000000 16      - Read 16-bit from address\n", cmd_name);
    rt_kprintf("\n");
}

/**
 * @brief 主命令入口函数
 *
 * @param argc 参数个数
 * @param argv 参数数组
 *        argv[1]: 子命令 (read/write/dump/cmp/fill/help)
 *
 * @return 0: 成功 -1: 失败
 */
static int cmd_memtool(int argc, char **argv)
{
    if (argc < 2)
    {
        cmd_show_help(argv[0]);
        return -1;
    }

    /* 获取子命令 */
    const char *subcmd = argv[1];

    /* 移除 argv[1]，将后续参数前移 */
    argc--;
    argv++;

    if (rt_strcmp(subcmd, "read") == 0)
    {
        return cmd_reg_read(argc, argv);
    }
    else if (rt_strcmp(subcmd, "write") == 0)
    {
        return cmd_reg_write(argc, argv);
    }
    else if (rt_strcmp(subcmd, "dump") == 0)
    {
        return cmd_mem_dump(argc, argv);
    }
    else if (rt_strcmp(subcmd, "cmp") == 0)
    {
        return cmd_mem_cmp(argc, argv);
    }
    else if (rt_strcmp(subcmd, "fill") == 0)
    {
        return cmd_mem_fill(argc, argv);
    }
    else if (rt_strcmp(subcmd, "help") == 0 || rt_strcmp(subcmd, "-h") == 0 || rt_strcmp(subcmd, "--help") == 0)
    {
        cmd_show_help(argv[0]);
        return 0;
    }
    else
    {
        rt_kprintf("Error: Unknown subcommand '%s'\n", subcmd);
        rt_kprintf("Run '%s help' for usage information.\n", argv[0]);
        return -1;
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_memtool, memtool, Register and Memory Operations Tool (memtool));
