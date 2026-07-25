/*
 * biz_i2c_proxy.c — KA200 side of MCU I2C mailbox (Mode B IP register proxy).
 *
 * MCU writes cmd frame to reg=0x00, KA200 accesses on-chip regs, MCU reads rsp.
 * Frame: [cmd_id][payload_len][crc8][rsvd][payload...]
 * CRC: same calcCRC() as HP2320 APP crc32.c (poly FACTOR=0x07).
 *
 * Phase B OTA (0xE8..0xEB): stream FW into IRAM1 host scratch, then async
 * drv_flash_write @ flash_addr (default 0xA6000).
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "biz_i2c_proxy.h"
#include "biz_log.h"
#include "biz_exec_handlers.h"
#include "biz_subsys.h"
#include "board.h"
#include "drv_flash.h"
#include "drv_pvt.h"

#define BIZ_I2C_OTA_DETAIL_OK            0U
#define BIZ_I2C_OTA_DETAIL_BAD_PARAM     1U
#define BIZ_I2C_OTA_DETAIL_OVERFLOW      2U
#define BIZ_I2C_OTA_DETAIL_CRC           3U
#define BIZ_I2C_OTA_DETAIL_FLASH         4U
#define BIZ_I2C_OTA_DETAIL_BUSY          5U
#define BIZ_I2C_OTA_DETAIL_INCOMPLETE    6U
#define BIZ_I2C_OTA_DETAIL_THREAD        7U

typedef struct {
    uint8_t state;
    uint8_t detail;
    uint32_t total_size;
    uint32_t img_crc32;
    uint32_t flash_addr;
    uint32_t recv_bytes;
    uint8_t *buf;
} biz_i2c_ota_ctx_t;

static biz_i2c_ota_ctx_t s_ota;
static struct rt_semaphore s_ota_commit_sem;
static rt_thread_t s_ota_worker;
static int s_ota_sem_inited;

static void ota_flash_thread_entry(void *param);

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

static void ota_reset_session(void)
{
    s_ota.state = BIZ_I2C_OTA_STATE_IDLE;
    s_ota.detail = BIZ_I2C_OTA_DETAIL_OK;
    s_ota.total_size = 0;
    s_ota.img_crc32 = 0;
    s_ota.flash_addr = BIZ_I2C_OTA_FLASH_ADDR_DEFAULT;
    s_ota.recv_bytes = 0;
    s_ota.buf = (uint8_t *)(uintptr_t)IRAM1_HOST_SCRATCH_START;
}

static void ota_scratch_dcache_clean(void)
{
    uintptr_t line = 64;
    uintptr_t s;
    uintptr_t e;
    int len;

    if (s_ota.buf == RT_NULL || s_ota.total_size == 0U)
        return;
    /* NC scratch: CPU stores already coherent with DRAM. */
    if (hp232x_addr_is_normal_nc(s_ota.buf, s_ota.total_size))
        return;

    /*
     * I2C OTA fills IRAM1 via CPU memcpy (WB). drv_flash_write then does
     * invalidate_dcache_all — without a prior clean, dirty lines are dropped
     * and page PP programs stale DRAM (Host eMMC Load is DMA→DRAM + inv, so
     * it does not hit this). Flush scratch to DRAM before FlashWrite.
     */
    s = (uintptr_t)s_ota.buf & ~(line - 1U);
    e = ((uintptr_t)s_ota.buf + s_ota.total_size + line - 1U) & ~(line - 1U);
    len = (int)(e - s);
    if (len > 0)
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)s, len);
}

static void ota_flash_thread_entry(void *param)
{
    (void)param;

    while (1)
    {
        int ret;
        uint32_t crc;
        int retry;

        rt_sem_take(&s_ota_commit_sem, RT_WAITING_FOREVER);

        BIZ_INFO("I2C OTA COMMIT size=%u crc=0x%x flash=0x%x\n",
                 s_ota.total_size, s_ota.img_crc32, s_ota.flash_addr);

        if (s_ota.img_crc32 != 0U)
        {
            crc = biz_crc32_calc(0, s_ota.buf, s_ota.total_size);
            if (crc != s_ota.img_crc32)
            {
                BIZ_ERROR("I2C OTA CRC mismatch calc=0x%x expect=0x%x\n",
                          crc, s_ota.img_crc32);
                s_ota.detail = BIZ_I2C_OTA_DETAIL_CRC;
                s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
                continue;
            }
        }

        ota_scratch_dcache_clean();
        (void)biz_flash_worker_ensure();

        ret = -1;
        for (retry = 0; retry < 3; retry++)
        {
            if (retry > 0)
            {
                BIZ_WARN("I2C OTA FlashWrite retry=%d\n", retry);
                ota_scratch_dcache_clean();
            }
            ret = drv_flash_write(s_ota.buf, (int)s_ota.total_size, s_ota.flash_addr);
            if (ret == 0)
                break;
        }
        if (ret != 0)
        {
            BIZ_ERROR("I2C OTA FlashWrite fail flash=0x%x ret=%d\n",
                      s_ota.flash_addr, ret);
            s_ota.detail = BIZ_I2C_OTA_DETAIL_FLASH;
            s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
            continue;
        }

        BIZ_INFO("I2C OTA OK FlashWrite flash=0x%x size=%u\n",
                 s_ota.flash_addr, s_ota.total_size);
        s_ota.detail = BIZ_I2C_OTA_DETAIL_OK;
        s_ota.state = BIZ_I2C_OTA_STATE_OK;
    }
}

static int ota_worker_ensure(void)
{
    if (s_ota_worker != RT_NULL)
        return 0;
    if (!s_ota_sem_inited)
    {
        if (rt_sem_init(&s_ota_commit_sem, "i2cota", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
            return -1;
        s_ota_sem_inited = 1;
    }
    s_ota_worker = rt_thread_create("i2cota", ota_flash_thread_entry, RT_NULL,
                                    3072, 12, 20);
    if (s_ota_worker == RT_NULL)
        return -1;
    rt_thread_startup(s_ota_worker);
    return 0;
}

static int ota_status_rsp(uint8_t *rsp_out, uint16_t rsp_max)
{
    biz_i2c_ota_status_t st;

    if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + sizeof(st)))
        return -1;

    st.state = s_ota.state;
    st.detail = s_ota.detail;
    st.recv_bytes = s_ota.recv_bytes;
    build_rsp(rsp_out, BIZ_I2C_CMD_OTA_STATUS, (const uint8_t *)&st, (uint8_t)sizeof(st));
    return (int)(BIZ_I2C_CMD_HEADER_LEN + sizeof(st));
}

static int handle_ota_open(const uint8_t *payload, uint8_t plen,
                           uint8_t *rsp_out, uint16_t rsp_max)
{
    biz_i2c_ota_open_t open;
    biz_i2c_ota_status_t st;

    if (s_ota.state == BIZ_I2C_OTA_STATE_WRITING)
        return -1;

    if (plen < sizeof(open))
        return -1;

    memcpy(&open, payload, sizeof(open));
    if (open.total_size == 0U ||
        open.total_size > (uint32_t)IRAM1_HOST_SCRATCH_SIZE)
        return -1;

    if (open.flash_addr == 0U)
        open.flash_addr = BIZ_I2C_OTA_FLASH_ADDR_DEFAULT;

    if (drv_flash_bootcode_overlap(open.flash_addr, open.total_size, 1))
        return -1;

    ota_reset_session();
    s_ota.total_size = open.total_size;
    s_ota.img_crc32 = open.img_crc32;
    s_ota.flash_addr = open.flash_addr;
    s_ota.state = BIZ_I2C_OTA_STATE_RECV;
    s_ota.detail = BIZ_I2C_OTA_DETAIL_OK;

    /* Pre-create worker at OPEN — COMMIT must not rt_thread_create in I2C BH. */
    if (ota_worker_ensure() != 0)
    {
        s_ota.detail = BIZ_I2C_OTA_DETAIL_THREAD;
        s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
        return ota_status_rsp(rsp_out, rsp_max);
    }

    /* Runs on i2c_mcu BH — avoid BIZ_INFO (384B stack frame); DEBUG only. */
    BIZ_DEBUG("I2C OTA OPEN size=%u crc=0x%x flash=0x%x\n",
              s_ota.total_size, s_ota.img_crc32, s_ota.flash_addr);

    if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + sizeof(st)))
        return 0;

    st.state = s_ota.state;
    st.detail = s_ota.detail;
    st.recv_bytes = s_ota.recv_bytes;
    build_rsp(rsp_out, BIZ_I2C_CMD_OTA_OPEN, (const uint8_t *)&st, (uint8_t)sizeof(st));
    return (int)(BIZ_I2C_CMD_HEADER_LEN + sizeof(st));
}

static int handle_ota_data(const uint8_t *payload, uint8_t plen)
{
    uint32_t offset;
    uint8_t chunk_len;

    if (s_ota.state != BIZ_I2C_OTA_STATE_RECV)
        return -1;
    if (plen < 4U)
        return -1;

    memcpy(&offset, payload, sizeof(offset));
    chunk_len = (uint8_t)(plen - 4U);
    if (chunk_len == 0U)
        return -1;
    if (offset > s_ota.total_size ||
        (offset + chunk_len) > s_ota.total_size)
    {
        s_ota.detail = BIZ_I2C_OTA_DETAIL_OVERFLOW;
        s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
        return -1;
    }

    memcpy(s_ota.buf + offset, payload + 4U, chunk_len);
    if ((offset + chunk_len) > s_ota.recv_bytes)
        s_ota.recv_bytes = offset + chunk_len;

    /* Progress every 32KB — DEBUG only (BH stack). */
    if ((s_ota.recv_bytes & 0x7FFFU) < chunk_len)
        BIZ_DEBUG("I2C OTA DATA recv=%u/%u\n", s_ota.recv_bytes, s_ota.total_size);

    return 0;
}

static int handle_ota_commit(uint8_t *rsp_out, uint16_t rsp_max)
{
    biz_i2c_ota_status_t st;

    if (s_ota.state == BIZ_I2C_OTA_STATE_WRITING)
    {
        s_ota.detail = BIZ_I2C_OTA_DETAIL_BUSY;
        return ota_status_rsp(rsp_out, rsp_max);
    }

    if (s_ota.state != BIZ_I2C_OTA_STATE_RECV ||
        s_ota.recv_bytes < s_ota.total_size)
    {
        BIZ_DEBUG("I2C OTA COMMIT incomplete recv=%u need=%u state=%u\n",
                  s_ota.recv_bytes, s_ota.total_size, s_ota.state);
        s_ota.detail = BIZ_I2C_OTA_DETAIL_INCOMPLETE;
        s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
        return ota_status_rsp(rsp_out, rsp_max);
    }

    if (ota_worker_ensure() != 0)
    {
        s_ota.detail = BIZ_I2C_OTA_DETAIL_THREAD;
        s_ota.state = BIZ_I2C_OTA_STATE_FAIL;
        return ota_status_rsp(rsp_out, rsp_max);
    }

    s_ota.state = BIZ_I2C_OTA_STATE_WRITING;
    s_ota.detail = BIZ_I2C_OTA_DETAIL_OK;
    BIZ_DEBUG("I2C OTA COMMIT start size=%u\n", s_ota.total_size);
    rt_sem_release(&s_ota_commit_sem);

    if (rsp_out == RT_NULL || rsp_max < (BIZ_I2C_CMD_HEADER_LEN + sizeof(st)))
        return 0;

    st.state = s_ota.state;
    st.detail = s_ota.detail;
    st.recv_bytes = s_ota.recv_bytes;
    build_rsp(rsp_out, BIZ_I2C_CMD_OTA_COMMIT, (const uint8_t *)&st, (uint8_t)sizeof(st));
    return (int)(BIZ_I2C_CMD_HEADER_LEN + sizeof(st));
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

    case BIZ_I2C_CMD_OTA_OPEN:
        ret = handle_ota_open(payload, plen, rsp_out, rsp_max);
        break;

    case BIZ_I2C_CMD_OTA_DATA:
        ret = handle_ota_data(payload, plen);
        break;

    case BIZ_I2C_CMD_OTA_COMMIT:
        ret = handle_ota_commit(rsp_out, rsp_max);
        break;

    case BIZ_I2C_CMD_OTA_STATUS:
        ret = ota_status_rsp(rsp_out, rsp_max);
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
