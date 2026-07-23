/*
 * biz_i2c_proxy.h — KA200 I2C mailbox agent (MCU Mode B).
 * Matches HP2320 APP Doc/I2C_PROTOCOL.md + ka200_i2c_cmd.h.
 */
#ifndef BIZ_I2C_PROXY_H__
#define BIZ_I2C_PROXY_H__

#include <stdint.h>

#define BIZ_I2C_MAILBOX_REG             0x00U
#define BIZ_I2C_CMD_HEADER_LEN          4U
#define BIZ_I2C_BUF_MAX                 128U
#define BIZ_I2C_PAYLOAD_MAX             (BIZ_I2C_BUF_MAX - BIZ_I2C_CMD_HEADER_LEN)
#define BIZ_I2C_IP_ACCESS_MAX           32U

/* Legacy hp640 READ_LOG (still supported alongside mailbox) */
#define BIZ_I2C_CMD_READ_LOG            0x01U

/* Pull query (optional; MCU builds with HP2320_KA200_CMD_QUERY=0 by default) */
#define BIZ_I2C_CMD_GET_SOC_TEMP        0xC0U
#define BIZ_I2C_CMD_GET_SOC_TIME        0xC1U
#define BIZ_I2C_CMD_GET_SOC_VOL         0xC2U
#define BIZ_I2C_CMD_GET_SOC_STATUS      0xC3U

/* Explicit on-chip IP / register proxy — MCU / mcu_tools primary path */
#define BIZ_I2C_CMD_READ_IP_REG         0xD0U
#define BIZ_I2C_CMD_WRITE_IP_REG        0xD1U

/* Phase B: MCU→KA200 firmware OTA via mailbox (see MCU_OTA_PHASE_B.md) */
#define BIZ_I2C_CMD_OTA_OPEN            0xE8U
#define BIZ_I2C_CMD_OTA_DATA            0xE9U
#define BIZ_I2C_CMD_OTA_COMMIT          0xEAU
#define BIZ_I2C_CMD_OTA_STATUS          0xEBU

#define BIZ_I2C_OTA_STATE_IDLE          0U
#define BIZ_I2C_OTA_STATE_RECV          1U
#define BIZ_I2C_OTA_STATE_WRITING       2U
#define BIZ_I2C_OTA_STATE_OK            3U
#define BIZ_I2C_OTA_STATE_FAIL          4U

#define BIZ_I2C_OTA_FLASH_ADDR_DEFAULT  0x000A6000UL

/* Absolute MMIO address + length (no base+offset; saves 4B on wire) */
typedef struct __attribute__((packed)) {
    uint32_t reg_addr;
    uint8_t  access_len;
} biz_i2c_ip_reg_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t img_crc32;
    uint32_t flash_addr;
} biz_i2c_ota_open_t;

typedef struct __attribute__((packed)) {
    uint8_t state;
    uint8_t detail;
    uint32_t recv_bytes; /* high-water bytes accepted into IRAM */
} biz_i2c_ota_status_t;

uint8_t biz_i2c_calc_crc(const uint8_t *pdat, uint32_t len);

/*
 * Handle a complete mailbox request frame (bytes after I2C reg select).
 * On success with a reply: fills rsp_out and returns total frame length (>= 4).
 * Push / write-only success: returns 0 (no I2C read expected).
 * On error: returns -1 (may still fill a error-style rsp if rsp_out non-NULL).
 */
int biz_i2c_proxy_handle(const uint8_t *req, uint16_t req_len,
                         uint8_t *rsp_out, uint16_t rsp_max);

#endif /* BIZ_I2C_PROXY_H__ */
