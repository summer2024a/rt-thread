/*
 * Copyright (c) 2024, RT-Thread Development Team
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __RPMSG_NET_PROTO_H__
#define __RPMSG_NET_PROTO_H__

#include <rtdef.h>

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#define RPMSG_NET_LINK_ID          0U
#define RPMSG_NET_LOCAL_EPT_ADDR   0x101U
#define RPMSG_NET_REMOTE_EPT_ADDR  0x100U
#define RPMSG_NET_MTU              1500U

#define RPMSG_NET_PROTO_VER        0x0001U

#define RPMSG_NET_PKT_TYPE_DATA    0x01U
#define RPMSG_NET_PKT_TYPE_HELLO   0x02U
#define RPMSG_NET_PKT_TYPE_LINK    0x03U

struct rpmsg_net_msg_hdr
{
    rt_uint16_t type;
    rt_uint16_t version;
    rt_uint32_t length;
} __attribute__((packed));

struct rpmsg_net_hello_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint8_t mac[ETH_ALEN];
    rt_uint16_t mtu;
    rt_uint16_t reserved;
} __attribute__((packed));

struct rpmsg_net_link_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint32_t link_up;
} __attribute__((packed));

struct rpmsg_net_data_msg
{
    struct rpmsg_net_msg_hdr hdr;
    rt_uint8_t payload[0];
} __attribute__((packed));

#endif /* __RPMSG_NET_PROTO_H__ */
