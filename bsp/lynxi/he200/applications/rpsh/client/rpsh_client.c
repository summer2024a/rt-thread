#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>
#include "rpsh_proto.h"

/* No valid server frame (CRC+session) for this long => assume server/link dead */
#define RPSH_SERVER_DEAD_SEC (HB_INTERVAL_SEC * 4 + 2)

#ifndef RPSH_CLIENT_RX_DIAG
#define RPSH_CLIENT_RX_DIAG 0
#endif

static int sock;
static int if_idx;
/* Device MAC (embedded rpnet0 side).
 * Default matches rpmsg-net logs: 02:12:20:11:34:00
 */
static uint8_t dev_mac[6] = {0x02,0x12,0x20,0x11,0x34,0x00};
static uint8_t host_mac[6] = {0};
static uint16_t g_seq = 1;
static uint8_t g_sid = INVALID_SESS_ID;
static pthread_t hb_tid;
static int running = 1;
static volatile sig_atomic_t g_sigint_handled = 0;
static struct termios old_tio;
static int need_resp_leading_newline = 0;
static int at_line_start = 1;
static int remote_last_was_cr = 0;
static uint8_t resp_agg_buf[8192];
static uint16_t resp_agg_off = 0;
static uint16_t resp_agg_total = 0;
static const char *g_prompt = "rpsh> ";
/* Async ulog push from device; disable with --no-async-log */
static int g_async_log_enabled = 1;
static time_t g_last_server_rx_sec;
static int g_dead_timeout;

static void mark_server_rx(void)
{
    g_last_server_rx_sec = time(NULL);
}

/* line-buffer to make host behave like a normal terminal */
static char line_buf[1024];
static size_t line_len = 0;

static void ui_redraw_input_line(void);

/* Local command history (host-side; same session as finsh history on device) */
#define RPSH_HIST_MAX 32
static char hist_buf[RPSH_HIST_MAX][sizeof(line_buf)];
static int hist_count;           /* 0 .. RPSH_HIST_MAX */
static int hist_browse = -1;     /* -1 = editing current line; else index 0=oldest */
static char line_draft[sizeof(line_buf)];
static size_t line_draft_len;

static void hist_push(const char *line)
{
    size_t n;

    if (!line)
        return;
    n = strlen(line);
    if (n == 0)
        return;
    if (hist_count > 0 && strcmp(hist_buf[hist_count - 1], line) == 0)
        return;
    if (hist_count < RPSH_HIST_MAX)
    {
        strncpy(hist_buf[hist_count], line, sizeof(hist_buf[0]) - 1);
        hist_buf[hist_count][sizeof(hist_buf[0]) - 1] = '\0';
        hist_count++;
    }
    else
    {
        memmove(hist_buf[0], hist_buf[1], sizeof(hist_buf[0]) * (RPSH_HIST_MAX - 1));
        strncpy(hist_buf[RPSH_HIST_MAX - 1], line, sizeof(hist_buf[0]) - 1);
        hist_buf[RPSH_HIST_MAX - 1][sizeof(hist_buf[0]) - 1] = '\0';
    }
}

static void hist_apply(int idx)
{
    strncpy(line_buf, hist_buf[idx], sizeof(line_buf) - 1);
    line_buf[sizeof(line_buf) - 1] = '\0';
    line_len = strlen(line_buf);
}

static void hist_up(void)
{
    if (hist_count <= 0)
        return;
    if (hist_browse < 0)
    {
        memcpy(line_draft, line_buf, line_len + 1);
        line_draft_len = line_len;
        hist_browse = hist_count - 1;
    }
    else if (hist_browse > 0)
        hist_browse--;
    else
        return;
    hist_apply(hist_browse);
    ui_redraw_input_line();
}

static void hist_down(void)
{
    if (hist_browse < 0)
        return;
    if (hist_browse < hist_count - 1)
    {
        hist_browse++;
        hist_apply(hist_browse);
    }
    else
    {
        memcpy(line_buf, line_draft, line_draft_len + 1);
        line_len = line_draft_len;
        hist_browse = -1;
    }
    ui_redraw_input_line();
}

/* After ESC [, consume CSI parameters until final byte.
 * Handles plain ESC [ A / ESC [ B and extended forms e.g. ESC [ 1 ; 5 A (modifier arrows). */
static void read_csi_hist_nav(void)
{
    char ch;
    int i;

    for (i = 0; i < 32; i++)
    {
        if (read(0, &ch, 1) != 1)
            return;
        if (ch >= '0' && ch <= '9')
            continue;
        if (ch == ';' || ch == ':' || ch == ' ')
            continue;
        if (ch == '?' || ch == '>' || ch == '<')
            continue;
        if (ch == 'A' || ch == 'a')
        {
            hist_up();
            return;
        }
        if (ch == 'B' || ch == 'b')
        {
            hist_down();
            return;
        }
        /* Other CSI finals (~, C–F, h/l, …): ignore */
        return;
    }
}

static void restore_tty(void) {
    tcsetattr(0, TCSANOW, &old_tio);
}

static void sigint(int s) {
    (void)s;
    if (g_sigint_handled) return;
    g_sigint_handled = 1;
    running = 0;
    if (sock > 0)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
    }
}

static uint16_t crc16(const uint8_t *d, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return 0;
}

static void get_iface_mac_fallback_sysfs(const char *ifname, uint8_t mac[6]) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return;
    }
    fclose(f);

    /* buf looks like "aa:bb:cc:dd:ee:ff\n" */
    if (parse_mac(buf, mac) != 0) {
        memset(mac, 0, 6);
    }
}

static void ui_write_bytes(const char *buf, size_t len)
{
    if (len == 0) return;
    write(1, buf, len);
}

static void ui_local_putc(char c)
{
    ui_write_bytes(&c, 1);
    if (c == '\n') at_line_start = 1;
    else at_line_start = 0;
}

static void ui_local_newline(void)
{
    const char crlf[] = "\r\n";
    ui_write_bytes(crlf, sizeof(crlf) - 1);
    at_line_start = 1;
}

static void ui_local_backspace(void)
{
    const char bs_seq[] = "\b \b";
    ui_write_bytes(bs_seq, sizeof(bs_seq) - 1);
    at_line_start = 0;
}

static void ui_show_prompt(void)
{
    if (at_line_start)
    {
        ui_write_bytes(g_prompt, strlen(g_prompt));
        at_line_start = 0;
    }
}

static void ui_redraw_input_line(void)
{
    /* Clear current line (erase in line) then redraw prompt + input */
    ui_write_bytes("\r\033[2K", 5);
    at_line_start = 1;
    ui_show_prompt();
    if (line_len > 0)
    {
        ui_write_bytes(line_buf, line_len);
        at_line_start = 0;
    }
}

/* Normalize remote output so '\n' becomes '\r\n' for cleaner terminal boundaries. */
static void ui_remote_write(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return;

    for (uint16_t i = 0; i < len; i++)
    {
        char c = (char)data[i];
        if (c == '\r')
        {
            const char crlf[] = "\r\n";
            ui_write_bytes(crlf, sizeof(crlf) - 1);
            at_line_start = 1;
            remote_last_was_cr = 1;
        }
        else if (c == '\n')
        {
            /* collapse CRLF as a single newline */
            if (!remote_last_was_cr)
            {
                const char crlf[] = "\r\n";
                ui_write_bytes(crlf, sizeof(crlf) - 1);
                at_line_start = 1;
            }
            remote_last_was_cr = 0;
        }
        else
        {
            ui_write_bytes(&c, 1);
            at_line_start = 0;
            remote_last_was_cr = 0;
        }
    }
}

static void resp_agg_reset(void)
{
    resp_agg_off = 0;
    resp_agg_total = 0;
}

/* Strip device-appended tab footer; fill line_buf for local redraw */
static int rpsh_peel_line_footer(uint8_t *buf, uint16_t *plen, char *line_out, size_t line_out_sz)
{
    static const uint8_t m1[] = {0xff, 'R', 'P', 'S', 'H', '_', 'L', 'I', 'N', 'E', '\n'};
    static const uint8_t m2[] = {'\n', 0xff, 'R', 'P', 'S', 'H', '_', 'E', 'N', 'D', '\n'};
    uint16_t n = *plen;
    size_t i;
    uint8_t *start = NULL;

    if (n < sizeof(m1))
        return 0;
    for (i = 0; i + sizeof(m1) <= (size_t)n; i++)
    {
        if (memcmp(buf + i, m1, sizeof(m1)) == 0)
        {
            start = buf + i;
            break;
        }
    }
    if (!start)
        return 0;
    {
        uint8_t *line_start = start + sizeof(m1);
        uint8_t *end = NULL;
        size_t j;

        for (j = 0; line_start + j + sizeof(m2) <= buf + n; j++)
        {
            if (memcmp(line_start + j, m2, sizeof(m2)) == 0)
            {
                end = line_start + j;
                break;
            }
        }
        if (!end)
            return 0;
        {
            size_t llen = (size_t)(end - line_start);

            if (llen >= line_out_sz)
                llen = line_out_sz - 1;
            memcpy(line_out, line_start, llen);
            line_out[llen] = '\0';
        }
        {
            uint8_t *seg_end = end + sizeof(m2);
            size_t tail = (size_t)((buf + n) - seg_end);

            memmove(start, seg_end, tail);
            *plen = (uint16_t)(*plen - (size_t)(seg_end - start));
        }
    }
    return 1;
}

static void resp_agg_append(const uint8_t *data, uint16_t len, uint16_t total)
{
    if (resp_agg_off == 0) resp_agg_total = total;
    if (resp_agg_total == 0) resp_agg_total = total;

    if (resp_agg_off + len > sizeof(resp_agg_buf))
    {
        /* Overflow guard: flush partial safely, then reset */
        if (resp_agg_off > 0) ui_remote_write(resp_agg_buf, resp_agg_off);
        resp_agg_reset();
        return;
    }

    memcpy(resp_agg_buf + resp_agg_off, data, len);
    resp_agg_off += len;
}

static void send_pkt(uint8_t type, uint8_t flags, uint16_t seq,
                     const uint8_t *data, uint16_t dlen, uint16_t total) {
    uint8_t pkt[1520] = {0};
    struct ethhdr *eh = (void*)pkt;
    shell_frame_t *sf = (void*)(eh + 1);
    memcpy(eh->h_dest, dev_mac, 6);
    /* Server learns client's MAC from ethhdr->h_source */
    memcpy(eh->h_source, host_mac, 6);
    eh->h_proto = htons(SHELL_ETH_TYPE);
    sf->type = type;
    sf->flags = flags;
    sf->session_id = g_sid;
    sf->seq = seq;
    sf->data_len = dlen;
    sf->total_len = total;
    if (dlen) memcpy(sf->data, data, dlen);
    /* CRC is computed with crc field forced to 0 */
    sf->crc = 0;
    sf->crc = crc16((uint8_t*)sf, sizeof(*sf));
    struct sockaddr_ll sll = {0};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_idx;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, dev_mac, 6);
    sendto(sock, pkt, sizeof(*eh) + sizeof(*sf), 0, (struct sockaddr *)&sll, sizeof(sll));
}

static void *hb(void *arg) {
    (void)arg;
    while (running) {
        send_pkt(FRAME_TYPE_HEARTBEAT, 0, g_seq++, NULL, 0, 0);
        for (int i = 0; i < HB_INTERVAL_SEC * 20 && running; i++) {
            usleep(50000); /* 50ms, faster Ctrl+C responsiveness */
        }
    }
    return NULL;
}

static int wait_ack(uint16_t seq) {
    uint8_t buf[1520];
    uint64_t rx_cnt = 0;
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {0, 200000}; /* 200ms */
        int sel = select(sock + 1, &fds, NULL, NULL, &tv);
        if (!running) return -1;
        if (sel <= 0) continue;

        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) continue;
        rx_cnt++;
        struct ethhdr *eh = (void*)buf;
        if (ntohs(eh->h_proto) != SHELL_ETH_TYPE) continue;
        shell_frame_t *sf = (void*)(eh + 1);

        uint16_t rx_crc = sf->crc;
        sf->crc = 0;
        uint16_t calc_crc = crc16((uint8_t*)sf, sizeof(*sf));
        sf->crc = rx_crc;
#if RPSH_CLIENT_RX_DIAG
        /* Optional RX diag for troubleshooting only. */
        printf("[rx%lu] eth dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x | type=%u seq=%u sid=%u flags=%u dlen=%u total=%u crc_calc=0x%04x crc_rx=0x%04x%s\n",
               rx_cnt,
               eh->h_dest[0],eh->h_dest[1],eh->h_dest[2],eh->h_dest[3],eh->h_dest[4],eh->h_dest[5],
               eh->h_source[0],eh->h_source[1],eh->h_source[2],eh->h_source[3],eh->h_source[4],eh->h_source[5],
               sf->type, sf->seq, sf->session_id, sf->flags, sf->data_len, sf->total_len,
               calc_crc, rx_crc, (calc_crc == rx_crc) ? "" : " CRC_MISMATCH");
        fflush(stdout);
#endif

        if (calc_crc != rx_crc) continue;
        if (sf->type == FRAME_TYPE_ACK && sf->seq == seq) {
            write(1, sf->data, sf->data_len);
            printf("\n");
            at_line_start = 1;
            g_sid = sf->session_id;
            mark_server_rx();
            return 0;
        }
    }
}

static void raw_tty(void) {
    tcgetattr(0, &old_tio);
    struct termios tio = old_tio;
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &tio);
}

int main(int argc, char **argv) {
    const char *ifname = NULL;
    const char *dev_mac_opt = NULL;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--no-async-log") == 0)
        {
            g_async_log_enabled = 0;
            continue;
        }
        if (ifname == NULL)
            ifname = argv[i];
        else if (dev_mac_opt == NULL)
            dev_mac_opt = argv[i];
        else
        {
            fprintf(stderr, "usage: %s [--no-async-log] <ifname> [device_mac]\n", argv[0]);
            fprintf(stderr, "  --no-async-log  suppress live ulog push (command RESP still works)\n");
            return 1;
        }
    }
    if (ifname == NULL)
    {
        fprintf(stderr, "usage: %s [--no-async-log] <ifname> [device_mac]\n", argv[0]);
        fprintf(stderr, "  example: %s eth0 02:12:20:11:34:00\n", argv[0]);
        return 1;
    }
    signal(SIGINT, sigint);

    sock = socket(AF_PACKET, SOCK_RAW, htons(SHELL_ETH_TYPE));
    if (sock < 0) { perror("sock"); return 1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);
    ioctl(sock, SIOCGIFINDEX, &ifr);
    if_idx = ifr.ifr_ifindex;

    /* Get local interface MAC address for ethhdr->h_source */
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(host_mac, ifr.ifr_hwaddr.sa_data, 6);
    else
    {
        /* Fallback: avoid sending replies to an all-zero MAC */
        memset(host_mac, 0, sizeof(host_mac));
        get_iface_mac_fallback_sysfs(ifname, host_mac);
    }

    printf("host_mac=%02x:%02x:%02x:%02x:%02x:%02x dev_mac=%02x:%02x:%02x:%02x:%02x:%02x async_log=%d\n",
           host_mac[0],host_mac[1],host_mac[2],host_mac[3],host_mac[4],host_mac[5],
           dev_mac[0],dev_mac[1],dev_mac[2],dev_mac[3],dev_mac[4],dev_mac[5],
           g_async_log_enabled);

    if (dev_mac_opt != NULL)
    {
        if (parse_mac(dev_mac_opt, dev_mac) != 0)
        {
            fprintf(stderr, "invalid device_mac: %s\n", dev_mac_opt);
            return 1;
        }
    }

    printf("login (admin/admin123 or user/user123; or admin123/user123): ");
    char pwd[32];
    fgets(pwd, sizeof(pwd), stdin);
    int l = strlen(pwd);
    /* Strip line endings: '\n' and optional '\r' (e.g. Windows CRLF) */
    while (l > 0 && (pwd[l-1] == '\n' || pwd[l-1] == '\r')) {
        pwd[--l] = 0;
    }

    /* Accept "admin/admin123" style and keep only password part */
    {
        char *slash = strchr(pwd, '/');
        if (slash && slash[1] != '\0') {
            int tail_len = (int)strlen(slash + 1);
            if (tail_len > (int)sizeof(pwd) - 1) tail_len = (int)sizeof(pwd) - 1;
            memmove(pwd, slash + 1, tail_len);
            pwd[tail_len] = '\0';
            l = tail_len;
        }
    }

    /* Accept "admin" and "user" as shortcuts */
    if (strcmp(pwd, "admin") == 0) {
        strcpy(pwd, "admin123");
        l = (int)strlen(pwd);
    }
    else if (strcmp(pwd, "user") == 0) {
        strcpy(pwd, "user123");
        l = (int)strlen(pwd);
    }

    printf("login pwd='%s' len=%d host_mac=%02x:%02x:%02x:%02x:%02x:%02x dev_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           pwd, l,
           host_mac[0],host_mac[1],host_mac[2],host_mac[3],host_mac[4],host_mac[5],
           dev_mac[0],dev_mac[1],dev_mac[2],dev_mac[3],dev_mac[4],dev_mac[5]);

    send_pkt(FRAME_TYPE_LOGIN, 0, g_seq++, (uint8_t*)pwd, l, l);
    if (wait_ack(g_seq-1) < 0) { printf("fail\n"); close(sock); return 1; }

    pthread_create(&hb_tid, NULL, hb, NULL);
    raw_tty();
    ui_show_prompt();

    uint8_t buf[2048];
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        FD_SET(sock, &fds);
        struct timeval tv = {1,0};
        select(sock+1, &fds, NULL, NULL, &tv);

        if (FD_ISSET(0, &fds)) {
            char c;
            if (read(0, &c, 1) <= 0) break;

            /* Normalize enter */
            if (c == '\r' || c == '\n') {
                /* Echo newline locally */
                ui_local_newline();
                hist_browse = -1;

                /* Send command only when line is not empty */
                if (line_len > 0) {
                    hist_push(line_buf);
                    send_pkt(FRAME_TYPE_CMD,
                             PACKET_FLAG_LAST,
                             g_seq++,
                             (const uint8_t *)line_buf,
                             (uint16_t)line_len,
                             (uint16_t)line_len);
                    need_resp_leading_newline = 1;
                }
                else
                {
                    ui_show_prompt();
                }

                line_len = 0;
                memset(line_buf, 0, sizeof(line_buf));
            }
            else if (c == 0x7f || c == 0x08) { /* backspace */
                if (line_len > 0) {
                    line_len--;
                    ui_local_backspace();
                }
            }
            else if ((unsigned char)c == 0x1b) {
                char s2, s3;

                if (read(0, &s2, 1) != 1)
                    continue;
                if (s2 == '[')
                    read_csi_hist_nav();
                else if (s2 == 'O')
                {
                    if (read(0, &s3, 1) != 1)
                        continue;
                    if (s3 == 'A' || s3 == 'a')
                        hist_up();
                    else if (s3 == 'B' || s3 == 'b')
                        hist_down();
                }
            }
            else if (c == '\t') {
                /* Tab: send line + \t; device runs msh_auto_complete + FinSH prompt */
                if (line_len + 1 >= sizeof(line_buf))
                    continue;
                line_buf[line_len] = '\t';
                send_pkt(FRAME_TYPE_CMD,
                         PACKET_FLAG_LAST,
                         g_seq++,
                         (const uint8_t *)line_buf,
                         (uint16_t)(line_len + 1),
                         (uint16_t)(line_len + 1));
                line_buf[line_len] = '\0';
                need_resp_leading_newline = 1;
            }
            else {
                if (line_len < sizeof(line_buf) - 1) {
                    line_buf[line_len++] = c;
                    /* Echo character */
                    ui_local_putc(c);
                }
            }
        }
        if (FD_ISSET(sock, &fds)) {
            int r = recv(sock, buf, sizeof(buf), 0);
            if (r <= 0) continue;
            struct ethhdr *eh = (void*)buf;
            if (ntohs(eh->h_proto) != SHELL_ETH_TYPE) continue;
            shell_frame_t *sf = (void*)(eh + 1);
            uint16_t rx_crc = sf->crc;
            sf->crc = 0;
            if (crc16((uint8_t *)sf, sizeof(*sf)) != rx_crc) continue;
            sf->crc = rx_crc;
            if (sf->session_id != g_sid) continue;
            mark_server_rx();
            if (sf->type == FRAME_TYPE_ACK) {
                /* Heartbeat / logout ACK — no terminal output */
                continue;
            }
            /* Only print command output/resp to keep terminal clean */
            if (sf->type == FRAME_TYPE_RESP) {
                if (sf->flags & PACKET_FLAG_ASYNC) {
                    if (g_async_log_enabled)
                    {
                        if (!at_line_start) ui_local_newline();
                        ui_remote_write(sf->data, sf->data_len);
                        if (!at_line_start) ui_local_newline();
                        ui_redraw_input_line();
                    }
                    continue;
                }
                if (need_resp_leading_newline && !at_line_start) {
                    ui_local_newline();
                    need_resp_leading_newline = 0;
                }
                resp_agg_append(sf->data, sf->data_len, sf->total_len);

                if ((sf->flags & PACKET_FLAG_MORE) == 0) {
                    /* LAST frame: flush one complete response */
                    int tab_peeled = 0;

                    if (resp_agg_off > 0) {
                        tab_peeled = rpsh_peel_line_footer(resp_agg_buf, &resp_agg_off,
                                                             line_buf, sizeof(line_buf));
                        ui_remote_write(resp_agg_buf, resp_agg_off);
                        if (tab_peeled)
                            line_len = strlen(line_buf);
                    }
                    resp_agg_reset();
                    if (!at_line_start) ui_local_newline();
                    if (tab_peeled)
                        ui_redraw_input_line();
                    else
                        ui_show_prompt();
                }
            }
        }

        if (time(NULL) - g_last_server_rx_sec > RPSH_SERVER_DEAD_SEC) {
            ui_local_newline();
            fprintf(stderr, "[rpsh] server lost (no response for %d s)\r\n", RPSH_SERVER_DEAD_SEC);
            g_dead_timeout = 1;
            running = 0;
        }
    }

    restore_tty();
    running = 0;
    if (sock > 0)
    {
        if (!g_dead_timeout)
            send_pkt(FRAME_TYPE_LOGOUT, 0, g_seq++, NULL, 0, 0);
    }
    pthread_join(hb_tid, NULL);
    if (sock > 0) close(sock);
    return g_dead_timeout ? 1 : 0;
}
