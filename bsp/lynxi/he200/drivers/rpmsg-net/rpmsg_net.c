/*
 * Lightweight RPMSG-based network driver for RT-Thread
 *
 * This driver implements a minimal eth_device frontend using rpmsg-lite
 * to transport ethernet frames between cores.
 *
 * Notes:
 * - Adjust RPMSG API calls (rpmsg_lite_*) to your platform's rpmsg-lite binding.
 * - Keep payload size within MAX_PKT_SIZE.
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>

// #ifdef PKG_USING_RPMSG_LITE /* or PKG_USING_RPMSG depending on your config */

#include <rtdevice.h>
#include <netif/ethernetif.h>
#include <lwip/pbuf.h>

/* RPMSG headers - adapt to your rpmsg-lite include paths */
#include "rpmsg_lite.h"
#include "platform/rpmsg_platform.h"

#define RPMSG_NET_RX_MQ_NAME   "rpnet_rx_mq"
#define RPMSG_NET_RX_MQ_MSGSZ  sizeof(void *)
#define RPMSG_NET_RX_MQ_ENTRIES 32
#define RPMSG_NET_RX_MQ_POOL_SIZE (RPMSG_NET_RX_MQ_MSGSZ * RPMSG_NET_RX_MQ_ENTRIES)

#define MAX_PKT_SIZE 1536
#define RPMSG_NET_EPT_NAME "rpmsg-net"
#define RPMSG_NET_LOCAL_EPT_ADDR   0x101U
#define RPMSG_NET_REMOTE_EPT_ADDR  0x100U

#ifdef DBG_TAG
#undef DBG_TAG
#undef DBG_LVL
#endif

#define DBG_TAG "rpmsg-net"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

struct rpmsg_net_device
{
    struct eth_device parent;
    struct rpmsg_lite_instance *rpmsg_inst;
    void *rpmsg_ept;
    struct rt_messagequeue rx_mq;
    char rx_mq_pool[RPMSG_NET_RX_MQ_POOL_SIZE];
    char name[RT_NAME_MAX];
};

static struct rpmsg_net_device *g_rpmsg_net_dev = RT_NULL;

/* 设备状态标志 */
static rt_bool_t g_rpmsg_net_initialized = RT_FALSE;

/* RPMSG receive callback (rpmsg-lite API) */
static int32_t rpmsg_net_recv_cb(void *payload, uint32_t len, uint32_t src, void *priv)
{
    // struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)priv;
    struct rpmsg_net_device *rdev = g_rpmsg_net_dev;
    struct pbuf *p;
    void *ptr;

    LOG_D("Received packet from src %u, len %u", src, len);

    if (!rdev)
    {
        LOG_E("rpmsg_net_dev is NULL");
        return -1;
    }

    if (len == 0 || len > MAX_PKT_SIZE)
    {
        LOG_E("Invalid packet length: %u", len);
        return -1;
    }

    /* allocate pbuf for incoming frame */
    p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == RT_NULL)
    {
        LOG_E("Failed to allocate pbuf");
        return -1;
    }

    /* copy payload into pbuf chain */
    pbuf_take(p, payload, len);

    /* send pbuf pointer to mailbox */
    ptr = p;
    if (rt_mq_send(&rdev->rx_mq, &ptr, sizeof(ptr)) != RT_EOK)
    {
        LOG_E("Failed to send pbuf to mailbox");
        pbuf_free(p);
        return -1;
    }

    /* notify net stack that device has packet */
    eth_device_ready(&rdev->parent);
    LOG_D("Packet forwarded to network stack");

    return 0;
}

/* transmit via rpmsg */
static rt_err_t rpmsg_net_tx(rt_device_t dev, struct pbuf *p)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;
    void *buf;
    uint32_t total_len = p->tot_len;
    uint32_t copied = 0;
    struct pbuf *q;
    rt_err_t ret = RT_EOK;

    /* 检查初始化标志 */
    if (g_rpmsg_net_initialized == RT_FALSE)
    {
        LOG_W("rpmsg-net not ready, drop packet len=%d", p->tot_len);
        return -RT_EBUSY;  /* 让 LWIP 重试 */
    }

    LOG_D("Transmitting packet, len %u", total_len);

    //打印pbuf的payload
    // LOG_HEX("pbuf", 16, p->payload, p->tot_len);

    if (total_len == 0 || total_len > MAX_PKT_SIZE)
    {
        LOG_E("Invalid packet length: %u", total_len);
        return -RT_ERROR;
    }

    /* temporary linear buffer */
    buf = rt_malloc(total_len);
    if (!buf)
    {
        LOG_E("Failed to allocate buffer");
        return -RT_ENOMEM;
    }

    for (q = p; q != RT_NULL; q = q->next)
    {
        memcpy((char *)buf + copied, q->payload, q->len);
        copied += q->len;
    }

    /* send using rpmsg-lite */
    ret = rpmsg_lite_send(rdev->rpmsg_inst, rdev->rpmsg_ept, 0, (char *)buf, total_len, 1000/*RL_BLOCK*/);
    if (RL_SUCCESS != ret)
    {
        LOG_E("Failed to send packet via rpmsg-lite, ret=%d", ret);
        rt_free(buf);
        return -RT_ERROR;
    }

    LOG_D("Packet sent successfully");
    rt_free(buf);
    return RT_EOK;
}

/* poll / receive hook for eth device */
static struct pbuf *rpmsg_net_rx(rt_device_t dev)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;
    void *ptr;
    struct pbuf *p = RT_NULL;

    if (rt_mq_recv(&rdev->rx_mq, &ptr, sizeof(ptr), 0) == RT_EOK)
    {
        p = (struct pbuf *)ptr;
        LOG_D("Received packet from mailbox, len %u", p->tot_len);
    }

    return p;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops rpmsg_net_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL
};
#endif

/* initialize eth device */
static __attribute__((unused))rt_err_t rpmsg_net_init(rt_device_t dev)
{
    RT_UNUSED(dev);
    return RT_EOK;
}

/* control operations */
static __attribute__((unused)) rt_err_t rpmsg_net_control(rt_device_t dev, int cmd, void *args)
{
    RT_UNUSED(dev);
    RT_UNUSED(cmd);
    RT_UNUSED(args);
    return -RT_EINVAL;
}

/* create and register the rpmsg-net device */
int rpmsg_net_device_register(struct rpmsg_lite_instance *inst, void *ept)
{
    struct rpmsg_net_device *rdev;
    rt_err_t err;

    LOG_D("Registering rpmsg-net device");

    if (inst == RT_NULL || ept == RT_NULL)
    {
        LOG_E("Invalid parameters: inst=%p, ept=%p", inst, ept);
        return -RT_ERROR;
    }

    rdev = rt_malloc(sizeof(*rdev));
    if (!rdev)
    {
        LOG_E("Failed to allocate device structure");
        return -RT_ENOMEM;
    }

    memset(rdev, 0, sizeof(*rdev));
    rdev->rpmsg_inst = inst;
    rdev->rpmsg_ept = ept;
    rt_snprintf(rdev->name, RT_NAME_MAX, "rpnet0");

    /* create mailbox for rx pbuf pointers */
    err = rt_mq_init(&rdev->rx_mq, RPMSG_NET_RX_MQ_NAME, rdev->rx_mq_pool, RPMSG_NET_RX_MQ_MSGSZ,
                     RPMSG_NET_RX_MQ_POOL_SIZE, RT_IPC_FLAG_FIFO);
    if (err != RT_EOK)
    {
        LOG_E("Failed to initialize mailbox");
        rt_free(rdev);
        return -RT_ERROR;
    }

    /* setup eth_device */
    rdev->parent.parent.type = RT_Device_Class_NetIf;
#ifdef RT_USING_DEVICE_OPS
    rdev->parent.parent.ops = &rpmsg_net_ops;
#else
    rdev->parent.parent.init = rpmsg_net_init;
    rdev->parent.parent.control = rpmsg_net_control;
#endif
    rdev->parent.eth_rx = rpmsg_net_rx;
    rdev->parent.eth_tx = rpmsg_net_tx;

    /* register device */
    if (eth_device_init(&rdev->parent, rdev->name) != RT_EOK)
    {
        LOG_E("Failed to initialize eth device");
        rt_mq_detach(&rdev->rx_mq);
        rt_free(rdev);
        return -RT_ERROR;
    }

    /* store global pointer for callback access */
    g_rpmsg_net_dev = rdev;
    LOG_D("rpmsg-net device registered successfully: %s", rdev->name);

    return RT_EOK;
}

/* helper to unregister */
void rpmsg_net_device_unregister(void)
{
    if (!g_rpmsg_net_dev)
        return;

    rt_device_close(&g_rpmsg_net_dev->parent.parent);
    rt_device_unregister(&g_rpmsg_net_dev->parent.parent);
    rt_mq_detach(&g_rpmsg_net_dev->rx_mq);
    rt_free(g_rpmsg_net_dev);
    g_rpmsg_net_dev = RT_NULL;
}

/* Example entry: create endpoint and register device
   Adapt rpmsg_lite_endpoint_create params to your rpmsg implementation.
*/
int rpmsg_net_start(struct rpmsg_lite_instance *inst)
{
    void *ept;

    LOG_D("Starting rpmsg-net device");

    if (!inst)
    {
        LOG_E("Invalid rpmsg instance");
        return -RT_ERROR;
    }

    /* create endpoint: rpmsg_lite_create_ept(inst, addr, rpmsg_net_recv_cb, priv) */
    ept = rpmsg_lite_create_ept(inst, RPMSG_NET_LOCAL_EPT_ADDR, rpmsg_net_recv_cb, RT_NULL);
    if (!ept)
    {
        LOG_E("Failed to create rpmsg endpoint");
        return -RT_ERROR;
    }

    LOG_D("rpmsg endpoint created successfully");
    return rpmsg_net_device_register(inst, ept);
}

/* 等待线程相关变量 */
static struct rt_thread *rpmsg_link_wait_thread = RT_NULL;
static struct rpmsg_lite_instance *g_rpmsg_inst = RT_NULL;
static void *g_rpmsg_ept = RT_NULL;

static void wait_netif_up(struct eth_device *eth_dev)
{
    int timeout = 50;  /* 50 * 10ms = 500ms */

    while (timeout-- > 0)
    {
        if (eth_dev->netif && eth_dev->netif->flags & NETIF_FLAG_UP)
        {
            LOG_I("netif is UP");
            return;
        }
        rt_thread_mdelay(10);
    }

    LOG_W("netif UP wait timeout, continue anyway");
}

/* 等待 Master 上线并注册设备的线程入口 */
static void rpmsg_link_wait_thread_entry(void *parameter)
{
    rt_err_t ret;
    struct rpmsg_lite_instance *inst = (struct rpmsg_lite_instance *)parameter;

    LOG_I("Waiting for rpmsg link up...");

    /* 直接阻塞等待 link up（30 秒超时）*/
    if (rpmsg_lite_wait_for_link_up(inst, (uint32_t)RL_BLOCK) == 0)
    {
        LOG_E("Wait for link up timeout, master not ready");
        return;
    }

    LOG_I("rpmsg link is up, starting device...");

    /* 启动设备 */
    ret = rpmsg_net_start(inst);
    if (ret == RT_EOK)
    {
        LOG_I("rpmsg-net device registered successfully");

        /* 等待网络接口 UP */
        wait_netif_up(&g_rpmsg_net_dev->parent);

        /* 标记设备就绪 */
        g_rpmsg_net_initialized = RT_TRUE;

        LOG_I("rpmsg-net device ready for TX/RX");
    }
    else
    {
        rpmsg_lite_destroy_ept(inst, g_rpmsg_ept);
        g_rpmsg_ept = RT_NULL;
        LOG_E("Failed to register rpmsg-net device");
    }
}

/* 初始化等待线程 */
static rt_err_t rpmsg_link_wait_thread_init(struct rpmsg_lite_instance *inst)
{
    /* 创建等待线程 */
    rpmsg_link_wait_thread = rt_thread_create("rpmsg_wait",
                                               rpmsg_link_wait_thread_entry,
                                               inst,
                                               4 * 1024,      /* 栈大小 4KB */
                                               20,            /* 优先级 20 */
                                               10);           /* 时间片 10 */
    if (rpmsg_link_wait_thread == RT_NULL)
    {
        LOG_E("Failed to create rpmsg link wait thread");
        return -RT_ENOMEM;
    }

    /* 启动线程 */
    rt_thread_startup(rpmsg_link_wait_thread);

    LOG_I("rpmsg link wait thread created");
    return RT_EOK;
}

/* 清理等待线程 */
void rpmsg_link_wait_thread_deinit(void)
{
    if (rpmsg_link_wait_thread != RT_NULL)
    {
        rt_thread_delete(rpmsg_link_wait_thread);
        rpmsg_link_wait_thread = RT_NULL;
    }

    LOG_I("rpmsg link wait thread deinitialized");
}

rt_int32_t rt_rpmsg_net_init(void)
{
    uint32_t link_id = 0;
    uint32_t init_flags = 0;
    void *shmem_addr = (void *)RPMSG_LITE_SHMEM_ADDR;
    struct rpmsg_lite_instance *rpmsg_inst = RT_NULL;

    LOG_D("Initializing rpmsg-net driver");
    LOG_D("Shared memory address: %p", shmem_addr);

    rpmsg_inst = rpmsg_lite_remote_init(shmem_addr, link_id, init_flags);
    if (!rpmsg_inst)
    {
        LOG_E("Failed to initialize rpmsg-lite");
        return -RT_ERROR;
    }

    /* 保存全局实例指针 */
    g_rpmsg_inst = rpmsg_inst;

    /* 创建等待线程，阻塞等待 Master 上线 */
    if (rpmsg_link_wait_thread_init(rpmsg_inst) != RT_EOK)
    {
        LOG_E("Failed to init link wait thread");
        rpmsg_lite_deinit(rpmsg_inst);
        return -RT_ERROR;
    }

    return RT_EOK;
}

/* 修改为 INIT_COMPONENT_EXPORT，确保调度器已就绪 */
INIT_COMPONENT_EXPORT(rt_rpmsg_net_init);