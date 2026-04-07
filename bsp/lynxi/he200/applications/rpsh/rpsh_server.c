#include <rtthread.h>
#include <rtdevice.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/prot/ethernet.h>
#include <lwip/inet.h>
#include <netif/ethernetif.h>
#include <netdev.h>
#include <string.h>
#include <finsh.h>
#include <shell.h>
#include <msh.h>
// #include <sys/socket.h>
#ifdef RT_USING_POSIX_STDIO
#include <fcntl.h>
#include <posix/stdio.h>
#endif
#ifdef RT_USING_ULOG
#include <ulog.h>
#endif
#include "rpsh_proto.h"
#define DBG_TAG "rpsh"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

#ifndef RPSH_DEBUG
#define RPSH_DEBUG 0
#endif

#if RPSH_DEBUG
#define RPSH_DBG(...) LOG_W(__VA_ARGS__)
#else
#define RPSH_DBG(...)
#endif

#if defined(RT_USING_SMP) && defined(BSP_RPMSG_NET_BIND_CPU0)
#ifndef BSP_RPMSG_NET_CPU
#define BSP_RPMSG_NET_CPU 0
#endif
#endif

extern void *rpmsg_net_eth_device_get(void);
extern struct pbuf *rpmsg_net_shell_rx_try(struct eth_device *edev);
/* Optional symbol: may be absent in some FINSH/POSIX stdio configs. */
extern void finsh_set_device(const char *device_name) __attribute__((weak));
extern const char *finsh_get_device(void) __attribute__((weak));

typedef struct shell_session {
    rt_bool_t  used;
    uint8_t    id;
    uint8_t    mac[6];
    rt_tick_t  last_hb;

    uint16_t   last_seq;
    rt_bool_t  seq_inited;

    uint8_t    recv_buf[RECV_BUF_SIZE];
    uint32_t   recv_off;
    uint32_t   recv_total;

    int         auth;           // 0=guest 1=admin
    int         fail_cnt;
    rt_tick_t   flood_ck;
    stat_t      stat;
} shell_session_t;

static struct eth_device *s_edev;
static shell_session_t s_sess[MAX_SESSIONS];

static uint8_t *g_out_buf;
static uint32_t g_out_left;
static rt_device_t g_console_passthrough = RT_NULL;
static struct rt_mutex g_exec_lock;
static const char *g_default_console_name = RT_NULL;
static rt_bool_t g_exec_in_progress = RT_FALSE;

/* Avoid large per-thread stack usage.
 * shell_server_entry uses RECV_BUF_SIZE/SEND_BUF_SIZE(typically 4096 each),
 * so keep them as file-static buffers to prevent stack overflow.
 */
static uint8_t g_shell_pkt[1520];
static uint8_t g_cmd_buf[RECV_BUF_SIZE];
static uint8_t g_resp_buf[SEND_BUF_SIZE];

#define RPSH_MEM_CONSOLE_DEV_NAME "rpsh-mem-console"

/* Client strips this footer and refreshes the input line (after Tab completion). */
#define RPSH_LINE_MAGIC "\xffRPSH_LINE\n"
#define RPSH_LINE_END   "\n\xffRPSH_END\n"

/* A minimal console device that redirects rt_kprintf output into memory.
 * This allows host to fetch command stdout via rpmsg instead of relying on UART.
 */
static struct rt_device s_mem_console_dev;
#ifdef RT_USING_ULOG
static struct ulog_backend g_rpsh_ulog_be;
#endif
static rt_bool_t g_log_capture_enabled = RT_FALSE;
static char g_log_backlog[4096];
static uint32_t g_log_backlog_len = 0;
static uint16_t g_async_seq = 1;

static void rpsh_capture_append(const char *data, rt_size_t len)
{
    if (!data || len == 0 || g_out_left == 0)
        return;

    rt_size_t copy = len;
    if (copy > g_out_left)
        copy = g_out_left;

    rt_memcpy(g_out_buf, data, copy);
    g_out_buf += copy;
    g_out_left -= copy;
}

static void rpsh_backlog_append(const char *data, rt_size_t len)
{
    if (!data || len == 0 || !g_log_capture_enabled)
        return;

    if (len >= sizeof(g_log_backlog))
    {
        rt_memcpy(g_log_backlog, data + (len - sizeof(g_log_backlog)), sizeof(g_log_backlog));
        g_log_backlog_len = sizeof(g_log_backlog);
        return;
    }

    if (g_log_backlog_len + len > sizeof(g_log_backlog))
    {
        uint32_t drop = g_log_backlog_len + len - sizeof(g_log_backlog);
        rt_memmove(g_log_backlog, g_log_backlog + drop, g_log_backlog_len - drop);
        g_log_backlog_len -= drop;
    }
    rt_memcpy(g_log_backlog + g_log_backlog_len, data, len);
    g_log_backlog_len += len;
}

static void rpsh_backlog_flush_to_out(void)
{
    if (g_log_backlog_len == 0)
        return;

    rpsh_capture_append(g_log_backlog, g_log_backlog_len);
    g_log_backlog_len = 0;
}

static uint32_t rpsh_backlog_take(uint8_t *buf, uint32_t max_len)
{
    if (!buf || max_len == 0 || g_log_backlog_len == 0)
        return 0;

    uint32_t take = g_log_backlog_len;
    if (take > max_len)
        take = max_len;

    rt_memcpy(buf, g_log_backlog, take);
    if (take < g_log_backlog_len)
    {
        rt_memmove(g_log_backlog, g_log_backlog + take, g_log_backlog_len - take);
    }
    g_log_backlog_len -= take;
    return take;
}

#ifdef RT_USING_ULOG
static void rpsh_ulog_output(struct ulog_backend *backend,
                             rt_uint32_t level,
                             const char *tag,
                             rt_bool_t is_raw,
                             const char *log,
                             rt_size_t len)
{
    (void)backend;
    (void)level;
    (void)tag;
    (void)is_raw;

    if (!g_log_capture_enabled || log == RT_NULL || len == 0)
        return;

    if (g_exec_in_progress)
        rpsh_capture_append(log, len);
    else
        rpsh_backlog_append(log, len);
}
#endif

static rt_err_t mem_console_init(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_err_t mem_console_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t mem_console_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_ssize_t mem_console_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;
    (void)buffer;
    (void)size;
    return 0;
}

static rt_ssize_t mem_console_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    (void)dev;

    if (buffer == RT_NULL || size == 0)
        return 0;

    /* 1) Capture for host RESP (g_out_buf). */
    if (g_out_left > 0)
    {
        rt_size_t write_len = size;
        if (write_len > g_out_left)
            write_len = g_out_left;

        rt_memcpy(g_out_buf, buffer, write_len);
        g_out_buf += write_len;
        g_out_left -= write_len;
    }

    /* 2) Tee to the UART saved in g_console_passthrough so local serial stays usable. */
    if (g_console_passthrough)
        (void)rt_device_write(g_console_passthrough, pos, buffer, size);

    return size;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops s_mem_console_ops =
{
    .init   = mem_console_init,
    .open   = mem_console_open,
    .close  = mem_console_close,
    .read   = mem_console_read,
    .write  = mem_console_write,
    .control = RT_NULL,
};
#endif

static uint16_t crc16(const uint8_t *d, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

static void restore_console_paths(const char *name)
{
    if (name == RT_NULL) return;
    rt_console_set_device(name);
    if (finsh_set_device)
    {
        finsh_set_device(name);
    }
#ifdef RT_USING_POSIX_STDIO
    /* finsh_getchar() uses read(rt_posix_stdio_get_console()); must track UART
     * after rt_console_set_device(), which closes the previous console device. */
    rt_posix_stdio_set_console(name, O_RDWR);
    /* Rebuild stdio fd 0/1/2 mapping to avoid stale stdin after console switch. */
    rt_posix_stdio_init();
#endif
}

static void ensure_console_restored(void)
{
    if (g_exec_in_progress || g_default_console_name == RT_NULL)
        return;

    rt_device_t cur = rt_console_get_device();
    if (cur != RT_NULL && cur->parent.name != RT_NULL &&
        rt_strcmp(cur->parent.name, g_default_console_name) == 0)
        return;

    /* With RT_USING_POSIX_STDIO, repeated rt_console_set_device/rt_posix_stdio_init
     * from a tight loop breaks serial input. Throttle restores when console drifts. */
    {
        static rt_tick_t last_restore;
        rt_tick_t now = rt_tick_get();

        if ((rt_tick_t)(now - last_restore) < rt_tick_from_millisecond(500))
            return;
        last_restore = now;
    }

    restore_console_paths(g_default_console_name);
}

static void restore_console_paths_strong(const char *name)
{
    if (name == RT_NULL) return;
    restore_console_paths(name);
    rt_thread_mdelay(1);
    restore_console_paths(name);
}

static rt_bool_t has_active_session(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (s_sess[i].used)
            return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_bool_t is_pull_cmd(const char *cmd)
{
    if (cmd == RT_NULL) return RT_FALSE;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (rt_strncmp(cmd, "pull", 4) != 0) return RT_FALSE;
    cmd += 4;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    return *cmd == '\0';
}

static int exec_cmd(const char *cmd, uint8_t *out, uint32_t max, uint32_t *out_len) {
    int result = -RT_ERROR;

    if (out == RT_NULL || max == 0 || out_len == RT_NULL)
        return -RT_EINVAL;

    rt_memset(out, 0, max);
    *out_len = 0;

    rt_mutex_take(&g_exec_lock, RT_WAITING_FOREVER);
    g_out_buf = out;
    g_out_left = max;
    g_exec_in_progress = RT_TRUE;
    g_console_passthrough = rt_console_get_device();
    RPSH_DBG("exec_cmd begin: cmd='%s' console=%s",
             cmd,
             (g_console_passthrough && g_console_passthrough->parent.name) ?
             g_console_passthrough->parent.name : "null");

    /* Avoid calling msh_exec while scheduler is locked. */
    if (rt_sched_is_locked())
    {
        result = -RT_EBUSY;
        *out_len = rt_snprintf((char *)out, max, "busy: scheduler locked");
        goto __cleanup;
    }

#ifdef ULOG_USING_ASYNC_OUTPUT
    ulog_async_output_enabled(RT_FALSE);
#endif
    /* Capture msh rt_kprintf output for host echo. */
    rt_console_set_device(RPSH_MEM_CONSOLE_DEV_NAME);
    rpsh_backlog_flush_to_out();

    {
        char *mutable_cmd = (char *)cmd;
        rt_size_t cmdlen = rt_strlen(cmd);
        rt_bool_t tab_req = (cmdlen > 0 && mutable_cmd[cmdlen - 1] == '\t');

        if (tab_req)
        {
            mutable_cmd[cmdlen - 1] = '\0';
            cmdlen--;
        }

        if (tab_req)
        {
            /* Same sequence as finsh shell_auto_complete(): listings + FINSH prompt + line */
            char work[FINSH_CMD_SIZE + 1];

            rt_strncpy(work, mutable_cmd, FINSH_CMD_SIZE);
            work[FINSH_CMD_SIZE] = '\0';

            rpsh_capture_append("\n", 1);
            msh_auto_complete(work);
#ifdef FINSH_USING_OPTION_COMPLETION
            msh_opt_auto_complete(work);
#endif
            {
                const char *pr = finsh_get_prompt();

                if (pr)
                    rpsh_capture_append(pr, rt_strlen(pr));
                rpsh_capture_append(work, rt_strlen(work));
            }
            rpsh_capture_append(RPSH_LINE_MAGIC, sizeof(RPSH_LINE_MAGIC) - 1);
            rpsh_capture_append(work, rt_strlen(work));
            rpsh_capture_append(RPSH_LINE_END, sizeof(RPSH_LINE_END) - 1);
            result = 0;
        }
        else
        {
            rpsh_capture_append("$ ", 2);
            if (cmdlen > 0)
                rpsh_capture_append(mutable_cmd, cmdlen);
            rpsh_capture_append("\r\n", 2);
            if (is_pull_cmd(mutable_cmd))
            {
                /* pull: flush captured logs only, no command execution. */
                result = 0;
            }
            else
            {
                result = msh_exec(mutable_cmd, cmdlen);
            }
        }
    }

__cleanup:
    g_exec_in_progress = RT_FALSE;
    /* exec_cmd currently doesn't switch global console, so avoid aggressive
     * restore on each command to prevent stdin path churn. */
    if (g_default_console_name)
        restore_console_paths(g_default_console_name);
    else if (g_console_passthrough && g_console_passthrough->parent.name)
        restore_console_paths(g_console_passthrough->parent.name);
    g_console_passthrough = RT_NULL;

    out[max - 1] = '\0';
    *out_len = (uint32_t)(max - g_out_left);
    if (*out_len == 0)
    {
        *out_len = rt_snprintf((char *)out, max, "[no output] ret=%d", result);
        if (*out_len >= max) *out_len = max - 1;
    }

    RPSH_DBG("exec_cmd end: cmd='%s' ret=%d out_len=%u", cmd, result, *out_len);
    rt_mutex_release(&g_exec_lock);
    return result;
}

static int cmd_sid(int argc, char **argv) {
    if (argc < 2) {
        rt_kprintf("sid list | sid kick <id>\n");
        return 0;
    }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            shell_session_t *s = &s_sess[i];
            if (s->used)
                rt_kprintf("sid:%d auth:%d mac:%02x:%02x:%02x:%02x:%02x:%02x\n",
                    s->id, s->auth, s->mac[0],s->mac[1],s->mac[2],s->mac[3],s->mac[4],s->mac[5]);
        }
        return 0;
    }
    if (!strcmp(argv[1], "kick") && argc == 3) {
        int id = atoi(argv[2]);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sess[i].used && s_sess[i].id == id) {
                rt_memset(&s_sess[i], 0, sizeof(shell_session_t));
                rt_kprintf("kick ok\n");
                return 0;
            }
        }
        rt_kprintf("no such sid\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_sid, sid, session mgmt);

static int cmd_stat(int argc, char **argv) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        shell_session_t *s = &s_sess[i];
        if (s->used)
            rt_kprintf("sid:%d pkt:%u drop:%u bytes:%u\n",
                s->id, s->stat.pkt_cnt, s->stat.drop_cnt, s->stat.bytes);
    }
    return 0;
}
MSH_CMD_EXPORT(cmd_stat, show shell stat);

static int cmd_rpsh_diag(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_device_t console = rt_console_get_device();
    rt_thread_t tshell = rt_thread_find("tshell");
    const char *finsh_dev = finsh_get_device ? finsh_get_device() : "n/a";

    rt_kprintf("rpsh_diag:\n");
    rt_kprintf("  default_console : %s\n", g_default_console_name ? g_default_console_name : "null");
    rt_kprintf("  rt_console      : %s\n",
               (console && console->parent.name) ? console->parent.name : "null");
    rt_kprintf("  finsh_device    : %s\n", finsh_dev ? finsh_dev : "null");
    rt_kprintf("  exec_in_progress: %d\n", g_exec_in_progress ? 1 : 0);
    rt_kprintf("  tshell          : %s\n", tshell ? "found" : "not found");
    if (tshell)
    {
        rt_kprintf("  tshell_stat     : 0x%02x\n", RT_SCHED_CTX(tshell).stat);
    }
#ifdef RT_USING_POSIX_STDIO
    rt_kprintf("  posix_console_fd: %d\n", rt_posix_stdio_get_console());
#else
    rt_kprintf("  posix_console_fd: n/a\n");
#endif
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_rpsh_diag, rpsh_diag, show rpsh diagnostic state);

static void send_frame(shell_session_t *s, uint8_t type, uint8_t flags,
                       uint16_t seq, const uint8_t *data, uint16_t dlen, uint16_t total) {
    uint8_t pkt[1520];
    struct eth_hdr *eh = (void*)pkt;
    shell_frame_t *sf = (void*)(eh + 1);
    rt_memcpy((uint8_t*)&eh->dest, s->mac, 6);
    rt_memcpy((uint8_t*)&eh->src, s_edev->netif->hwaddr, 6);
    eh->type = htons(SHELL_ETH_TYPE);
    rt_memset(sf, 0, sizeof(*sf));
    sf->type = type;
    sf->flags = flags;
    sf->session_id = s->id;
    sf->seq = seq;
    sf->data_len = dlen;
    sf->total_len = total;
    if (dlen) rt_memcpy(sf->data, data, dlen);
    /* CRC is computed with crc field forced to 0 */
    sf->crc = 0;
    sf->crc = crc16((uint8_t*)sf, sizeof(*sf));
    int size = sizeof(*eh) + sizeof(*sf);
    /* Use eth_tx callback if available */
    if (s_edev->eth_tx) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_RAM);
        if (p) {
            pbuf_take(p, pkt, size);
            s_edev->eth_tx((rt_device_t)s_edev, p);
            /* rpmsg-net eth_tx does not free pbuf on success */
            pbuf_free(p);
        }
    }
}

static void send_resp(shell_session_t *s, uint16_t seq, const uint8_t *data, uint32_t total, uint8_t extra_flags) {
    uint32_t off = 0;
    RPSH_DBG("rpsh send_resp: sid=%d seq=%u total=%u", s ? s->id : -1, seq, total);
    while (off < total) {
        uint16_t frag = total - off;
        if (frag > MAX_PAYLOAD_PER_PACKET) frag = MAX_PAYLOAD_PER_PACKET;
        uint8_t flg = ((off + frag < total) ? PACKET_FLAG_MORE : PACKET_FLAG_LAST) | extra_flags;
        RPSH_DBG("rpsh send_resp frag: sid=%d seq=%u off=%u frag=%u flags=0x%02x",
                 s ? s->id : -1, seq, off, frag, flg);
        send_frame(s, FRAME_TYPE_RESP, flg, seq++, data + off, frag, total);
        off += frag;
    }
}

static shell_session_t *find_sess(const uint8_t *mac) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (s_sess[i].used && !memcmp(s_sess[i].mac, mac, 6))
            return &s_sess[i];
    return NULL;
}

static shell_session_t *alloc_sess(const uint8_t *mac) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sess[i].used) {
            rt_memset(&s_sess[i], 0, sizeof(shell_session_t));
            s_sess[i].used = RT_TRUE;
            s_sess[i].id = i + 1;
            rt_memcpy(s_sess[i].mac, mac, 6);
            s_sess[i].last_hb = rt_tick_get();
            return &s_sess[i];
        }
    }
    return NULL;
}

static void timeout_check(void) {
    rt_tick_t now = rt_tick_get();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        shell_session_t *s = &s_sess[i];
        if (s->used && (now - s->last_hb) > RT_TICK_PER_SECOND * SESSION_TIMEOUT_SEC) {
            rt_memset(s, 0, sizeof(shell_session_t));
            if (!has_active_session())
            {
                g_log_capture_enabled = RT_FALSE;
                g_log_backlog_len = 0;
            }
            if (g_default_console_name)
            {
                g_exec_in_progress = RT_FALSE;
                restore_console_paths_strong(g_default_console_name);
            }
        }
    }
    ensure_console_restored();
}

static void shell_server_entry(void *arg) {
    (void)arg;
    uint8_t *pkt = g_shell_pkt;
    uint8_t *cmd_buf = g_cmd_buf;
    uint8_t *resp_buf = g_resp_buf;

    while(1) {
        s_edev = rpmsg_net_eth_device_get();
        if (!s_edev)
            rt_thread_mdelay(1000);
        else
            break;
    }

    LOG_I("rpsh server started");

    while (1) {
        timeout_check();
        if (!g_exec_in_progress && g_log_backlog_len > 0)
        {
            uint8_t async_buf[512];
            uint32_t n = rpsh_backlog_take(async_buf, sizeof(async_buf));
            if (n > 0)
            {
                for (int i = 0; i < MAX_SESSIONS; i++)
                {
                    if (s_sess[i].used)
                    {
                        send_resp(&s_sess[i], g_async_seq++, async_buf, n, PACKET_FLAG_ASYNC);
                    }
                }
            }
        }
        int rx = 0;

        /* RPSH frames use shell_mq only; never steal ICMP/ARP from lwIP rx_mq. */
        if (s_edev) {
            struct pbuf *p = rpmsg_net_shell_rx_try(s_edev);
            if (p) {
                rt_uint16_t copy_len = (p->tot_len > (int)sizeof(g_shell_pkt)) ?
                    (rt_uint16_t)sizeof(g_shell_pkt) : (rt_uint16_t)p->tot_len;

                pbuf_copy_partial(p, pkt, copy_len, 0);
                rx = copy_len;
                pbuf_free(p);
            }
        }
        if (rx <= 0) { rt_thread_mdelay(10); continue; }

        struct eth_hdr *eh = (void*)pkt;
        if (ntohs(eh->type) != SHELL_ETH_TYPE) continue;

        shell_frame_t *sf = (void*)(eh + 1);
        uint16_t rx_crc = sf->crc;
        sf->crc = 0;
        uint16_t crc = crc16((uint8_t*)sf, sizeof(*sf));
        sf->crc = rx_crc;
        if (crc != rx_crc) continue;

        shell_session_t *s = find_sess((uint8_t*)&eh->src);

        if (sf->type == FRAME_TYPE_LOGIN) {
            const char *pwd = (const char*)sf->data;
            int auth = 0;
            if (sf->data_len == strlen(SHELL_PWD_ADMIN) &&
                !memcmp(pwd, SHELL_PWD_ADMIN, sf->data_len)) auth = 1;
            else if (sf->data_len == strlen(SHELL_PWD_USER) &&
                     !memcmp(pwd, SHELL_PWD_USER, sf->data_len)) auth = 0;
            else {
                if (s) s->fail_cnt++;
                send_frame(s ? s : (void*)s_sess, FRAME_TYPE_ACK,0,sf->seq,(uint8_t*)"pwd err",6,6);
                continue;
            }
            /* Allow same-MAC re-login by reusing existing session. */
            if (!(s && s->used))
            {
                s = alloc_sess((uint8_t*)&eh->src);
                if (!s)
                {
                    shell_session_t tmp;

                    rt_memset(&tmp, 0, sizeof(tmp));
                    tmp.used = RT_TRUE;
                    rt_memcpy(tmp.mac, &eh->src, 6);
                    send_frame(&tmp, FRAME_TYPE_ACK, 0, sf->seq,
                               (uint8_t *)"sess full", 9, 9);
                    continue;
                }
            }
            s->auth = auth;
            s->last_hb = rt_tick_get();
            g_log_capture_enabled = RT_TRUE;
            g_log_backlog_len = 0;
            char msg[16];
            rt_snprintf(msg, sizeof(msg), "ok sid=%d", s->id);
            send_frame(s, FRAME_TYPE_ACK, 0, sf->seq, (uint8_t*)msg, strlen(msg), strlen(msg));
            continue;
        }

        if (!s || !s->used) continue;
        s->last_hb = rt_tick_get();
        s->stat.pkt_cnt++;

        if (sf->type == FRAME_TYPE_HEARTBEAT) {
            send_frame(s, FRAME_TYPE_ACK, 0, sf->seq, (uint8_t*)"hb", 2, 2);
        } else if (sf->type == FRAME_TYPE_LOGOUT) {
            send_frame(s, FRAME_TYPE_ACK,0,sf->seq,(uint8_t*)"logout",6,6);
            rt_memset(s, 0, sizeof(*s));
            if (!has_active_session())
            {
                g_log_capture_enabled = RT_FALSE;
                g_log_backlog_len = 0;
            }
            if (g_default_console_name)
            {
                g_exec_in_progress = RT_FALSE;
                restore_console_paths_strong(g_default_console_name);
            }
        } else if (sf->type == FRAME_TYPE_CMD) {
            uint32_t cmd_len;
            if (sf->flags & PACKET_FLAG_MORE) {
                if (s->recv_off + sf->data_len < RECV_BUF_SIZE) {
                    rt_memcpy(s->recv_buf + s->recv_off, sf->data, sf->data_len);
                    s->recv_off += sf->data_len;
                }
                continue;
            }
            rt_memcpy(s->recv_buf + s->recv_off, sf->data, sf->data_len);
            cmd_len = s->recv_off + sf->data_len;
            s->recv_off = 0;
            if (cmd_len >= RECV_BUF_SIZE) cmd_len = RECV_BUF_SIZE-1;
            rt_memcpy(cmd_buf, s->recv_buf, cmd_len);
            cmd_buf[cmd_len] = 0;

            uint32_t resp_len;
            exec_cmd((const char*)cmd_buf, resp_buf, sizeof(g_resp_buf), &resp_len);
            RPSH_DBG("rpsh exec_cmd done: sid=%d cmd='%s' resp_len=%u",
                     s->id, (const char*)cmd_buf, resp_len);
            if (resp_len > 0)
            {
                uint32_t dump_len = resp_len > 16 ? 16 : resp_len;
                char hex_line[16 * 3 + 1];
                uint32_t pos = 0;
                for (uint32_t i = 0; i < dump_len && (pos + 3) < sizeof(hex_line); i++)
                {
                    pos += rt_snprintf(&hex_line[pos], sizeof(hex_line) - pos, "%02x ", resp_buf[i]);
                }
                if (pos > 0 && pos < sizeof(hex_line)) hex_line[pos - 1] = '\0';
                RPSH_DBG("rpsh resp head(%u): %s", dump_len, hex_line);
            }
            send_resp(s, sf->seq, resp_buf, resp_len, 0);
        }
        /* Keep shell responsive even under heavy rpmsg traffic. */
        rt_thread_mdelay(1);
    }
}

int rpmsg_shell_server_init(void) {
    /* Disable ulog async output globally for stability.
     * The observed crash happens inside ulog async output path
     * (ulog_output -> rt_sem_release -> rt_susp_list_dequeue) when
     * scheduler is temporarily locked.
     */
#ifdef ULOG_USING_ASYNC_OUTPUT
    ulog_async_output_enabled(RT_FALSE);
#endif

    /* Register memory console once */
    rt_memset(&s_mem_console_dev, 0, sizeof(s_mem_console_dev));
    s_mem_console_dev.type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    s_mem_console_dev.ops  = &s_mem_console_ops;
#else
    s_mem_console_dev.init   = mem_console_init;
    s_mem_console_dev.open   = mem_console_open;
    s_mem_console_dev.close  = mem_console_close;
    s_mem_console_dev.read   = mem_console_read;
    s_mem_console_dev.write  = mem_console_write;
    s_mem_console_dev.control = RT_NULL;
#endif
    rt_device_register(&s_mem_console_dev, RPSH_MEM_CONSOLE_DEV_NAME, RT_DEVICE_FLAG_RDWR);

    rt_mutex_init(&g_exec_lock, "rpsh_exec", RT_IPC_FLAG_FIFO);

#ifdef RT_USING_ULOG
    rt_memset(&g_rpsh_ulog_be, 0, sizeof(g_rpsh_ulog_be));
    g_rpsh_ulog_be.output = rpsh_ulog_output;
    ulog_backend_register(&g_rpsh_ulog_be, "rpsh_cap", RT_FALSE);
#endif

    {
        rt_device_t console = rt_console_get_device();
        if (console)
            g_default_console_name = console->parent.name;
    }
#ifdef RT_CONSOLE_DEVICE_NAME
    g_default_console_name = RT_CONSOLE_DEVICE_NAME;
#endif
    /* Do not touch rt_console / posix stdio here: INIT_APP_EXPORT may run
     * before or in parallel with tshell; restore_console_paths breaks serial. */

    /* Run below tshell priority to avoid starving UART shell input. */
    rt_thread_t t = rt_thread_create("sh_srv", shell_server_entry, NULL, 12288, 24, 20);
    if (t)
    {
#if defined(RT_USING_SMP) && defined(BSP_RPMSG_NET_BIND_CPU0)
        {
            int cpu = BSP_RPMSG_NET_CPU;

            if (cpu < 0)
            {
                cpu = 0;
            }
#if defined(RT_CPUS_NR)
            if (cpu >= (int)RT_CPUS_NR)
            {
                cpu = (int)RT_CPUS_NR - 1;
            }
#endif
            (void)rt_thread_control(t, RT_THREAD_CTRL_BIND_CPU, (void *)(rt_size_t)cpu);
        }
#endif
        rt_thread_startup(t);
    }
    return 0;
}
INIT_APP_EXPORT(rpmsg_shell_server_init);
