/*
 * biz_apu_dump.c — APU failed-status text dump (hp640 getFailedStatus simplified).
 */
#include "biz_modules.h"

#if defined(BIZ_MOD_EXEC_APU)

#include <stdio.h>
#include <string.h>
#include "drv_apu.h"
#include "biz_log.h"
#include "hp232x_mmu.h"
#include "biz_error_code.h"

#define APU_CR_AXI                  HP232X_APU_BASE

int biz_apu_dump_failed_status(uint32_t chip_id, unsigned char *addr, unsigned int size)
{
    char line[160];
    unsigned char *cur = addr;
    unsigned char *end;
    uint32_t phasecnt, cr_status, reg_task_en, reg_busy;

    if (!addr || size < 64)
        return BIZ_ERR_CLI_PARAM;

    end = addr + size;
    phasecnt = drv_apu_read32(0x189178UL);
    cr_status = drv_apu_read32(DRV_APU_STAT_OFFSET);
    reg_task_en = drv_apu_read32(0x188174UL);
    reg_busy = drv_apu_read32(0x18816cUL);

    rt_snprintf(line, sizeof(line),
        "[ chip%02u ] APU phase=0x%x cr=0x%x busy=%u taskEN=0x%x\r\n",
        chip_id, phasecnt & 0xFFFFU, cr_status,
        reg_busy & 1U, reg_task_en);
    if (cur + strlen(line) >= end)
        return BIZ_ERR_PRIM_PARAM;
    memcpy(cur, line, strlen(line));
    cur += strlen(line);

    rt_snprintf(line, sizeof(line),
        "[ chip%02u ] dump size=%u (simplified hp640 getFailedStatus)\r\n",
        chip_id, size);
    if (cur + strlen(line) >= end)
        return BIZ_ERR_PRIM_PARAM;
    memcpy(cur, line, strlen(line));

    return BIZ_SUCCESS;
}

#endif /* BIZ_MOD_EXEC_APU */
