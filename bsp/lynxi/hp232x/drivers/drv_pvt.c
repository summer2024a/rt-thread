/*
 * drv_pvt.c — KA200 PVT read-only driver for RT-Thread HP232x BSP.
 * Ported from hp640_arm/common/spl/spl_pvt.c (no IRQ, no dump).
 */

#include <rtthread.h>
#include <stdint.h>
#include <string.h>
#include "drv_pvt.h"
#include "tick.h"

#define APB_PVT_BASE_ADDR               0x12200000UL

#define APB_PVT_TM_SCRATCH_REG_OFFSET   0x000C
#define APB_PVT_TS_REG_OFFSET           0x0080
#define APB_PVT_TS_ONE_SONSOR_SZIE      0x0040
#define APB_PVT_TS_SMPL_CTRL_OFFSET     0x00A0
#define APB_PVT_PD_REG_OFFSET           0x0200
#define APB_PVT_PD_ONE_DETECTOR_SZIE    0x0040
#define APB_PVT_PD_SMPL_CTRL_OFFSET     0x0220
#define APB_PVT_FIRST_PD_OFFSET_OFFSET  0x0040
#define APB_PVT_VM_REG_OFFSET           0x0400
#define APB_PVT_VM_CHANNEL_OFFSET       0x0240
#define APB_PVT_VM_ONE_CHANNEL_SZIE     0x0004
#define APB_PVT_VM_SMPL_CTRL_OFFSET     0x0420
#define APB_PVT_VM_ONE_MONITOR_SZIE     0x0200

#define APB_PVT_SDIF_REG_OFFSET_OFFSET      0x000C
#define APB_PVT_SDIF_DONE_OFFSET_OFFSET     0x0014
#define APB_PVT_SDIF_DATA_OFFSET_OFFSET     0x0018
#define APB_PVT_SDIF_STATUS_OFFSET_OFFSET   0x0008
#define APB_PVT_SDIF_OFFSET_OFFSET          0x000C

#define SDIF_CONFIG_SIZE                    8
#define SDIF_SECOND_CONFIG_OFFSET           8
#define SDIF_IP_CTRL_VM_MODE_OFFSET         10

#define SDIF_CFG1_TS_OPERATION_MODE_BITSOFFSET  0
#define SDIF_CFG1_TS_OPERATION_MODE_BITSSIZE    4
#define SDIF_CFG1_TS_OUTPUT_OPTION_BITSOFFSET   5
#define SDIF_CFG1_TS_OUTPUT_OPTION_BITSSIZE     3

#define SDIF_CFG2_PD_PRESCALE_B_BITSOFFSET  5
#define SDIF_CFG2_PD_PRESCALE_B_BITSSIZE    3
#define SDIF_CFG3_PD_PRESCALE_A_BITSOFFSET  0
#define SDIF_CFG3_PD_PRESCALE_A_BITSSIZE    4
#define SDIF_CFG3_PD_WINDOW_SIZE_BITSOFFSET 4
#define SDIF_CFG3_PD_WINDOW_SIZE_BITSSIZE   4

#define SDIF_CFG1_VM_DATA_MODE_BITSOFFSET       4
#define SDIF_CFG1_VM_DATA_MODE_BITSSIZE         1
#define SDIF_CFG1_VM_OUTPUT_RESCTL_BITSOFFSET   5
#define SDIF_CFG1_VM_OUTPUT_RESCTL_BITSSIZE     2
#define SDIF_CFG2_VM_VREF_SELECT_BITSOFFSET     7
#define SDIF_CFG2_VM_VREF_SELECT_BITSSIZE       1

#define SDIF_SMPL_DONE_BITSIZE                  0
#define SDIF_DATA_SAMPLE_FAULT_BITOFFSET        17
#define SDIF_DATA_SAMPLE_TYPE_BITOFFSET         16
#define SDIF_DATA_SAMPLE_DATA_BITSSIZE          16

#define INPUT_FCLK              6.25f
#define DELAY_LOOP_GATE_NUM     91

#define IP_CTRL_UP              0x88000108U
#define IP_TMR                  0x8B000512U
#define IP_TS_CFG               0x89000000U
#define IP_PD_CFG               0x89002000U
#define IP_VM_CFG               0x89008000U
#define IP_POLL_0               0x8C000000U
#define CLK_WORK                0x01010303U

#define PVT_UDELAY(t)           rt_hw_us_delay(t)

typedef enum {
    PVT_TS = 1,
    PVT_VM = 2,
    PVT_PD = 3,
} pvt_type_t;

struct pvt_ts_para {
    float slope;
    float offset;
    unsigned int bitwide;
    unsigned int operation_mode;
};

struct pvt_pd_para {
    unsigned int cws;
    unsigned int ratioa;
    unsigned int ratiob;
    unsigned int fclk;
    unsigned int ngates;
    unsigned int bitwide;
};

struct pvt_vm_para {
    float vref;
    unsigned int vrefmode;
    unsigned int bitwide;
    unsigned int datamode;
};

struct sdif_config {
    unsigned long addr;
    unsigned long datastart;
    int (*register_parse)(unsigned int, void *);
    int (*para_init)(struct sdif_config *);
    int (*calc_ret)(struct sdif_config *, float *);
    pvt_type_t type;
    unsigned int unitnum;
    unsigned int unitstartno;
    unsigned int unitsize;
    unsigned int ipcfg;
    void *data;
    unsigned int init_flag;
};

static inline uint32_t pvt_readl(uintptr_t addr)
{
    return *(volatile uint32_t *)(addr);
}

static inline void pvt_writel(uint32_t val, uintptr_t addr)
{
    *(volatile uint32_t *)(addr) = val;
}

static unsigned int get_bit_value(unsigned int val, int index)
{
    return 0x1u & (val >> index);
}

static unsigned int set_bit_value(unsigned int val, rt_bool_t flag, int index)
{
    if (flag == RT_TRUE)
        return val | (0x1u << index);
    return val & (~(0x1u << index));
}

static unsigned int get_nbits_value(unsigned int val, int bitnum)
{
    unsigned int ret = 0;
    int i;

    for (i = 0; i < bitnum; i++)
        ret += ((1u << i) & val);
    return ret;
}

static void pvt_alive_test(void)
{
    uintptr_t reg_addr = APB_PVT_BASE_ADDR + APB_PVT_TM_SCRATCH_REG_OFFSET;
    static rt_bool_t done = RT_FALSE;
    uint32_t rdata;

    if (done)
        return;

    pvt_writel(0xff00ff00, reg_addr);
    PVT_UDELAY(10);
    do {
        rdata = pvt_readl(reg_addr);
    } while (rdata != 0xff00ff00);
    done = RT_TRUE;
}

static void configure_clock_work(pvt_type_t pvt)
{
    uintptr_t clk_addr;

    if (pvt == PVT_TS)
        clk_addr = APB_PVT_BASE_ADDR + 0x80;
    else if (pvt == PVT_PD)
        clk_addr = APB_PVT_BASE_ADDR + 0x200;
    else
        clk_addr = APB_PVT_BASE_ADDR + 0x400;

    if (pvt_readl(clk_addr) == CLK_WORK)
        return;

    pvt_writel(0, clk_addr + 0x10);
    pvt_writel(0, clk_addr + 0x4);
    pvt_writel(CLK_WORK, clk_addr);
    PVT_UDELAY(10);
}

static void configure_smpl_ctrl(pvt_type_t pvt)
{
    uintptr_t ctrl_addr;

    if (pvt == PVT_TS)
        ctrl_addr = APB_PVT_BASE_ADDR + APB_PVT_TS_SMPL_CTRL_OFFSET;
    else if (pvt == PVT_PD)
        ctrl_addr = APB_PVT_BASE_ADDR + APB_PVT_PD_SMPL_CTRL_OFFSET;
    else
        ctrl_addr = APB_PVT_BASE_ADDR + APB_PVT_VM_SMPL_CTRL_OFFSET;

    if (pvt_readl(ctrl_addr) == 0)
        return;
    pvt_writel(0, ctrl_addr);
    PVT_UDELAY(10);
}

static void pvt_ctrl_init(pvt_type_t pvt)
{
    pvt_alive_test();
    configure_clock_work(pvt);
    configure_smpl_ctrl(pvt);
}

static void configure_sda_register(uintptr_t sdif_status_addr,
                                   uintptr_t sdif_addr, uint32_t value)
{
    while (get_nbits_value(pvt_readl(sdif_status_addr), 2) == 0x1)
    PVT_UDELAY(10);
    pvt_writel(value, sdif_addr);
    PVT_UDELAY(5000);
}

static int read_pvt_sample_data(uintptr_t offsetstart, unsigned int bitwide)
{
    uintptr_t sdif_done_addr;
    uintptr_t sdif_data_addr;
    uint32_t sdif_data = 0;
    int try_cnt = 5;
    int val;

    (void)bitwide;

    if (offsetstart > (APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET + APB_PVT_VM_ONE_MONITOR_SZIE))
    {
        sdif_done_addr = APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET +
                         APB_PVT_VM_CHANNEL_OFFSET - APB_PVT_SDIF_REG_OFFSET_OFFSET;
        sdif_data_addr = offsetstart + APB_PVT_SDIF_DATA_OFFSET_OFFSET;
    }
    else
    {
        sdif_done_addr = offsetstart + APB_PVT_SDIF_DONE_OFFSET_OFFSET;
        sdif_data_addr = offsetstart + APB_PVT_SDIF_DATA_OFFSET_OFFSET;
    }

read_sensor:
    while (!get_bit_value(pvt_readl(sdif_done_addr), SDIF_SMPL_DONE_BITSIZE))
        PVT_UDELAY(10);

    sdif_data = pvt_readl(sdif_data_addr);
    while (get_bit_value(sdif_data, SDIF_DATA_SAMPLE_TYPE_BITOFFSET) == 0x1)
    {
        PVT_UDELAY(10);
        sdif_data = pvt_readl(sdif_data_addr);
    }

    val = (int)get_nbits_value(sdif_data, SDIF_DATA_SAMPLE_DATA_BITSSIZE);
    if (get_bit_value(sdif_data, SDIF_DATA_SAMPLE_FAULT_BITOFFSET))
    {
        if (--try_cnt < 0)
            return -1;
            goto read_sensor;
    }
    return val;
}

static int read_serial_data_interface_register(struct sdif_config *config)
{
    uintptr_t sdif_status_addr;
    uintptr_t sdif_addr;
    uint32_t val;

    sdif_status_addr = config->addr + APB_PVT_SDIF_STATUS_OFFSET_OFFSET;
    sdif_addr = config->addr + APB_PVT_SDIF_OFFSET_OFFSET;

    configure_sda_register(sdif_status_addr, sdif_addr,
        set_bit_value(IP_CTRL_UP, (config->type == PVT_VM), SDIF_IP_CTRL_VM_MODE_OFFSET));
    configure_sda_register(sdif_status_addr, sdif_addr, IP_TMR);

    if (config->type == PVT_VM)
    {
        configure_sda_register(sdif_status_addr, sdif_addr,
            IP_POLL_0 | (((2u << config->unitnum) - 1u) << config->unitstartno));
        PVT_UDELAY(100);
    }

    configure_sda_register(sdif_status_addr, sdif_addr, config->ipcfg);
    val = pvt_readl(sdif_addr);

    if (config->register_parse && config->data)
        return config->register_parse(val, config->data);
    return 0;
}

static float calc_one_sensor_temperature(unsigned int val, struct pvt_ts_para *para)
{
    if (!para)
        return 0.0f;
    if (para->operation_mode)
        return (float)(59.1 + 202.8 * (val / 4096.0 - 0.5) - 0.16 * INPUT_FCLK);
    return (float)(42.74 + 220.5 * (val / 4096.0 - 0.5) - 0.16 * INPUT_FCLK);
}

static int ts_sdif_config_register_parse(unsigned int val, void *data)
{
    struct pvt_ts_para *para = data;
    unsigned int config;
    unsigned int mode;
    unsigned int bits;

    config = get_nbits_value(val, SDIF_CONFIG_SIZE);
    mode = get_nbits_value(config >> SDIF_CFG1_TS_OPERATION_MODE_BITSOFFSET,
                           SDIF_CFG1_TS_OPERATION_MODE_BITSSIZE);
    if (mode > 1)
        return -1;
    para->operation_mode = mode;

    bits = get_nbits_value(config >> SDIF_CFG1_TS_OUTPUT_OPTION_BITSOFFSET,
                           SDIF_CFG1_TS_OUTPUT_OPTION_BITSSIZE);
    if (bits == 0)
        para->bitwide = 12;
    else if (bits == 1)
        para->bitwide = 10;
    else if (bits == 2)
        para->bitwide = 8;
    else
            return -1;
    return 0;
}

static int sensor_temperature_convert_para_init(struct sdif_config *config)
{
    struct pvt_ts_para *para = config->data;

    para->slope = 1.181818f;
    para->offset = -12.72728f;
    if (config->init_flag)
    return 0;
    return read_serial_data_interface_register(config);
}

static int pvt_ts_calc_and_return(struct sdif_config *config, float *result)
{
    struct pvt_ts_para *para = config->data;
    int i;

    for (i = 0; i < DRV_PVT_TS_COUNT; i++)
    {
        uintptr_t offsetstart = config->datastart + i * config->unitsize;
        int val = read_pvt_sample_data(offsetstart, para->bitwide);

        if (val < 0)
        return -1;
        result[i] = calc_one_sensor_temperature((unsigned int)val, para);
    }
    return 0;
}

static float calc_voltage_val(unsigned int val, struct pvt_vm_para *para)
{
    (void)para;
    return (float)((val - 2753) * 1.4513 / 16384);
}

static int vm_sdif_config_register_parse(unsigned int val, void *data)
{
    struct pvt_vm_para *para = data;
    unsigned int config;
    unsigned int config1;
    unsigned int config2;
    unsigned int bitwidthflag;

    config = get_nbits_value(val, 2 * SDIF_CONFIG_SIZE);
    config1 = get_nbits_value(config, SDIF_CONFIG_SIZE);
    config2 = get_nbits_value(config >> SDIF_SECOND_CONFIG_OFFSET, SDIF_CONFIG_SIZE);

    para->datamode = get_nbits_value(config1 >> SDIF_CFG1_VM_DATA_MODE_BITSOFFSET,
                                     SDIF_CFG1_VM_DATA_MODE_BITSSIZE);
    bitwidthflag = get_nbits_value(config1 >> SDIF_CFG1_VM_OUTPUT_RESCTL_BITSOFFSET,
                                   SDIF_CFG1_VM_OUTPUT_RESCTL_BITSSIZE);
    if (bitwidthflag == 0)
        para->bitwide = 14;
    else if (bitwidthflag == 1)
        para->bitwide = 12;
    else if (bitwidthflag == 2)
        para->bitwide = 10;
    else
        para->bitwide = 8;

    para->vrefmode = get_nbits_value(config2 >> SDIF_CFG2_VM_VREF_SELECT_BITSOFFSET,
                                     SDIF_CFG2_VM_VREF_SELECT_BITSSIZE);
    return 0;
}

static int voltage_monitor_convert_para_init(struct sdif_config *config)
{
    struct pvt_vm_para *para = config->data;

    para->vref = para->vrefmode ? 1.0f : 1.2077f;
    if (config->init_flag)
        return 0;
    return read_serial_data_interface_register(config);
}

static int pvt_vm_calc_and_return(struct sdif_config *config, float *result)
{
    struct pvt_vm_para *para = config->data;
    unsigned int i;

    for (i = config->unitstartno; i < config->unitstartno + config->unitnum; i++)
    {
        uintptr_t offsetstart = config->datastart + i * config->unitsize;
        int val = read_pvt_sample_data(offsetstart, para->bitwide);

        if (val < 0)
        return -1;
        result[i] = calc_voltage_val((unsigned int)val, para);
    }
    return 0;
}

static float calc_delay_loop_frequency(unsigned int val, struct pvt_pd_para *para)
{
    if (!para || !para->cws)
        return 0.0f;
    return (float)((val * para->ratioa * para->ratiob * para->fclk) / para->cws);
}

static float calc_delay_gate_delay(float floop, struct pvt_pd_para *para)
{
    if (!para || !para->ngates || !floop)
        return 0.0f;
    return (float)(1.0 / (floop * 2 * para->ngates));
}

static int pd_sdif_config_register_parse(unsigned int val, void *data)
{
    struct pvt_pd_para *para = data;
    unsigned int config;
    unsigned int config2;
    unsigned int config3;
    unsigned int ratiob_flag;
    unsigned int ratioa_flag;
    unsigned int cws_flag;

    config = get_nbits_value(val, 3 * SDIF_CONFIG_SIZE) >> SDIF_SECOND_CONFIG_OFFSET;
    config2 = get_nbits_value(config, SDIF_SECOND_CONFIG_OFFSET);
    ratiob_flag = get_nbits_value(config2 >> SDIF_CFG2_PD_PRESCALE_B_BITSOFFSET,
                                  SDIF_CFG2_PD_PRESCALE_B_BITSSIZE);
    if (ratiob_flag > 6)
        para->ratiob = 1;
    else if (ratiob_flag > 0)
        para->ratiob = 4;
    else
        return -1;

    config3 = config >> SDIF_CONFIG_SIZE;
    ratioa_flag = get_nbits_value(config3 >> SDIF_CFG3_PD_PRESCALE_A_BITSOFFSET,
                                  SDIF_CFG3_PD_PRESCALE_A_BITSSIZE);
    para->ratioa = (ratioa_flag == 0x3) ? 1 : (4u * (1u << ratioa_flag));

    cws_flag = get_nbits_value(config3 >> SDIF_CFG3_PD_WINDOW_SIZE_BITSOFFSET,
                               SDIF_CFG3_PD_WINDOW_SIZE_BITSSIZE);
    para->cws = (256u / (1u << cws_flag)) - 1u;
    return 0;
}

static int process_detector_convert_para_init(struct sdif_config *config)
{
    struct pvt_pd_para *para = config->data;

    para->ngates = DELAY_LOOP_GATE_NUM;
    para->fclk = (unsigned int)INPUT_FCLK;
    para->bitwide = 12;
    if (config->init_flag)
        return 0;
    return read_serial_data_interface_register(config);
}

static int pvt_pd_calc_and_return(struct sdif_config *config, float *result)
{
    struct pvt_pd_para *para = config->data;
    unsigned int i;
    unsigned int out = 0;

    for (i = config->unitstartno; i < config->unitstartno + config->unitnum; i++)
    {
        uintptr_t offsetstart = config->datastart + i * config->unitsize;
        int val = read_pvt_sample_data(offsetstart, para->bitwide);
        float floop;
        float gdelay;

        if (val < 0)
                return -1;

        floop = calc_delay_loop_frequency((unsigned int)val, para);
        if (para->ratiob != 4)
            para->ngates = 0;
        gdelay = (para->ngates == 0) ? 0.0f : calc_delay_gate_delay(floop, para);

        result[out++] = floop;
        result[out++] = gdelay;
    }
    return 0;
}

/* hp640 pvt_vm_calc_and_return_apu_vm() */
static int pvt_vm_calc_and_return_apu_vm(struct sdif_config *config, float *result)
{
    struct pvt_vm_para *para = config->data;
    int i;

    for (i = 1; i < DRV_PVT_APU_VM_COUNT; i++)
    {
        uintptr_t offsetstart = config->datastart + i * config->unitsize;
        int val = read_pvt_sample_data(offsetstart, para->bitwide);

        if (val < 0)
        {
            if (config->unitnum == 1)
                return -1;
            continue;
        }
        result[i] = calc_voltage_val((unsigned int)val, para);
    }

    return 0;
}

static int pvt_read(struct sdif_config *config, float *result)
{
    if (!config || !result)
        return -1;

    pvt_ctrl_init(config->type);
    if (!config->para_init || config->para_init(config) != 0)
        return -1;
    if (!config->calc_ret)
        return -1;
    return config->calc_ret(config, result);
    }

int drv_pvt_read_temperature(float *result, uint32_t init)
    {
    static struct sdif_config cfg;
    static struct pvt_ts_para para;

    if (!result)
        return -1;
    if (!init)
        memset(&cfg, 0, sizeof(cfg));

    memset(&para, 0, sizeof(para));
    cfg.init_flag = init;
    cfg.data = &para;
    cfg.type = PVT_TS;
    cfg.ipcfg = IP_TS_CFG;
    cfg.unitnum = DRV_PVT_TS_COUNT;
    cfg.unitstartno = 0;
    cfg.unitsize = APB_PVT_TS_ONE_SONSOR_SZIE;
    cfg.addr = APB_PVT_BASE_ADDR + APB_PVT_TS_REG_OFFSET;
    cfg.datastart = APB_PVT_BASE_ADDR + APB_PVT_TS_REG_OFFSET + APB_PVT_TS_ONE_SONSOR_SZIE;
    cfg.register_parse = ts_sdif_config_register_parse;
    cfg.para_init = sensor_temperature_convert_para_init;
    cfg.calc_ret = pvt_ts_calc_and_return;

    return pvt_read(&cfg, result);
}

int drv_pvt_read_voltage(float *result, uint32_t init)
{
    static struct sdif_config cfg;
    static struct pvt_vm_para para;

    if (!result)
        return -1;
    if (!init)
        memset(&cfg, 0, sizeof(cfg));

    memset(&para, 0, sizeof(para));
    cfg.init_flag = init;
    cfg.data = &para;
    cfg.type = PVT_VM;
    cfg.ipcfg = IP_VM_CFG;
    cfg.unitnum = DRV_PVT_VM_COUNT;
    cfg.unitstartno = 0;
    cfg.unitsize = APB_PVT_VM_ONE_CHANNEL_SZIE;
    cfg.addr = APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET;
    cfg.datastart = APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET +
                    APB_PVT_VM_CHANNEL_OFFSET - APB_PVT_SDIF_DATA_OFFSET_OFFSET;
    cfg.register_parse = vm_sdif_config_register_parse;
    cfg.para_init = voltage_monitor_convert_para_init;
    cfg.calc_ret = pvt_vm_calc_and_return;

    return pvt_read(&cfg, result);
}

/* hp640 read_pvt_vm_sonsor() — APU VM channels for heartbeat */
int drv_pvt_read_apu_vm(float *result, uint32_t init)
{
    static struct sdif_config cfg;
    static struct pvt_vm_para para;

    if (!result)
        return -1;
    if (!init)
        memset(&cfg, 0, sizeof(cfg));

    memset(&para, 0, sizeof(para));
    cfg.init_flag = init;
    cfg.data = &para;
    cfg.type = PVT_VM;
    cfg.ipcfg = IP_VM_CFG;
    cfg.unitnum = DRV_PVT_APU_VM_COUNT;
    cfg.unitstartno = 1;
    cfg.unitsize = APB_PVT_VM_ONE_CHANNEL_SZIE;
    cfg.addr = APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET;
    cfg.datastart = APB_PVT_BASE_ADDR + APB_PVT_VM_REG_OFFSET +
                    APB_PVT_VM_CHANNEL_OFFSET - APB_PVT_SDIF_DATA_OFFSET_OFFSET;
    cfg.register_parse = vm_sdif_config_register_parse;
    cfg.para_init = voltage_monitor_convert_para_init;
    cfg.calc_ret = pvt_vm_calc_and_return_apu_vm;

    return pvt_read(&cfg, result);
}

int drv_pvt_read_process_detector(float *floop, float *gdelay,
                                  uint32_t startno, uint32_t num)
{
    static struct sdif_config cfg;
    static struct pvt_pd_para para;
    float buf[DRV_PVT_PD_COUNT * 2];
    unsigned int i;

    if (!floop || !gdelay || startno >= DRV_PVT_PD_COUNT ||
        num == 0 || (startno + num) > DRV_PVT_PD_COUNT)
        return -1;

    memset(&cfg, 0, sizeof(cfg));
    memset(&para, 0, sizeof(para));
    cfg.init_flag = 0;
    cfg.data = &para;
    cfg.type = PVT_PD;
    cfg.ipcfg = IP_PD_CFG;
    cfg.unitnum = num;
    cfg.unitstartno = startno;
    cfg.unitsize = APB_PVT_PD_ONE_DETECTOR_SZIE;
    cfg.addr = APB_PVT_BASE_ADDR + APB_PVT_PD_REG_OFFSET;
    cfg.datastart = APB_PVT_BASE_ADDR + APB_PVT_PD_REG_OFFSET + APB_PVT_FIRST_PD_OFFSET_OFFSET;
    cfg.register_parse = pd_sdif_config_register_parse;
    cfg.para_init = process_detector_convert_para_init;
    cfg.calc_ret = pvt_pd_calc_and_return;

    if (pvt_read(&cfg, buf) != 0)
        return -1;

    for (i = 0; i < num; i++)
    {
        floop[i] = buf[i * 2];
        gdelay[i] = buf[i * 2 + 1];
    }
    return 0;
}
