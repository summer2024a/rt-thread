#ifndef __SHELL_PROTO_H__
#define __SHELL_PROTO_H__

#include <stdint.h>

#define SHELL_ETH_TYPE          0x88B6

#define FRAME_TYPE_LOGIN        1
#define FRAME_TYPE_LOGOUT       2
#define FRAME_TYPE_CMD          3
#define FRAME_TYPE_RESP         4
#define FRAME_TYPE_HEARTBEAT    5
#define FRAME_TYPE_ACK          6
#define FRAME_TYPE_PTY          7      // 虚拟终端数据
#define FRAME_TYPE_FILE         8      // 文件传输
#define FRAME_TYPE_RESIZE       9      // 窗口大小变化

#define PACKET_FLAG_MORE        0x01
#define PACKET_FLAG_LAST        0x00
#define PACKET_FLAG_ASYNC       0x80

#define MAX_SESSIONS            4
#define INVALID_SESS_ID         0xFF
#define SESSION_TIMEOUT_SEC     20
#define HB_INTERVAL_SEC         4
#define MAX_PAYLOAD_PER_PACKET  128

#define RECV_BUF_SIZE           4096
#define SEND_BUF_SIZE           4096

#define SHELL_PWD_ADMIN         "admin123"
#define SHELL_PWD_USER          "user123"

typedef struct shell_frame {
    uint8_t     type;
    uint8_t     flags;
    uint8_t     session_id;
    uint8_t     reserved;

    uint16_t    seq;
    uint16_t    data_len;
    uint16_t    total_len;

    uint16_t    crc;            // 帧校验
    uint8_t     data[MAX_PAYLOAD_PER_PACKET];
} __attribute__((packed)) shell_frame_t;

typedef struct {
    uint16_t    pkt_cnt;
    uint16_t    drop_cnt;
    uint32_t    bytes;
} stat_t;

#endif
