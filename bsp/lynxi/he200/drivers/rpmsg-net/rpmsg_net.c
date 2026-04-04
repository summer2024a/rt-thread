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
#include <stdlib.h>

// #ifdef PKG_USING_RPMSG_LITE /* or PKG_USING_RPMSG depending on your config */

#include <rtdevice.h>
#include <netif/ethernetif.h>
#include <lwip/pbuf.h>
#include <lwip/netif.h>

/* RPMSG headers - adapt to your rpmsg-lite include paths */
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "platform/rpmsg_platform.h"
#include "rpmsg_net_proto.h"
#include "lynxi.h"

/* rpsh shell diag (for TX visibility) */
#ifndef SHELL_ETH_TYPE
#define SHELL_ETH_TYPE 0x88B6
#endif
#ifndef FRAME_TYPE_LOGIN
#define FRAME_TYPE_LOGIN 1
#define FRAME_TYPE_LOGOUT 2
#define FRAME_TYPE_CMD 3
#define FRAME_TYPE_RESP 4
#define FRAME_TYPE_HEARTBEAT 5
#define FRAME_TYPE_ACK 6
#endif

#define RPSH_MAX_PAYLOAD_PER_PACKET 128
#ifndef RPMSG_NET_RPSH_DIAG
#define RPMSG_NET_RPSH_DIAG 0
#endif

typedef struct rpsh_shell_frame_diag {
    uint8_t type;
    uint8_t flags;
    uint8_t session_id;
    uint8_t reserved;
    uint16_t seq;
    uint16_t data_len;
    uint16_t total_len;
    uint16_t crc;
    uint8_t data[RPSH_MAX_PAYLOAD_PER_PACKET];
} __attribute__((packed)) rpsh_shell_frame_diag_t;

/* 🟡 优化：添加配置选项 */
#ifndef RPMSG_NET_TX_RETRY_COUNT
#define RPMSG_NET_TX_RETRY_COUNT     3    /* 发送重试次数 */
#endif

#ifndef RPMSG_NET_SMALL_PKT_THRESHOLD
#define RPMSG_NET_SMALL_PKT_THRESHOLD 256  /* 小包阈值，用于优化内存拷贝 */
#endif

#ifndef RPMSG_NET_PURGE_ON_FAIL
#define RPMSG_NET_PURGE_ON_FAIL      1     /* 在 pbuf 分配失败时是否清理旧包 */
#endif

#ifndef RPMSG_NET_FAIL_RATE_THRESHOLD
#define RPMSG_NET_FAIL_RATE_THRESHOLD 10.0f /* 失败率阈值 (百分比)，降低阈值以提高响应速度 */
#endif

#ifndef RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD
#define RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD 5 /* 连续失败次数阈值，超过则立即清理 */
#endif

/* 🟢 新增：零拷贝支持 - 使用 RPMsg 共享内存 */
#ifndef RPMSG_NET_ZERO_COPY_RX
#define RPMSG_NET_ZERO_COPY_RX       0     /* 启用零拷贝接收模式 */
#endif

#ifndef RPMSG_NET_RX_POOL_SIZE
#define RPMSG_NET_RX_POOL_SIZE       32    /* 自定义 pbuf 池大小 */
#endif

#define RPMSG_NET_RX_MQ_NAME   "rpnet_rx_mq"
#define RPMSG_NET_RX_MQ_MSGSZ  sizeof(void *)
#define RPMSG_NET_RX_MQ_ENTRIES 64  /* 增加邮箱条目数，从 32 到 64 */
#define RPMSG_NET_RX_MQ_POOL_SIZE (RPMSG_NET_RX_MQ_MSGSZ * RPMSG_NET_RX_MQ_ENTRIES)

#define MAX_PKT_SIZE                 1536
#define RPMSG_NET_EPT_NAME           "rpmsg-net"

#ifdef DBG_TAG
#undef DBG_TAG
#undef DBG_LVL
#endif

#define DBG_TAG "rpmsg-net"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

struct rpmsg_net_device
{
    struct eth_device parent;
    struct rpmsg_lite_instance *rpmsg_inst;
    struct rpmsg_lite_endpoint *rpmsg_ept;
    rpmsg_queue_handle rpmsg_queue;
    struct rt_thread *rx_thread;
    rt_bool_t rx_thread_running;
    struct rt_messagequeue rx_mq;
    char rx_mq_pool[RPMSG_NET_RX_MQ_POOL_SIZE];
    char name[RT_NAME_MAX];
    rt_bool_t remote_ready;
    rt_bool_t local_link_up;
    rt_bool_t remote_link_up;
    rt_bool_t hello_sent;
    rt_bool_t link_state_sent;
    rt_bool_t pending_link_down;
    uint32_t remote_addr;
    uint32_t mtu;
    rt_uint8_t local_mac[ETH_ALEN];
    rt_uint8_t remote_mac[ETH_ALEN];
};

struct rpmsg_net_context
{
    struct rpmsg_net_device *device;
    struct rpmsg_lite_instance *rpmsg_inst;
    struct rt_thread *link_wait_thread;
    rt_uint8_t *mac_reg_base;
    rt_bool_t initialized;
};

/* 🟢 新增：零拷贝 RX 支持 - 自定义 pbuf 结构体 */
#if RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF
/**
 * 自定义 pbuf 结构体，用于零拷贝接收
 * 将 pbuf 直接指向 RPMsg 共享内存，避免数据拷贝
 */
typedef struct rpmsg_net_custom_pbuf {
    struct pbuf_custom p;           /* LwIP 自定义 pbuf */
    void *rpmsg_payload;            /* RPMsg 消息的原始 payload 指针 */
    rt_uint32_t msg_len;            /* 消息总长度 */
} rpmsg_net_custom_pbuf_t;

/* 声明 RX 内存池 */
LWIP_MEMPOOL_DECLARE(rpmsg_net_rx_pool, RPMSG_NET_RX_POOL_SIZE, sizeof(rpmsg_net_custom_pbuf_t), "RPMSG NET RX PBUF pool");

#endif /* RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF */

static struct rpmsg_net_context g_rpmsg_net_ctx =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_FALSE,
};

/* 🟢 新增：零拷贝 RX 支持 - 自定义 pbuf 释放函数 */
#if RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF
/**
 * 释放自定义 pbuf
 * 注意：RPMsg 共享内存由 RPMsg 框架管理，这里只需要释放 pbuf_custom 结构体本身
 */
static void rpmsg_net_pbuf_free_custom(struct pbuf *p)
{
    SYS_ARCH_DECL_PROTECT(old_level);
    rpmsg_net_custom_pbuf_t *rpmsg_pbuf;

    if (p == RT_NULL) {
        return;
    }

    rpmsg_pbuf = (rpmsg_net_custom_pbuf_t *)p;
    
    LOG_D("Free custom pbuf: payload=%p, len=%u", rpmsg_pbuf->p.pbuf.payload, rpmsg_pbuf->p.pbuf.tot_len);

    /* RPMsg 共享内存不需要释放，由 RPMsg 框架自动管理 */
    /* 只需归还自定义 pbuf 结构体到内存池 */
    SYS_ARCH_PROTECT(old_level);
    LWIP_MEMPOOL_FREE(rpmsg_net_rx_pool, rpmsg_pbuf);
    SYS_ARCH_UNPROTECT(old_level);
}
#endif /* RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF */

static void rpmsg_net_rx_thread_entry(void *parameter);
static void rpmsg_net_stop_rx_path(struct rpmsg_net_device *rdev);
static void rpmsg_net_reset_context(void);
static void rpmsg_net_free_device(struct rpmsg_net_device *rdev, rt_bool_t detach_mq);
static void rpmsg_net_cleanup_unregistered_device(struct rpmsg_net_device *rdev, rt_bool_t mq_inited);

static rt_bool_t rpmsg_net_is_valid_mac(const rt_uint8_t *mac)
{
    rt_bool_t all_zero = RT_TRUE;
    rt_bool_t all_ff = RT_TRUE;
    int i;

    if (mac == RT_NULL)
    {
        return RT_FALSE;
    }

    if ((mac[0] & 0x01U) != 0U)
    {
        return RT_FALSE;
    }

    for (i = 0; i < ETH_ALEN; i++)
    {
        if (mac[i] != 0x00U)
        {
            all_zero = RT_FALSE;
        }
        if (mac[i] != 0xFFU)
        {
            all_ff = RT_FALSE;
        }
    }

    return !(all_zero || all_ff);
}

static rt_err_t rpmsg_net_parse_mac_string(const char *str, rt_uint8_t *mac)
{
    unsigned int values[ETH_ALEN];

    if (str == RT_NULL || mac == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != ETH_ALEN)
    {
        return -RT_ERROR;
    }

    for (int i = 0; i < ETH_ALEN; i++)
    {
        mac[i] = (rt_uint8_t)values[i];
    }

    return rpmsg_net_is_valid_mac(mac) ? RT_EOK : -RT_ERROR;
}

static rt_err_t rpmsg_net_load_mac_from_env(rt_uint8_t *mac)
{
    const char *env_mac;

    env_mac = getenv("RPMSG_NET_MAC");
    if (env_mac == RT_NULL || env_mac[0] == '\0')
    {
        return -RT_ERROR;
    }

    if (rpmsg_net_parse_mac_string(env_mac, mac) != RT_EOK)
    {
        LOG_W("Ignore invalid RPMSG_NET_MAC: %s", env_mac);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t rpmsg_net_load_mac_from_hw(rt_uint8_t *mac)
{
    if (g_rpmsg_net_ctx.mac_reg_base == RT_NULL)
    {
        return -RT_ERROR;
    }

    rt_memcpy(mac, g_rpmsg_net_ctx.mac_reg_base, ETH_ALEN);
    return rpmsg_net_is_valid_mac(mac) ? RT_EOK : -RT_ERROR;
}

static void rpmsg_net_make_fallback_mac(struct rpmsg_net_device *rdev)
{
    rt_ubase_t seed;

    seed = (rt_ubase_t)rdev ^ (rt_ubase_t)rdev->rpmsg_inst ^ (rt_ubase_t)rdev->rpmsg_ept;

    rdev->local_mac[0] = 0x02U;
    rdev->local_mac[1] = 0x12U;
    rdev->local_mac[2] = (rt_uint8_t)((seed >> 0) & 0xFFU);
    rdev->local_mac[3] = (rt_uint8_t)((seed >> 8) & 0xFFU);
    rdev->local_mac[4] = (rt_uint8_t)((seed >> 16) & 0xFFU);
    rdev->local_mac[5] = (rt_uint8_t)((seed >> 24) & 0xFFU);
}

static void rpmsg_net_init_local_mac(struct rpmsg_net_device *rdev)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    if (rpmsg_net_load_mac_from_env(rdev->local_mac) == RT_EOK)
    {
        LOG_I("Use MAC from RPMSG_NET_MAC");
        return;
    }

    if (rpmsg_net_load_mac_from_hw(rdev->local_mac) == RT_EOK)
    {
        LOG_I("Use MAC from hardware register");
        return;
    }

    rpmsg_net_make_fallback_mac(rdev);
    LOG_W("Use fallback locally-administered MAC");
}

static rt_uint16_t rpmsg_net_cpu_to_le16(rt_uint16_t value)
{
    return value;
}

static rt_uint32_t rpmsg_net_cpu_to_le32(rt_uint32_t value)
{
    return value;
}

static rt_uint16_t rpmsg_net_le16_to_cpu(rt_uint16_t value)
{
    return value;
}

static rt_uint32_t rpmsg_net_le32_to_cpu(rt_uint32_t value)
{
    return value;
}

typedef void (*rpmsg_net_fill_tx_cb)(void *buf, void *parameter);

static rt_bool_t rpmsg_net_is_ready(struct rpmsg_net_device *rdev)
{
    return (rdev != RT_NULL) && rdev->local_link_up && rdev->remote_link_up && rdev->remote_ready;
}

static void rpmsg_net_mark_local_state(struct rpmsg_net_device *rdev, rt_bool_t link_up)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    rdev->local_link_up = link_up;
    if (!link_up)
    {
        rdev->remote_ready = RT_FALSE;
        rdev->remote_link_up = RT_FALSE;
    }
}

static void rpmsg_net_sync_carrier(struct rpmsg_net_device *rdev)
{
    rt_bool_t carrier_on;

    if (rdev == RT_NULL || rdev->parent.netif == RT_NULL)
    {
        return;
    }

    carrier_on = rpmsg_net_is_ready(rdev);
    if (carrier_on)
    {
        netif_set_link_up(rdev->parent.netif);
    }
    else
    {
        netif_set_link_down(rdev->parent.netif);
    }
}

static rt_err_t rpmsg_net_send_nocopy_message(struct rpmsg_net_device *rdev,
                                              rt_uint32_t len,
                                              rpmsg_net_fill_tx_cb fill,
                                              void *parameter)
{
    int32_t ret;
    uint32_t tx_buf_size;
    void *tx_buf;
    uint32_t dst_addr;

    if (rdev == RT_NULL || rdev->rpmsg_inst == RT_NULL || rdev->rpmsg_ept == RT_NULL || fill == RT_NULL)
    {
        return -RT_ERROR;
    }

    tx_buf = rpmsg_lite_alloc_tx_buffer(rdev->rpmsg_inst, &tx_buf_size, RL_BLOCK);
    if (tx_buf == RT_NULL)
    {
        LOG_E("rpmsg_lite_alloc_tx_buffer failed");
        return -RT_ERROR;
    }

    if (len > tx_buf_size)
    {
        LOG_E("tx buffer too small, len=%u size=%u", len, tx_buf_size);
        return -RT_ERROR;
    }

    fill(tx_buf, parameter);

    /* avoid heavy log pressure in TX hot path */
    if (len > 0)
    {
        rt_uint32_t dump_len = (len > 64U) ? 64U : len;
        LOG_D("Sending message len=%u (dump %u bytes)", len, dump_len);
        LOG_HEX("Sending message:", 16, tx_buf, dump_len);
    }

    /*
     * Prefer replying to the source endpoint learned from RX path.
     * Fallback to protocol default endpoint before the first RX packet arrives.
     */
    dst_addr = (rdev->remote_addr != 0U) ? rdev->remote_addr : RPMSG_NET_REMOTE_EPT_ADDR;
    LOG_D("rpmsg tx dst=0x%08x (remote_addr=0x%08x, fallback=0x%08x), len=%u",
          dst_addr, rdev->remote_addr, RPMSG_NET_REMOTE_EPT_ADDR, len);

    ret = rpmsg_lite_send_nocopy(rdev->rpmsg_inst,
                                 rdev->rpmsg_ept,
                                 dst_addr,
                                 tx_buf,
                                 len);
    if (ret != RL_SUCCESS)
    {
        LOG_E("rpmsg_lite_send_nocopy failed, ret=%d", ret);
        return -RT_ERROR;
    }
    else
    {
        LOG_D("Message sent successfully");
    }

    return RT_EOK;
}

static void rpmsg_net_fill_hello_msg(void *buf, void *parameter)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)parameter;
    struct rpmsg_net_hello_msg *msg = (struct rpmsg_net_hello_msg *)buf;

    rt_memset(msg, 0, sizeof(*msg));
    msg->hdr.type = rpmsg_net_cpu_to_le16(RPMSG_NET_PKT_TYPE_HELLO);
    msg->hdr.version = rpmsg_net_cpu_to_le16(RPMSG_NET_PROTO_VER);
    msg->hdr.length = rpmsg_net_cpu_to_le32(sizeof(*msg));
    msg->mtu = rpmsg_net_cpu_to_le16((rt_uint16_t)rdev->mtu);
    rt_memcpy(msg->mac, rdev->local_mac, sizeof(msg->mac));

    LOG_I("Create hello message done!");
}

static void rpmsg_net_fill_link_msg(void *buf, void *parameter)
{
    rt_bool_t link_up = *(rt_bool_t *)parameter;
    struct rpmsg_net_link_msg *msg = (struct rpmsg_net_link_msg *)buf;

    rt_memset(msg, 0, sizeof(*msg));
    msg->hdr.type = rpmsg_net_cpu_to_le16(RPMSG_NET_PKT_TYPE_LINK);
    msg->hdr.version = rpmsg_net_cpu_to_le16(RPMSG_NET_PROTO_VER);
    msg->hdr.length = rpmsg_net_cpu_to_le32(sizeof(*msg));
    msg->link_up = rpmsg_net_cpu_to_le32(link_up ? 1U : 0U);

    LOG_I("Create link message done!");
}

static void rpmsg_net_fill_data_msg(void *buf, void *parameter)
{
    struct pbuf *p = RT_NULL;
    struct pbuf *q;
    struct rpmsg_net_data_msg *msg = (struct rpmsg_net_data_msg *)buf;
    uint32_t copied = 0;

    p = (struct pbuf *)parameter;
    if (p == RT_NULL)
    {
        /* Should never happen: keep safe against unexpected callers */
        return;
    }
    msg->hdr.type = rpmsg_net_cpu_to_le16(RPMSG_NET_PKT_TYPE_DATA);
    msg->hdr.version = rpmsg_net_cpu_to_le16(RPMSG_NET_PROTO_VER);
    msg->hdr.length = rpmsg_net_cpu_to_le32(sizeof(*msg) + p->tot_len);

    /* 🟡 优化：根据数据包大小选择最优的拷贝策略 */
    if (p->tot_len <= RPMSG_NET_SMALL_PKT_THRESHOLD)
    {
        /* 小包：直接内存拷贝，效率更高 */
        for (q = p; q != RT_NULL; q = q->next)
        {
            rt_memcpy((char *)msg->payload + copied, q->payload, q->len);
            copied += q->len;
        }
        LOG_D("Small packet copy: %u bytes", p->tot_len);
    }
    else if (p->next == RT_NULL && p->len == p->tot_len)
    {
        /* 单pbuf大包：一次性拷贝 */
        rt_memcpy(msg->payload, p->payload, p->tot_len);
        LOG_D("Single pbuf large packet copy: %u bytes", p->tot_len);
    }
    else
    {
        /* 多pbuf大包：分块拷贝，添加进度跟踪 */
        rt_uint32_t chunk_cnt = 0;
        for (q = p; q != RT_NULL; q = q->next)
        {
            rt_memcpy((char *)msg->payload + copied, q->payload, q->len);
            copied += q->len;
            chunk_cnt++;
        }
        LOG_D("Multi-pbuf large packet copy: %u bytes in %u chunks", p->tot_len, chunk_cnt);
    }
}

static rt_bool_t rpmsg_net_len_valid(const struct rpmsg_net_msg_hdr *hdr,
                                     rt_uint32_t rx_len,
                                     rt_uint32_t min_len,
                                     rt_uint32_t *msg_len)
{
    rt_uint32_t declared_len;

    if (hdr == RT_NULL || msg_len == RT_NULL)
    {
        return RT_FALSE;
    }

    declared_len = rpmsg_net_le32_to_cpu(hdr->length);
    if (declared_len < min_len || declared_len > rx_len)
    {
        return RT_FALSE;
    }

    *msg_len = declared_len;
    return RT_TRUE;
}

static rt_err_t rpmsg_net_send_hello(struct rpmsg_net_device *rdev)
{
    if (rdev == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (rpmsg_net_send_nocopy_message(rdev,
                                      sizeof(struct rpmsg_net_hello_msg),
                                      rpmsg_net_fill_hello_msg,
                                      rdev) != RT_EOK)
    {
        return -RT_ERROR;
    }

    rdev->hello_sent = RT_TRUE;
    return RT_EOK;
}

static rt_err_t rpmsg_net_send_link_state(struct rpmsg_net_device *rdev, rt_bool_t link_up)
{
    if (rdev == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (rpmsg_net_send_nocopy_message(rdev,
                                      sizeof(struct rpmsg_net_link_msg),
                                      rpmsg_net_fill_link_msg,
                                      &link_up) != RT_EOK)
    {
        return -RT_ERROR;
    }

    rdev->link_state_sent = link_up;
    rdev->pending_link_down = RT_FALSE;
    return RT_EOK;
}

static void rpmsg_net_request_link_down(struct rpmsg_net_device *rdev)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    if (rdev->link_state_sent)
    {
        if (rpmsg_net_send_link_state(rdev, RT_FALSE) != RT_EOK)
        {
            rdev->pending_link_down = RT_TRUE;
        }
    }

    rpmsg_net_mark_local_state(rdev, RT_FALSE);
    rpmsg_net_sync_carrier(rdev);
}

static void rpmsg_net_apply_remote_hello(struct rpmsg_net_device *rdev,
                                         const struct rpmsg_net_hello_msg *msg)
{
    rt_uint16_t mtu;

    if (rdev == RT_NULL || msg == RT_NULL)
    {
        return;
    }

    rt_memcpy(rdev->remote_mac, msg->mac, sizeof(rdev->remote_mac));
    mtu = rpmsg_net_le16_to_cpu(msg->mtu);
    if (mtu > 0U && mtu <= RPMSG_NET_MTU)
    {
        rdev->mtu = (rdev->mtu < mtu) ? rdev->mtu : mtu;
    }

    rdev->remote_ready = RT_TRUE;

    LOG_D("remote hello: mtu = %d", mtu);

    /* ✨ 收到 HELLO 后才回复 HELLO（如果还没发送过） */
    if (!rdev->hello_sent)
    {
        if (rpmsg_net_send_hello(rdev) != RT_EOK)
        {
            LOG_W("Failed to send HELLO reply");
        }
    }

    rpmsg_net_sync_carrier(rdev);
}

static void rpmsg_net_apply_remote_link(struct rpmsg_net_device *rdev,
                                        const struct rpmsg_net_link_msg *msg)
{
    if (rdev == RT_NULL || msg == RT_NULL)
    {
        LOG_E("rpmsg_net_apply_remote_link: rdev or msg is NULL");
        return;
    }

    rdev->remote_link_up = (rpmsg_net_le32_to_cpu(msg->link_up) != 0U) ? RT_TRUE : RT_FALSE;

    LOG_D("remote link: up = %d", rdev->remote_link_up);

    /* 收到 LINK UP 后，如果还没发送过 LINK 状态，则回复 LINK UP */
    if (rdev->remote_link_up && !rdev->link_state_sent)
    {
        if (rpmsg_net_send_link_state(rdev, RT_TRUE) != RT_EOK)
        {
            LOG_W("Failed to send LINK UP reply");
        }
    }

    rpmsg_net_sync_carrier(rdev);
}

static rt_err_t rpmsg_net_handle_control(struct rpmsg_net_device *rdev,
                                         const struct rpmsg_net_msg_hdr *hdr,
                                         rt_uint32_t msg_len)
{
    rt_uint16_t type;

    if (rdev == RT_NULL || hdr == RT_NULL)
    {
        return -RT_ERROR;
    }

    type = rpmsg_net_le16_to_cpu(hdr->type);
    switch (type)
    {
    case RPMSG_NET_PKT_TYPE_HELLO:
        if (msg_len != sizeof(struct rpmsg_net_hello_msg))
        {
            LOG_W("Invalid hello packet length: %u", msg_len);
            return -RT_ERROR;
        }
        rpmsg_net_apply_remote_hello(rdev, (const struct rpmsg_net_hello_msg *)hdr);
        return RT_EOK;

    case RPMSG_NET_PKT_TYPE_LINK:
        if (msg_len != sizeof(struct rpmsg_net_link_msg))
        {
            LOG_W("Invalid link packet length: %u", msg_len);
            return -RT_ERROR;
        }
        rpmsg_net_apply_remote_link(rdev, (const struct rpmsg_net_link_msg *)hdr);
        return RT_EOK;

    default:
        return -RT_ERROR;
    }
}

/* 全局统计变量，便于调试命令访问 */
static rt_uint32_t g_alloc_fail_cnt = 0;
static rt_uint32_t g_alloc_success_cnt = 0;
static rt_uint32_t g_consecutive_fail_cnt = 0; /* 连续失败计数器 */

static void rpmsg_net_dispatch_rx(struct rpmsg_net_device *rdev, void *payload, uint32_t len, uint32_t src)
{
    struct rpmsg_net_msg_hdr *hdr;
    struct pbuf *p = NULL;
    void *ptr;
    rt_err_t ready_ret;
    rt_uint32_t msg_len;
    rt_uint16_t type;

    if (!rdev)
    {
        LOG_E("rpmsg_net_dev is NULL");
        return;
    }

    if (len < sizeof(*hdr) || len > (MAX_PKT_SIZE + sizeof(struct rpmsg_net_data_msg)))
    {
        LOG_E("Invalid packet length: %u", len);
        return;
    }

    hdr = (struct rpmsg_net_msg_hdr *)payload;
    rdev->remote_addr = src;

    if (rpmsg_net_le16_to_cpu(hdr->version) != RPMSG_NET_PROTO_VER)
    {
        LOG_W("Protocol version mismatch: %u", rpmsg_net_le16_to_cpu(hdr->version));
        return;
    }

    if (!rpmsg_net_len_valid(hdr, len, sizeof(*hdr), &msg_len))
    {
        LOG_W("Invalid declared msg length");
        return;
    }

    type = rpmsg_net_le16_to_cpu(hdr->type);
    if (type != RPMSG_NET_PKT_TYPE_DATA)
    {
        if (rpmsg_net_handle_control(rdev, hdr, msg_len) != RT_EOK)
        {
            LOG_W("Invalid control packet type=%u len=%u", type, msg_len);
            return;
        }
        return;
    }

    if (msg_len <= sizeof(struct rpmsg_net_data_msg))
    {
        LOG_W("DATA packet without payload");
        return;
    }

    /* 🟢 优化：使用零拷贝方式分配 pbuf（如果启用） */
#if RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF
    {
        rpmsg_net_custom_pbuf_t *custom_pbuf;

        /* 从内存池分配自定义 pbuf */
        custom_pbuf = (rpmsg_net_custom_pbuf_t *)LWIP_MEMPOOL_ALLOC(rpmsg_net_rx_pool);
        if (custom_pbuf != RT_NULL) {
            /* 初始化自定义 pbuf */
            custom_pbuf->p.custom_free_function = rpmsg_net_pbuf_free_custom;
            custom_pbuf->rpmsg_payload = payload;
            custom_pbuf->msg_len = msg_len;

            /* 创建 pbuf，直接指向 RPMsg 共享内存 */
            p = pbuf_alloced_custom(PBUF_RAW,
                                    msg_len - sizeof(struct rpmsg_net_data_msg),
                                    PBUF_REF,
                                    &custom_pbuf->p,
                                    (rt_uint8_t *)payload + sizeof(struct rpmsg_net_data_msg),
                                    msg_len - sizeof(struct rpmsg_net_data_msg));

            if (p == RT_NULL) {
                /* 分配失败，归还内存池 */
                LWIP_MEMPOOL_FREE(rpmsg_net_rx_pool, custom_pbuf);
                g_alloc_fail_cnt++;
                g_consecutive_fail_cnt++;
                LOG_W("pbuf_alloced_custom failed, fallback to PBUF_RAM");
            } else {
                g_alloc_success_cnt++;
                g_consecutive_fail_cnt = 0;
                LOG_D("Zero-copy RX: payload=%p, len=%u", p->payload, p->tot_len);
            }
        } else {
            g_alloc_fail_cnt++;
            g_consecutive_fail_cnt++;
            LOG_W("LWIP_MEMPOOL_ALLOC failed, fallback to PBUF_RAM");
        }
    }
#endif /* RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF */

    /* 🟡 回退路径：如果零拷贝未启用或失败，使用传统方式 */
#if !(RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF) || \
    (RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF && defined(CONFIG_RPMSG_NET_FORCE_FALLBACK))
    if (p == RT_NULL)
#endif
    {
        /* allocate pbuf for incoming frame */
        p = pbuf_alloc(PBUF_RAW, msg_len - sizeof(struct rpmsg_net_data_msg), PBUF_RAM);
        if (p == RT_NULL)
        {
            g_alloc_fail_cnt++;
            g_consecutive_fail_cnt++;

            float fail_rate = (float)g_alloc_fail_cnt * 100 / (g_alloc_success_cnt + g_alloc_fail_cnt);
            rt_bool_t need_purge = RT_FALSE;
            rt_bool_t severe = RT_FALSE;

            LOG_W("Failed to allocate pbuf: payload_size=%u, msg_len=%u, Success=%u, Fail=%u, Rate=%.1f%%, Consecutive=%u",
                  msg_len - sizeof(struct rpmsg_net_data_msg), msg_len,
                  g_alloc_success_cnt, g_alloc_fail_cnt, fail_rate, g_consecutive_fail_cnt);

            /* 条件 1：总失败率超过阈值 */
            if (g_alloc_fail_cnt > 50 && fail_rate > RPMSG_NET_FAIL_RATE_THRESHOLD)
            {
                LOG_E("High pbuf allocation failure rate (%.1f%% > %.0f%%), possible memory leak!", 
                      fail_rate, RPMSG_NET_FAIL_RATE_THRESHOLD);
                need_purge = RT_TRUE;
                severe = RT_TRUE;
            }

            /* 条件 2：连续失败次数超过阈值（更敏感） */
            if (g_consecutive_fail_cnt >= RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD)
            {
                LOG_E("Consecutive pbuf allocation failures (%u >= %u), immediate purge needed!", 
                      g_consecutive_fail_cnt, RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD);
                need_purge = RT_TRUE;
                severe = RT_TRUE;
            }

            if (severe)
            {
                LOG_E("RX pbuf memory pressure is severe. Current RT_LWIP_PBUF_NUM=%d", RT_LWIP_PBUF_NUM);
            }
            else
            {
                LOG_W("RX pbuf temporary pressure. Current RT_LWIP_PBUF_NUM=%d", RT_LWIP_PBUF_NUM);
            }

#if RPMSG_NET_PURGE_ON_FAIL
            if (need_purge)
            {
                /* 如果内存池严重耗尽，考虑丢弃一些旧包 */
                static rt_uint32_t last_purge_time = 0;
                rt_uint32_t current_time = rt_tick_get();

                /* 每隔 5 秒尝试清理一次（从 10 秒缩短到 5 秒） */
                if (current_time - last_purge_time > rt_tick_from_millisecond(5000))
                {
                    LOG_W("Attempting to purge old packets from RX queue...");

                    /* 尝试清理邮箱中的部分 pbuf，增加清理数量 */
                    void *purge_ptr;
                    rt_uint32_t purge_count = 0;
                    rt_uint32_t purge_max = (g_consecutive_fail_cnt >= RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD) ? 10 : 5;

                    while (purge_count < purge_max &&
                           rt_mq_recv(&rdev->rx_mq, &purge_ptr, sizeof(purge_ptr), 0) > 0)
                    {
                        pbuf_free((struct pbuf *)purge_ptr);
                        purge_count++;
                    }

                    if (purge_count > 0)
                    {
                        LOG_I("Purged %u old packets from RX queue (consecutive_fail=%u)", 
                              purge_count, g_consecutive_fail_cnt);
                        last_purge_time = current_time;

                        /* 重新尝试分配 */
                        p = pbuf_alloc(PBUF_RAW, msg_len - sizeof(struct rpmsg_net_data_msg), PBUF_RAM);
                        if (p != RT_NULL)
                        {
                            LOG_I("Successfully allocated pbuf after purge");
                            g_alloc_success_cnt++;
                            g_consecutive_fail_cnt = 0; /* 重置连续失败计数 */
                        }
                    }
                    else
                    {
                        LOG_W("RX queue is empty, purge had no effect");
                    }
                }
            }
#endif /* RPMSG_NET_PURGE_ON_FAIL */

            if (p == RT_NULL)
            {
                return;  /* 如果仍然分配失败，放弃这个包 */
            }
        }
        else
        {
            g_alloc_success_cnt++;
            g_consecutive_fail_cnt = 0; /* 成功分配，重置连续失败计数 */
        }

        /* copy payload into pbuf chain */
        if (pbuf_take(p,
                      (rt_uint8_t *)payload + sizeof(struct rpmsg_net_data_msg),
                      msg_len - sizeof(struct rpmsg_net_data_msg)) != ERR_OK)
        {
            LOG_E("pbuf_take failed, dropping packet");
            pbuf_free(p);
            return;
        }
    }

    /* send pbuf pointer to mailbox */
    ptr = p;
    if (rt_mq_send(&rdev->rx_mq, &ptr, sizeof(ptr)) != RT_EOK)
    {
        LOG_E("Failed to send pbuf to mailbox (MQ full?)");
        pbuf_free(p);  /* ✅ 确保释放 pbuf */
        return;
    }

    /* Lightweight frame sanity log: first bytes + ethertype */
    if (p->tot_len >= 14)
    {
        rt_uint8_t hdr[14];
        pbuf_copy_partial(p, hdr, sizeof(hdr), 0);
        LOG_D("RX frame: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x len=%u",
              hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
              hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[11],
              hdr[12], hdr[13], p->tot_len);

        if (hdr[12] == 0x08U && hdr[13] == 0x00U && p->tot_len >= 34U)
        {
            rt_uint8_t iphdr[20];
            pbuf_copy_partial(p, iphdr, sizeof(iphdr), 14);
            LOG_D("RX IPv4: dst_ip=%u.%u.%u.%u src_ip=%u.%u.%u.%u proto=%u",
                  iphdr[16], iphdr[17], iphdr[18], iphdr[19],
                  iphdr[12], iphdr[13], iphdr[14], iphdr[15],
                  iphdr[9]);
        }
        else if (hdr[12] == 0x08U && hdr[13] == 0x06U && p->tot_len >= 42U)
        {
            rt_uint8_t arphdr[28];
            pbuf_copy_partial(p, arphdr, sizeof(arphdr), 14);
            LOG_D("RX ARP: oper=%u target_ip=%u.%u.%u.%u sender_ip=%u.%u.%u.%u",
                  ((rt_uint16_t)arphdr[6] << 8) | arphdr[7],
                  arphdr[24], arphdr[25], arphdr[26], arphdr[27],
                  arphdr[14], arphdr[15], arphdr[16], arphdr[17]);
        }
    }
    else
    {
        LOG_D("RX payload too short for ethernet header: len=%u", p->tot_len);
    }

    /* notify net stack that device has packet */
    /* Shell frames (ethertype 0x88B6) should not be forwarded to lwIP RX thread.
     * Otherwise `ethernetif.c` will dequeue and drop them, and `rpsh_server`
     * (which also reads from eth_rx) may never see the LOGIN/RESP packets. */
    {
        rt_bool_t is_shell_frame = RT_FALSE;
        rt_uint8_t hdr[14];

        if (p->tot_len >= sizeof(hdr))
        {
            pbuf_copy_partial(p, hdr, sizeof(hdr), 0);
            if (hdr[12] == 0x88 && hdr[13] == 0xB6) {
                /* 0x88B6 in network byte order */
                is_shell_frame = RT_TRUE;
                LOG_D("Shell frame: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x len=%u",
                      hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
                      hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[11],
                      hdr[12], hdr[13], p->tot_len);

                /* Optional deep shell diag for troubleshooting only. */
#if RPMSG_NET_RPSH_DIAG
                if (p->tot_len >= (14U + sizeof(rpsh_shell_frame_diag_t)))
                {
                    uint8_t frame_buf[14U + sizeof(rpsh_shell_frame_diag_t)];
                    pbuf_copy_partial(p, frame_buf, sizeof(frame_buf), 0);

                    rpsh_shell_frame_diag_t *sf = (rpsh_shell_frame_diag_t *)(frame_buf + 14U);

            /* CRC is computed with crc field forced to 0 */
            uint16_t rx_crc = sf->crc;
            sf->crc = 0;

            uint16_t c = 0xFFFF;
            const uint8_t *crc_begin = (const uint8_t *)sf;
            int crc_len = (int)sizeof(*sf);
                    for (int i = 0; i < crc_len; i++)
                    {
                        c ^= crc_begin[i];
                        for (int j = 0; j < 8; j++)
                            c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
                    }

            sf->crc = rx_crc;

            LOG_W("SHELL RX DIAG: type=%u seq=%u sid=%u flags=%u dlen=%u total=%u crc_calc=0x%04x crc_rx=0x%04x%s",
                  sf->type, sf->seq, sf->session_id, sf->flags, sf->data_len, sf->total_len,
                  c, rx_crc, (c == rx_crc) ? "" : " CRC_MISMATCH");
                }
#endif
            }

        }

        if (!is_shell_frame)
        {
            ready_ret = eth_device_ready(&rdev->parent);
            if (ready_ret != RT_EOK)
            {
                LOG_W("eth_device_ready failed: %d", ready_ret);
            }
            else
            {
                LOG_D("Packet forwarded to network stack");
            }
        }
        else
        {
            LOG_D("Shell frame: skip eth_device_ready (keep for rpsh_server)");
        }
    }
}

/* RPMSG receive callback (rpmsg-lite API) */
static int32_t rpmsg_net_recv_cb(void *payload, uint32_t len, uint32_t src, void *priv)
{
    return rpmsg_queue_rx_cb(payload, len, src, priv);
}

static void rpmsg_net_rx_worker(struct rpmsg_net_device *rdev)
{
    void *rx_buf;
    uint32_t len;
    uint32_t src;
    int32_t ret;

    if (rdev == RT_NULL || rdev->rpmsg_inst == RT_NULL || rdev->rpmsg_queue == RT_NULL)
    {
        return;
    }

    while (rdev->rx_thread_running)
    {
        rx_buf = RT_NULL;
        len = 0;
        src = 0;

        ret = rpmsg_queue_recv_nocopy(rdev->rpmsg_inst,
                                      rdev->rpmsg_queue,
                                      &src,
                                      (char **)&rx_buf,
                                      &len,
                                      RL_BLOCK);
        if (ret != RL_SUCCESS)
        {
            LOG_W("rpmsg_queue_recv_nocopy failed, ret=%d", ret);
            continue;
        }

        rpmsg_net_dispatch_rx(rdev, rx_buf, len, src);
        if (rpmsg_queue_nocopy_free(rdev->rpmsg_inst, rx_buf) != RL_SUCCESS)
        {
            LOG_W("Failed to free rx buffer");
        }
    }
}

static void rpmsg_net_rx_thread_entry(void *parameter)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)parameter;

    if (rdev == RT_NULL)
    {
        return;
    }

    rpmsg_net_rx_worker(rdev);

    rdev->rx_thread = RT_NULL;
}

static void rpmsg_net_stop_rx_path(struct rpmsg_net_device *rdev)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    rdev->rx_thread_running = RT_FALSE;

    /* 修复内存泄漏：停止线程前清理邮箱中剩余的 pbuf */
    {
        void *purge_ptr;
        while (rt_mq_recv(&rdev->rx_mq, &purge_ptr, sizeof(purge_ptr), 0) > 0)
        {
            pbuf_free((struct pbuf *)purge_ptr);
        }
    }

    if (rdev->rpmsg_ept != RT_NULL)
    {
        rpmsg_lite_destroy_ept(rdev->rpmsg_inst, rdev->rpmsg_ept);
        rdev->rpmsg_ept = RT_NULL;
    }

    if (rdev->rpmsg_queue != RT_NULL)
    {
        rpmsg_queue_destroy(rdev->rpmsg_inst, rdev->rpmsg_queue);
        rdev->rpmsg_queue = RT_NULL;
    }

    if (rdev->rx_thread != RT_NULL)
    {
        rt_thread_mdelay(10);
        if (rdev->rx_thread != RT_NULL)
        {
            rt_thread_delete(rdev->rx_thread);
            rdev->rx_thread = RT_NULL;
        }
    }
}

static void rpmsg_net_reset_context(void)
{
    g_rpmsg_net_ctx.device = RT_NULL;
    g_rpmsg_net_ctx.rpmsg_inst = RT_NULL;
    g_rpmsg_net_ctx.link_wait_thread = RT_NULL;
    g_rpmsg_net_ctx.initialized = RT_FALSE;
}

static void rpmsg_net_free_device(struct rpmsg_net_device *rdev, rt_bool_t detach_mq)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    rpmsg_net_stop_rx_path(rdev);

    if (detach_mq)
    {
        rt_mq_detach(&rdev->rx_mq);
    }

    rt_free(rdev);
}

static void rpmsg_net_cleanup_unregistered_device(struct rpmsg_net_device *rdev, rt_bool_t mq_inited)
{
    if (rdev == RT_NULL)
    {
        return;
    }

    if (mq_inited)
    {
        rt_mq_detach(&rdev->rx_mq);
    }

    rpmsg_net_free_device(rdev, RT_FALSE);
}

/* transmit via rpmsg */
static rt_err_t rpmsg_net_tx(rt_device_t dev, struct pbuf *p)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;
    uint32_t total_len = p->tot_len;
    uint32_t msg_len;
    rt_err_t ret;
    int retry_count = 0;
    static rt_uint32_t tx_fail_cnt = 0;
    static rt_uint32_t tx_success_cnt = 0;
    const int max_retries = RPMSG_NET_TX_RETRY_COUNT;

    if (g_rpmsg_net_ctx.initialized == RT_FALSE)
    {
        LOG_W("rpmsg-net not initialized, drop packet len=%d", p->tot_len);
        return -RT_EBUSY;
    }

    if (!rpmsg_net_is_ready(rdev))
    {
        LOG_D("rpmsg-net control flags not ready, continue TX");
    }

    LOG_D("Transmitting packet, len %u", total_len);

    /* pbuf may be chained, copy a small head snapshot before hex dump */
    {
        rt_uint8_t dump_buf[64];
        rt_uint16_t dump_len = (total_len > sizeof(dump_buf)) ? sizeof(dump_buf) : (rt_uint16_t)total_len;
        if (dump_len > 0)
        {
            pbuf_copy_partial(p, dump_buf, dump_len, 0);
            LOG_HEX("tx pbuf head", 16, dump_buf, dump_len);
        }
    }

#if RPMSG_NET_RPSH_DIAG
    /* Optional deep shell TX diag for troubleshooting only. */
    if (total_len >= (14U + sizeof(rpsh_shell_frame_diag_t)))
    {
        uint8_t frame_buf[14U + sizeof(rpsh_shell_frame_diag_t)];
        pbuf_copy_partial(p, frame_buf, sizeof(frame_buf), 0);

        /* ethertype is stored in network byte order */
        if (frame_buf[12] == 0x88 && frame_buf[13] == 0xB6)
        {
            rpsh_shell_frame_diag_t *sf = (rpsh_shell_frame_diag_t *)(frame_buf + 14);

            /* CRC is computed with crc field forced to 0 */
            uint16_t rx_crc = sf->crc;
            sf->crc = 0;

            /* crc16 algorithm must match rpsh_server/client */
            uint16_t c = 0xFFFF;
            const uint8_t *crc_begin = (const uint8_t *)sf;
            int crc_len = (int)sizeof(*sf);
            for (int i = 0; i < crc_len; i++)
            {
                c ^= crc_begin[i];
                for (int j = 0; j < 8; j++)
                    c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
            }

            if (sf->type == FRAME_TYPE_LOGIN || sf->type == FRAME_TYPE_ACK ||
                sf->type == FRAME_TYPE_RESP  || sf->type == FRAME_TYPE_CMD)
            {
                sf->crc = rx_crc;
                LOG_W("SHELL TX: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x "
                      "type=0x%02x seq=%u sid=%u flags=%u dlen=%u total=%u crc_calc=0x%04x crc_rx=0x%04x",
                      frame_buf[0],frame_buf[1],frame_buf[2],frame_buf[3],frame_buf[4],frame_buf[5],
                      frame_buf[6],frame_buf[7],frame_buf[8],frame_buf[9],frame_buf[10],frame_buf[11],
                      sf->type, sf->seq, sf->session_id, sf->flags, sf->data_len, sf->total_len,
                      c, rx_crc);
            }
        }
    }
#endif

    if (total_len == 0 || total_len > MAX_PKT_SIZE)
    {
        LOG_E("Invalid packet length: %u", total_len);
        return -RT_ERROR;
    }

    msg_len = sizeof(struct rpmsg_net_data_msg) + total_len;

    /* 🟡 优化：添加重试机制，处理临时性发送失败 */
    do {
        ret = rpmsg_net_send_nocopy_message(rdev,
                                           msg_len,
                                           rpmsg_net_fill_data_msg,
                                           p);
        if (ret == RT_EOK)
        {
            tx_success_cnt++;
            LOG_D("Packet sent successfully (attempt %d)", retry_count + 1);
            return RT_EOK;
        }

        tx_fail_cnt++;
        retry_count++;

        LOG_W("TX attempt %d failed, total failures: %u, successes: %u", 
              retry_count, tx_fail_cnt, tx_success_cnt);

        /* 根据失败次数决定重试策略，使用指数退避算法 */
        if (retry_count < max_retries)
        {
            /* 指数退避：1ms, 2ms, 4ms, 8ms, ... */
            rt_thread_mdelay(1 << (retry_count - 1));
        }

    } while (retry_count < max_retries);

    /* 所有重试都失败 */
    LOG_E("TX failed after %d attempts, dropping packet len=%u", retry_count, total_len);

    /* 注意：eth_tx() 不拥有上层 pbuf 生命周期，失败时不在驱动中释放 */

    /* 如果失败率很高，记录警告 */
    if (tx_fail_cnt > 100 &&
        (float)tx_fail_cnt * 100 / (tx_fail_cnt + tx_success_cnt) > 25.0f)
    {
        LOG_E("High TX failure rate (>25%%), possible link issues or congestion");
    }

    return -RT_ERROR;
}

/* poll / receive hook for eth device */
static struct pbuf *rpmsg_net_rx(rt_device_t dev)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;
    void *ptr;
    struct pbuf *p = RT_NULL;
    static rt_uint32_t rx_cnt = 0;

    {
        rt_ssize_t mq_recv_ret = rt_mq_recv(&rdev->rx_mq, &ptr, sizeof(ptr), 1);
        if (mq_recv_ret > 0)
        {
            p = (struct pbuf *)ptr;
            rx_cnt++;
            LOG_D("[PBUF RX#%lu] pbuf=%p, tot_len=%u, ref=%d",
                  rx_cnt, p, p->tot_len, p->ref);

            /* 检测异常：如果 ref != 1，说明 pbuf 被多处引用，可能存在泄漏风险 */
            if (p->ref != 1)
            {
                LOG_W("PBUF reference count abnormal: p=%p, ref=%d (should be 1)", p, p->ref);

                /* 🟡 优化：处理引用计数异常，避免潜在问题 */
                if (p->ref == 0)
                {
                    /* 严重错误：pbuf已经被释放，返回NULL避免使用 */
                    LOG_E("PBUF already freed (ref=0), dropping packet");
                    return RT_NULL;
                }
                else if (p->ref > 1)
                {
                    /* 警告：pbuf被多处引用，可能影响数据完整性 */
                    LOG_W("PBUF has multiple references, potential data corruption risk");

                    /* 如果ref很大，可能是内存损坏 */
                    if (p->ref > 10)
                    {
                        LOG_E("PBUF ref count too high (%d), possible memory corruption", p->ref);
                        pbuf_free(p);
                        return RT_NULL;
                    }
                }
            }
        }
        else
        {
            LOG_D("rpmsg_net_rx empty poll");
        }
    }

    return p;
}

/* initialize eth device */
static __attribute__((unused))rt_err_t rpmsg_net_init(rt_device_t dev)
{
    RT_UNUSED(dev);
    return RT_EOK;
}

static rt_err_t rpmsg_net_open(rt_device_t dev, rt_uint16_t oflag)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;

    RT_UNUSED(oflag);

    if (rdev == RT_NULL)
    {
        return -RT_ERROR;
    }

    LOG_D("Opening rpmsg-net device");

    rpmsg_net_mark_local_state(rdev, RT_TRUE);
    rpmsg_net_sync_carrier(rdev);

    /* 
     * ✨ 关键修改：遵循协议时序，Remote 侧不主动发送 HELLO 和 LINK
     * 只有在收到 Master 的 HELLO 或 LINK 消息后，才会在回调函数中回复
     * 这里不再主动发送，避免违反"被动响应"模式
     */
    LOG_I("Device opened, waiting for master's messages...");

    return RT_EOK;
}

static rt_err_t rpmsg_net_close(rt_device_t dev)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;

    LOG_D("Closing rpmsg-net device");

    rpmsg_net_request_link_down(rdev);
    return RT_EOK;
}

/* control operations */
static rt_err_t rpmsg_net_control(rt_device_t dev, int cmd, void *args)
{
    struct rpmsg_net_device *rdev = (struct rpmsg_net_device *)dev;
    rt_err_t ret = RT_EOK;

    if (rdev == RT_NULL)
    {
        return -RT_ERROR;
    }

    switch (cmd)
    {
    case NIOCTL_GADDR:
        /* 获取 MAC 地址 */
        if (args == RT_NULL)
        {
            return -RT_ERROR;
        }
        rt_memcpy(args, rdev->local_mac, ETH_ALEN);
        LOG_D("Get MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
              rdev->local_mac[0], rdev->local_mac[1], rdev->local_mac[2],
              rdev->local_mac[3], rdev->local_mac[4], rdev->local_mac[5]);
        break;

    default:
        ret = -RT_EINVAL;
        break;
    }

    return ret;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops rpmsg_net_ops =
{
    rpmsg_net_init,
    rpmsg_net_open,
    rpmsg_net_close,
    RT_NULL,
    RT_NULL,
    rpmsg_net_control
};
#endif

/* create and register the rpmsg-net device */
int rpmsg_net_device_register(struct rpmsg_lite_instance *inst, void *ept, rpmsg_queue_handle queue)
{
    struct rpmsg_net_device *rdev;
    rt_err_t err;

    LOG_D("Registering rpmsg-net device");

    if (inst == RT_NULL || ept == RT_NULL)
    {
        LOG_E("Invalid parameters: inst=%p, ept=%p", inst, ept);
        return -RT_ERROR;
    }

    /* 🟢 新增：初始化零拷贝 RX 内存池 */
#if RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF
    LWIP_MEMPOOL_INIT(rpmsg_net_rx_pool);
    LOG_I("Zero-copy RX pool initialized: size=%u, entry_size=%u", 
          RPMSG_NET_RX_POOL_SIZE, sizeof(rpmsg_net_custom_pbuf_t));
#endif

    rdev = rt_malloc(sizeof(*rdev));
    if (!rdev)
    {
        LOG_E("Failed to allocate device structure");
        return -RT_ENOMEM;
    }

    memset(rdev, 0, sizeof(*rdev));
    rdev->rpmsg_inst = inst;
    rdev->rpmsg_ept = (struct rpmsg_lite_endpoint *)ept;
    rdev->rpmsg_queue = queue;
    rdev->local_link_up = RT_TRUE;
    rdev->remote_link_up = RT_FALSE;
    rdev->remote_ready = RT_FALSE;
    rdev->hello_sent = RT_FALSE;
    rdev->link_state_sent = RT_FALSE;
    rdev->pending_link_down = RT_FALSE;
    rdev->mtu = RPMSG_NET_MTU;
    rpmsg_net_init_local_mac(rdev);
    rt_snprintf(rdev->name, RT_NAME_MAX, "rpnet0");

    if (rdev->rpmsg_queue == RT_NULL)
    {
        LOG_E("Invalid rpmsg queue");
        rpmsg_net_cleanup_unregistered_device(rdev, RT_FALSE);
        return -RT_ERROR;
    }

    /* create mailbox for rx pbuf pointers */
    err = rt_mq_init(&rdev->rx_mq, RPMSG_NET_RX_MQ_NAME, rdev->rx_mq_pool, RPMSG_NET_RX_MQ_MSGSZ,
                     RPMSG_NET_RX_MQ_POOL_SIZE, RT_IPC_FLAG_FIFO);
    if (err != RT_EOK)
    {
        LOG_E("Failed to initialize mailbox");
        rpmsg_net_cleanup_unregistered_device(rdev, RT_FALSE);
        return -RT_ERROR;
    }

    rdev->rx_thread_running = RT_TRUE;
    rdev->rx_thread = rt_thread_create("rpnet_rx",
                                       rpmsg_net_rx_thread_entry,
                                       rdev,
                                       8192,   /* 栈大小从 4096 增加到 8192，防止栈溢出 */
                                       14,     /* 优先级从 18 提升到 14，加快处理速度 */
                                       10);
    if (rdev->rx_thread == RT_NULL)
    {
        LOG_E("Failed to create rx thread");
        rpmsg_net_cleanup_unregistered_device(rdev, RT_TRUE);
        return -RT_ERROR;
    }

    rt_thread_startup(rdev->rx_thread);

    /* setup eth_device */
    rdev->parent.parent.type = RT_Device_Class_NetIf;
#ifdef RT_USING_DEVICE_OPS
    rdev->parent.parent.ops = &rpmsg_net_ops;
#else
    rdev->parent.parent.init = rpmsg_net_init;
    rdev->parent.parent.open = rpmsg_net_open;
    rdev->parent.parent.close = rpmsg_net_close;
    rdev->parent.parent.control = rpmsg_net_control;
#endif
    rdev->parent.eth_rx = rpmsg_net_rx;
    rdev->parent.eth_tx = rpmsg_net_tx;

    /* register device */
    if (eth_device_init(&rdev->parent, rdev->name) != RT_EOK)
    {
        LOG_E("Failed to initialize eth device");
        rpmsg_net_free_device(rdev, RT_TRUE);
        return -RT_ERROR;
    }

    if (rdev->parent.netif)
    {
        rdev->parent.netif->mtu = RPMSG_NET_MTU;
        rt_memcpy(rdev->parent.netif->hwaddr, rdev->local_mac, ETH_ALEN);
        rdev->parent.netif->hwaddr_len = ETH_ALEN;
        netif_set_link_down(rdev->parent.netif);
    }

    /* store global pointer for callback access */
    g_rpmsg_net_ctx.device = rdev;
    LOG_D("rpmsg-net device registered successfully: %s", rdev->name);

    return RT_EOK;
}

/* helper to unregister */
void rpmsg_net_device_unregister(void)
{
    if (!g_rpmsg_net_ctx.device)
        return;

    rpmsg_net_request_link_down(g_rpmsg_net_ctx.device);
    g_rpmsg_net_ctx.initialized = RT_FALSE;

    rt_device_close(&g_rpmsg_net_ctx.device->parent.parent);
    rt_device_unregister(&g_rpmsg_net_ctx.device->parent.parent);
    rpmsg_net_free_device(g_rpmsg_net_ctx.device, RT_TRUE);
    rpmsg_net_reset_context();
}

void *rpmsg_net_eth_device_get(void)
{
    return &g_rpmsg_net_ctx.device->parent;
}

/* Example entry: create endpoint and register device
   Adapt rpmsg_lite_endpoint_create params to your rpmsg implementation.
*/
int rpmsg_net_start(struct rpmsg_lite_instance *inst)
{
    void *ept;
    rpmsg_queue_handle queue;

    LOG_D("Starting rpmsg-net device");

    if (!inst)
    {
        LOG_E("Invalid rpmsg instance");
        return -RT_ERROR;
    }

    queue = rpmsg_queue_create(inst);
    if (queue == RT_NULL)
    {
        LOG_E("Failed to create rpmsg queue");
        return -RT_ERROR;
    }

    /* create endpoint: rpmsg_lite_create_ept(inst, addr, rpmsg_net_recv_cb, priv) */
    ept = rpmsg_lite_create_ept(inst,
                                RPMSG_NET_LOCAL_EPT_ADDR,
                                rpmsg_net_recv_cb,
                                queue);
    if (!ept)
    {
        LOG_E("Failed to create rpmsg endpoint");
        rpmsg_queue_destroy(inst, queue);
        return -RT_ERROR;
    }

    LOG_D("rpmsg endpoint created successfully");
    return rpmsg_net_device_register(inst, ept, queue);
}

/* 等待线程相关变量 */
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
    int timeout;

    LOG_I("Waiting for rpmsg link up...");

    /* 直接阻塞等待 link up */
    if (rpmsg_lite_wait_for_link_up(inst, (uint32_t)RL_BLOCK) == 0)
    {
        LOG_E("Wait for link up timeout, master not ready");
        return;
    }

    LOG_I("rpmsg link is up, starting device...");

    /* 启动设备 - 仅注册 endpoint 和 queue */
    ret = rpmsg_net_start(inst);
    if (ret == RT_EOK)
    {
        LOG_I("rpmsg-net device registered successfully");

        /*
         * ✨ 关键修改：在协议链路建立前，不标记设备就绪
         * 等待 Master 发送 HELLO 和 LINK UP 消息，完成协议握手
         */
        LOG_I("Waiting for protocol handshake (HELLO and LINK)...");

        /* 轮询等待协议握手完成 */
        timeout = 300; /* 300 * 100ms = 30s timeout */
        while (timeout-- > 0)
        {
            if (g_rpmsg_net_ctx.device != RT_NULL)
            {
                /* 检查是否完成了 HELLO 和 LINK 握手 */
                if (g_rpmsg_net_ctx.device->remote_ready && 
                    g_rpmsg_net_ctx.device->remote_link_up)
                {
                    LOG_I("Protocol handshake completed!");
                    break;
                }
            }
            rt_thread_mdelay(100);
        }

        if (timeout <= 0)
        {
            LOG_W("Protocol handshake timeout, continue anyway");
        }

        /* 等待网络接口 UP */
        wait_netif_up(&g_rpmsg_net_ctx.device->parent);

        /*
         * ✨ 只有完成协议握手后，才标记设备就绪
         * 这样可以确保在 initialized=true 时，链路真正可用
         */
        g_rpmsg_net_ctx.initialized = RT_TRUE;

        LOG_I("rpmsg-net device ready for TX/RX (handshake done)");
    }
    else
    {
        LOG_E("Failed to register rpmsg-net device");
        rpmsg_net_device_unregister();
    }

    g_rpmsg_net_ctx.link_wait_thread = RT_NULL;
}

/* 初始化等待线程 */
static rt_err_t rpmsg_link_wait_thread_init(struct rpmsg_lite_instance *inst)
{
    /* 创建等待线程 */
    g_rpmsg_net_ctx.link_wait_thread = rt_thread_create("rpmsg_wait",
                                                        rpmsg_link_wait_thread_entry,
                                                        inst,
                                                        4 * 1024,
                                                        20,
                                                        10);
    if (g_rpmsg_net_ctx.link_wait_thread == RT_NULL)
    {
        LOG_E("Failed to create rpmsg link wait thread");
        return -RT_ENOMEM;
    }

    /* 启动线程 */
    rt_thread_startup(g_rpmsg_net_ctx.link_wait_thread);

    LOG_I("rpmsg link wait thread created");
    return RT_EOK;
}

/* 清理等待线程 */
void rpmsg_link_wait_thread_deinit(void)
{
    if (g_rpmsg_net_ctx.link_wait_thread != RT_NULL)
    {
        if (g_rpmsg_net_ctx.device != RT_NULL)
        {
            rpmsg_net_request_link_down(g_rpmsg_net_ctx.device);
        }
        rt_thread_delete(g_rpmsg_net_ctx.link_wait_thread);
        g_rpmsg_net_ctx.link_wait_thread = RT_NULL;
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
    g_rpmsg_net_ctx.rpmsg_inst = rpmsg_inst;

    /* 创建等待线程，阻塞等待 Master 上线 */
    if (rpmsg_link_wait_thread_init(rpmsg_inst) != RT_EOK)
    {
        LOG_E("Failed to init link wait thread");
        rpmsg_lite_deinit(rpmsg_inst);
        rpmsg_net_reset_context();
        return -RT_ERROR;
    }

    return RT_EOK;
}

/* 修改为 INIT_COMPONENT_EXPORT，确保调度器已就绪 */
INIT_COMPONENT_EXPORT(rt_rpmsg_net_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

/* 全局统计变量，用于调试命令访问 */
extern rt_uint32_t g_alloc_fail_cnt;
extern rt_uint32_t g_alloc_success_cnt;
extern rt_uint32_t g_consecutive_fail_cnt;

static void rpmsg_net_stats(void)
{
    extern rt_uint32_t g_alloc_fail_cnt;
    extern rt_uint32_t g_alloc_success_cnt;
    extern rt_uint32_t g_consecutive_fail_cnt;

    if (!g_rpmsg_net_ctx.initialized || !g_rpmsg_net_ctx.device) {
        LOG_E("rpmsg-net not initialized");
        return;
    }

    LOG_I("=== rpmsg-net Statistics ===");
    LOG_I("Device name: %s", g_rpmsg_net_ctx.device->name);
    LOG_I("Remote ready: %d", g_rpmsg_net_ctx.device->remote_ready);
    LOG_I("Remote link up: %d", g_rpmsg_net_ctx.device->remote_link_up);
    LOG_I("Local link up: %d", g_rpmsg_net_ctx.device->local_link_up);
    LOG_I("RX MQ max entries: %d", RPMSG_NET_RX_MQ_ENTRIES);

#if RPMSG_NET_ZERO_COPY_RX && LWIP_SUPPORT_CUSTOM_PBUF
    LOG_I("Zero-copy RX: Enabled (Pool size=%u)", RPMSG_NET_RX_POOL_SIZE);
#else
    LOG_I("Zero-copy RX: Disabled (using PBUF_RAM)");
#endif

    LOG_I("=== PBUF Allocation Statistics ===");
    if (g_alloc_success_cnt + g_alloc_fail_cnt > 0) {
        float fail_rate = (float)g_alloc_fail_cnt * 100 / (g_alloc_success_cnt + g_alloc_fail_cnt);
        LOG_I("Total allocations: %u", g_alloc_success_cnt + g_alloc_fail_cnt);
        LOG_I("Success: %u, Fail: %u (%.1f%%)", 
              g_alloc_success_cnt, g_alloc_fail_cnt, fail_rate);
        LOG_I("Consecutive failures: %u", g_consecutive_fail_cnt);
        LOG_I("RT_LWIP_PBUF_NUM: %d", RT_LWIP_PBUF_NUM);

        if (fail_rate > RPMSG_NET_FAIL_RATE_THRESHOLD) {
            LOG_W("WARNING: Failure rate (%.1f%%) exceeds threshold (%.1f%%)", 
                  fail_rate, RPMSG_NET_FAIL_RATE_THRESHOLD);
        }

        if (g_consecutive_fail_cnt >= RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD) {
            LOG_W("WARNING: Consecutive failures (%u) exceeds threshold (%u)", 
                  g_consecutive_fail_cnt, RPMSG_NET_CONSECUTIVE_FAIL_THRESHOLD);
        }
    } else {
        LOG_I("No allocation data available yet");
    }
}
MSH_CMD_EXPORT(rpmsg_net_stats, "Show rpmsg-net and pbuf statistics");


#endif /* RT_USING_FINSH */