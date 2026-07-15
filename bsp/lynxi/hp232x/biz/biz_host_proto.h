/*
 * biz_host_proto.h — Host (FPGA/MCU) task protocol types from lynxi_hp640.h.
 */

#ifndef BIZ_HOST_PROTO_H__
#define BIZ_HOST_PROTO_H__

#include <stdint.h>

#define BIZ_ARCH_DMA_MINALIGN       64
#define BIZ_BLK_SIZE                512

#define HP640_CMD_ERR_CODE          (1U << 0)
#define HP640_CMD_CHECK_EN          (1U << 1)
#define HP640_CMD_CANCLE            (1U << 2)
#define HP640_CMD_CONFIG            (1U << 3)
#define HP640_CMD_FIFO              (1U << 4)
#define HP640_CMD_NOT_END           (1U << 15)

#define HP640_QUERY_TASK_ADDR       0x0500000U   /* hp640 lynxi_hp640.h, FPGA eMMC-Read */
#define HP640_TASK_PKG_BYTES        (HP640_TAKS_PKG_LENGTH * sizeof(HP640_Task))
#define HP640_CHIP_COUNT            32
#define HP640_TAKS_PKG_LENGTH       16
#define BIZ_FLASH_TASK_LIST_ADDR    0xEA000U

typedef struct __attribute__((packed)) HP640_Task
{
    unsigned char chip_id;
    unsigned char cmd;
    unsigned short flag;
    union {
        unsigned int size;
        unsigned int timeout;
        unsigned int count;
        unsigned int apu_config_val;
        unsigned int loop_tag;
        struct {
            union {
                unsigned short blk_cnt;
                unsigned short id;
            };
            union {
                unsigned short max_cnt;
                unsigned short type;
            };
        };
        struct {
            unsigned int batch_cnt : 24;
            unsigned char batch_size;
        };
    };
    union {
        unsigned long src_addr;
        unsigned long src_flash;
        unsigned long loop_counter;
        struct {
            unsigned long core_id : 16;
            unsigned long src_apu : 48;
        };
        unsigned long bd_addr;
        unsigned long imm_value;
        unsigned long microsecond;
        unsigned long timer_begin;
        struct {
            unsigned int duration;
            unsigned int total;
        };
        unsigned long debug_type;
    };
    union {
        unsigned long dst_addr;
        unsigned long ret_addr;
        unsigned long dst_flash;
        unsigned long cfg_offset;
        unsigned long times;
        struct {
            unsigned int value;
            unsigned int mask;
        };
    };
    unsigned int tag;
    unsigned int crc;
} HP640_Task;

typedef struct __attribute__((packed)) HP640_TaskResult
{
    unsigned int tag;
    unsigned int error_code;
} HP640_TaskResult;

typedef struct __attribute__((packed)) HP640_Config
{
    unsigned char chip_id;
    unsigned char heart_beat_interval;
    unsigned char log_level;
    unsigned char unlimited_apu_task;
    unsigned int self_test_report_base_address;
    unsigned int heart_beat_report_base_address;
    unsigned int task_result_report_base_address;
} HP640_Config;

typedef struct __attribute__((packed)) HP640_HeartBeatPackage
{
    unsigned char index;
    union {
        unsigned short status;
        struct {
            unsigned short apu : 1;
            unsigned short ddr_iram : 1;
            unsigned short emmc : 1;
            unsigned short pcie_type : 1;
            unsigned short pcie_status : 1;
            unsigned short pcie_link_speed : 1;
            unsigned short pcie_link_lane : 1;
            unsigned short i2c_status : 1;
            unsigned short chip_type : 1;
            unsigned short reserve : 7;
        };
    };
    unsigned char temp;
    unsigned char vm;
    union {
        unsigned short version;
        struct {
            unsigned char minor_version;
            unsigned char major_version;
        };
    };
    unsigned char dll_offset;
    unsigned int apu_clk;
    unsigned int emmc_rx;
    unsigned int emmc_tx;
    unsigned char uuid[36];
} HP640_HeartBeatPackage;

typedef struct __attribute__((packed)) HP640_SelfTestReport
{
    union {
        unsigned short version;
        struct {
            unsigned char minor_version;
            unsigned char major_version;
        };
    };
    union {
        unsigned short status;
        struct {
            unsigned short ddr_iram : 1;
            unsigned short apu_cr : 1;
            unsigned short spi_flash : 1;
            unsigned short reserve : 13;
        };
    };
} HP640_SelfTestReport;

#define HP640_STRESS_TIME_REG_ADDR      0x00400200U
#define BIZ_FLASH_EMMC_CONFIG_ADDR      0x000E8000UL
#define BIZ_FLASH_DLL_OFFSET_ADDR       (BIZ_FLASH_EMMC_CONFIG_ADDR + 0x29U)

enum HP640_PCIE_MODE
{
    HP640_PCIE_MODE_RC = 0,
    HP640_PCIE_MODE_EP = 1,
};

enum HP640_CMD_CODE
{
    HP640_CMD_Idle          = 0x00,
    HP640_CMD_Load          = 0x01,
    HP640_CMD_Store         = 0x02,
    HP640_CMD_ExecBD        = 0x03,
    HP640_CMD_Write         = 0x04,
    HP640_CMD_DgbRead       = 0x05,
    HP640_CMD_Copy          = 0x06,
    HP640_CMD_Wait          = 0x07,
    HP640_CMD_DgbWait       = 0x08,
    HP640_CMD_FlashRead     = 0x09,
    HP640_CMD_FlashWrite    = 0x0A,
    HP640_CMD_Config        = 0x0B,
    HP640_CMD_SelfTest      = 0x0C,
    HP640_CMD_Delay         = 0x0D,
    HP640_CMD_PWM           = 0x0E,
    HP640_CMD_ApuClock      = 0x0F,
    HP640_CMD_CRC32         = 0x10,
    HP640_CMD_Stress        = 0x11,
    HP640_CMD_Loop          = 0x12,
    HP640_CMD_Timer         = 0x13,
    HP640_CMD_APUDebug      = 0x14,
    HP640_CMD_PCIeStress    = 0x15,
    HP640_CMD_PCIeSetup     = 0x16,
    HP640_CMD_EMMC_DLL_SCAN = 0x17,
    HP640_CMD_SetTimestamp  = 0x18,
};

#endif /* BIZ_HOST_PROTO_H__ */
