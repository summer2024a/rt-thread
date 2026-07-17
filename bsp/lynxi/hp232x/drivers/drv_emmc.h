/*
 * drv_emmc.h — eMMC hardware driver interface for KA200 RT-Thread HP232x.
 */

#ifndef _DRV_EMMC_H_
#define _DRV_EMMC_H_

#include <rtthread.h>
#include <stdint.h>
#include <stdbool.h>
#include "biz_error_code.h"
#include "biz_host_proto.h"

typedef struct {
    uint32_t read_count;
    uint32_t write_count;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint32_t read_error_count;
    uint32_t write_error_count;
} emmc_statistics_t;

#define EMMC_BLK_SIZE         512
#define EMMC_ADMA_MAX_LEN     (64U * 512U)
#define EMMC_ADMA_DESC_LEN    32
#define EMMC_MAX_ADMA_LINK    4
/* hp640: one ADMA table = ADMA_DESC_LEN(32) * ADMA_MAX_LEN(64KB) per chunk */
#define EMMC_MAX_BLK_PER_XFER (EMMC_ADMA_DESC_LEN * (EMMC_ADMA_MAX_LEN / EMMC_BLK_SIZE))
#define EMMC_MAX_TRANSFER     (EMMC_BLK_SIZE * EMMC_MAX_BLK_PER_XFER)

#ifndef CONFIG_EMMC_BASE_ADDR
#define CONFIG_EMMC_BASE_ADDR 0x10040000ULL
#endif

/* hp640 CONFIG_SPL_BSS @ 0x100040000 — keep same link slot; MMU WB+flush or NC */
#define EMMC_DMA_BASE_ADDR      IRAM1_DMA_NC_START
#define HP232X_DMA_NOCACHE_SEC  ".bss.dma_nocache"
#define HP232X_DMA_BUF_ATTR     __attribute__((aligned(BIZ_ARCH_DMA_MINALIGN), section(HP232X_DMA_NOCACHE_SEC)))

uint8_t *drv_emmc_heartbeat_buf(void);
uint8_t *drv_emmc_query_buf(void);

extern emmc_statistics_t s_emmc_stats;

int drv_emmc_init(void);
int drv_emmc_try_init(bool force);
int drv_emmc_available(void);
void drv_emmc_get_statistics(emmc_statistics_t *stats);
void drv_emmc_flush_dma_buf(void *addr, rt_size_t size);
void drv_emmc_report_error(biz_err_code_t err);

int drv_emmc_read_blocks(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size);
int drv_emmc_write_blocks(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size);
int drv_emmc_exec_bd(uint64_t bd_addr);

#ifdef BSP_EMMC_HS400_100M
uint8_t drv_emmc_dll_offset_get(void);
#endif

#endif /* _DRV_EMMC_H_ */
