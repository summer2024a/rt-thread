/*
 * biz_i2c_proxy.c — KA200 side of MCU I2C mailbox (Mode B IP register proxy).
 *
 * MCU writes cmd frame to reg=0x00, KA200 accesses on-chip regs, MCU reads rsp.
 * Frame: [cmd_id][payload_len][crc8][rsvd][payload...]
 * CRC: same calcCRC() as HP2320 APP crc32.c (poly FACTOR=0x07).
 */
#include <rtthread.h>
#include <string.h>
#include "biz_i2c_proxy.h"
#include "biz_log.h"
#include "drv_pvt.h"

uint8_t biz_i2c_calc_crc(const uint8_t *pdat, uint32_t len)
{
    uint8_t crc = 0x00;
    uint8_t j;

#define FACTOR (0x107U & 0xFFU)

    while (len--)
    {
        crc ^= *pdat++;
        for (j = 8; j > 0; j--)
        {
            if (crc & 0x80U)
                crc = (uint8_t)((crc << 1) ^ FACTOR);
            else
                crc = (uint8_t)(crc << 1);
        }
    }

    return crc;
}

static void build_rsp(uint8_t *rsp, uint8_t cmd_id,
                      const uint8_t *payload, uint8_t plen)
{
    rsp[0] = cmd_id;
    rsp[1] = plen;
    rsp[2] = (plen > 0U) ? biz_i2c_calc_crc(payload, plen) : 0U;
    rsp[3] = 0U;
    if (plen > 0U && payload != RT_NULL)
        memcpy(rsp + BIZ_I2C_CMD_HEADER_LEN, payload, plen);
}

static int check_req(const uint8_t *req, uint16_t req_len)
{
    uint8_t plen;

    if (req == RT_NULL || req_len < BIZ_I2C_CMD_HEADER_LEN)
        return -1;

    plen = req[1];
    if (plen > BIZ_I2C_PAYLOAD_MAX)
        return -1;
    if ((uint16_t)(BIZ_I2C_CMD_HEADER_LEN + plen) > req_len)
        return -1;
    if (plen > 0U &&
        req[2] != biz_i2c_calc_crc(req + BIZ_I2C_CMD_HEADER_LEN, plen))
        return -1;

    return 0;
}

/*
 * Device MMIO often rejects byte/half accesses (CPR BOOT_SELECT etc.).
 * Prefer natural-width aligned ops; fall back to byte only for unaligned tails.
 */
static void mmio_read_bytes(uintptr_t addr, uint8_t *out, uint8_t len)
{
    while (len > 0U)
    {
        if (((addr & 3U) == 0U) && (len >= 4U))
        {
            uint32_t v = *(volatile uint32_t *)addr;
            memcpy(out, &v, 4);
            addr += 4U;
            out += 4;
            len = (uint8_t)(len - 4U);
        }
        else if (((addr & 1U) == 0U) && (len >= 2U))
        {
            uint16_t v = *(volatile uint16_t *)addr;
            memcpy(out, &v, 2);
            addr += 2U;
            out += 2;
            len = (uint8_t)(len - 2U);
        }
        else
        {
            *out++ = *(volatile uint8_t *)addr;
            addr++;
            len--;
        }
    }
}

static void mmio_write_bytes(uintptr_t addr, const uint8_t *in, uint8_t len)
{
    while (len > 0U)
    {
        if (((addr & 3U) == 0U) && (len >= 4U))
        {
            uint32_t v;
            memcpy(&v, in, 4);
            *(volatile uint32_t *)addr = v;
            addr += 4U;
            in += 4;
            len = (uint8_t)(len - 4U);
        }
        else if (((addr & 1U) == 0U) && (len >= 2U))
        {
            uint16_t v;
            memcpy(&v, in, 2);
            *(volatile uint16_t *)addr = v;
            addr += 2U;
            in += 2;
            len = (uint8_t)(len - 2U);
        }
        else
        {
            *(volatile uint8_t *)addr = *in++;
            addr++;
            len--;
        }
    }
}

static int handle_read_ip_reg(const uint8_t *payload, uint8_t plen,
                              uint8_t *rsp_out, uint16_t rsp_max)
{
    biz_i2c_ip_reg_desc_t desc;
    uint8_t data[BIZ_I2C_IP_ACCESS_MAX];
    uintptr_t addr;
    uint16_t need;

    if (plen < sizeof(desc))
        return -1;

    memcpy(&desc, payload, sizeof(desc));
    if (desc.access_len == 0U || desc.access_len > BIZ_I2C_IP_ACCESS_MAX)
        return -1;

    need = (uint16_t)(BIZ_I2C_CMD_HEADER_LEN + desc.access_len);
    if (rsp_out == RT_NULL || rsp_max < need)
        return -1;

    addr = (uintptr_t)desc.reg_addr;
    mmio_read_bytes(addr, data, desc.access_len);
    build_rsp(rsp_out, BIZ_I2C_CMD_READ_IP_REG, data, desc.access_len);
    return (int)need;
}

static int handle_write_ip_reg(const uint8_t *payload, uint8_t plen)
{
    biz_i2c_ip_reg_desc_t desc;
    uintptr_t addr;

    if (plen < sizeof(desc))
        return -1;

    memcpy(&desc, payload, sizeof(desc));
    if (desc.access_len == 0U || desc.access_len > BIZ_I2C_IP_ACCESS_MAX)
        return -1;
    if (plen < (uint8_t)(sizeof(desc) + desc.access_len))
        return -1;

    addr = (uintptr_t)desc.reg_addr;
    mmio_write_bytes(addr, payload + sizeof(desc), desc.access_len);
    return 0;
}

static int handle_get_soc_temp(uint8_t *rsp_out, uint16_t rsp_max)
{
    float temps[DRV_PVT_TS_COUNT];
    char text[16];
    int n;
    uint8_t max_t = 0;
    int i;

    if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + 12U))
        return -1;

    if (drv_pvt_read_temperature(temps, 0) == 0)
    {
        for (i = 0; i < DRV_PVT_TS_COUNT; i++)
        {
            if (temps[i] < 0.0f)
                continue;
            if (temps[i] > (float)max_t)
                max_t = (uint8_t)temps[i];
        }
    }

    n = rt_snprintf(text, sizeof(text), "%u", (unsigned)max_t);
    if (n < 0)
        n = 0;
    if (n > 12)
        n = 12;

    build_rsp(rsp_out, BIZ_I2C_CMD_GET_SOC_TEMP, (const uint8_t *)text, (uint8_t)n);
    return (int)(BIZ_I2C_CMD_HEADER_LEN + (uint8_t)n);
}

static int handle_get_soc_status(uint8_t *rsp_out, uint16_t rsp_max)
{
    static const char ok[] = "0";

    if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + 1U))
        return -1;

    build_rsp(rsp_out, BIZ_I2C_CMD_GET_SOC_STATUS, (const uint8_t *)ok, 1);
    return (int)(BIZ_I2C_CMD_HEADER_LEN + 1U);
}

int biz_i2c_proxy_handle(const uint8_t *req, uint16_t req_len,
                         uint8_t *rsp_out, uint16_t rsp_max)
{
    uint8_t cmd_id;
    uint8_t plen;
    const uint8_t *payload;
    int ret;

    if (check_req(req, req_len) != 0)
        return -1;

    cmd_id = req[0];
    plen = req[1];
    payload = req + BIZ_I2C_CMD_HEADER_LEN;

    switch (cmd_id)
    {
    case BIZ_I2C_CMD_READ_IP_REG:
        ret = handle_read_ip_reg(payload, plen, rsp_out, rsp_max);
        break;

    case BIZ_I2C_CMD_WRITE_IP_REG:
        ret = handle_write_ip_reg(payload, plen);
        break;

    case BIZ_I2C_CMD_GET_SOC_TEMP:
        ret = handle_get_soc_temp(rsp_out, rsp_max);
        break;

    case BIZ_I2C_CMD_GET_SOC_STATUS:
        ret = handle_get_soc_status(rsp_out, rsp_max);
        break;

    case BIZ_I2C_CMD_GET_SOC_TIME:
    case BIZ_I2C_CMD_GET_SOC_VOL:
        /* Placeholder ASCII until dedicated sensors wired */
        if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + 1U))
            return -1;
        build_rsp(rsp_out, cmd_id, (const uint8_t *)"0", 1);
        ret = (int)(BIZ_I2C_CMD_HEADER_LEN + 1U);
        break;

    default:
        /* Push board-info cmds (0x80–0x8F): accept & ignore for now */
        if (cmd_id >= 0x80U && cmd_id <= 0x8FU)
            return 0;
        BIZ_WARN("I2C proxy unknown cmd 0x%02x\n", cmd_id);
        ret = -1;
        break;
    }

    return ret;
}
