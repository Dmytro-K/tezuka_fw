// SPDX-License-Identifier: MIT
/*
 * iqnet_rx - host-side test receiver for the iqnet UDP IQ stream.
 *
 * Binds a UDP socket, asks the board (iqnetd, TCP control port 30433) to
 * START streaming to it, receives with recvmmsg(), validates the iqnet
 * header, tracks lost / reordered / duplicated datagrams, prints a
 * per-second report and, for CS12, checks the PL sync bursts (magic +
 * 32-bit counter every 262144 samples) using absolute stream offsets.
 * Offset jumps without a seq gap are board-side loss (DMA overflow, dropped
 * short blocks); seq == 0 && offset == 0 while a stream is active is a
 * restart of the stream on the board (state is reset, restarts counted).
 * At the end (timeout or Ctrl+C) it sends STATS and STOP.
 *
 * Linux only, no dependencies. See README.md.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "iqnet_proto.h"

_Static_assert(sizeof(struct iqnet_hdr) == IQNET_HDR_LEN, "iqnet_hdr size");

#define BATCH 64
#define DGRAM_BUF (IQNET_HDR_LEN + IQNET_MAX_PAYLOAD) /* jumbo (path=pl); larger => MSG_TRUNC */
#define SEEN_BITS (1u << 16) /* duplicate-detection window, in datagrams */
#define DUMP_BUF (4u << 20)

/* CS12 sync burst: 20 magic bytes + u32 LE counter, every 262144 samples */
static const uint8_t CS12_MAGIC[20] = {0xA1, 0x5C, 0x1E, 0xAB, 0xD2, 0xC5, 0xEF, 0x12, 0x37, 0x9A,
                                       0x4D, 0x6B, 0xE1, 0xF0, 0x8A, 0x3C, 0x56, 0x7D, 0x91, 0x24};
#define CS12_SYNC_SAMPLES 262144u
#define CS12_SYNC_PERIOD ((uint64_t)CS12_SYNC_SAMPLES / 8u * IQNET_BURST) /* 786432 B */

/* ------------------------------------------------------------------ */
/* options                                                             */

struct opts
{
    const char *host;
    unsigned udp_port;
    unsigned ctrl_port;
    const char *mode;
    const char *path; /* NULL = not sent (board default: kernel) */
    long payload;     /* -1 = not sent */
    unsigned bytes_per_iq;
    int cs12;
    double seconds; /* 0 = until Ctrl+C */
    long gso;       /* -1 = not sent */
    long blocks;
    long block_size;
    const char *dump;
    unsigned rcvbuf_mb;
    int verbose;
};

static struct opts o = {
    .host = "10.10.11.20",
    .udp_port = 30432,
    .ctrl_port = IQNET_CTRL_PORT,
    .mode = "cs12",
    .path = NULL,
    .payload = -1,
    .bytes_per_iq = 3,
    .cs12 = 1,
    .seconds = 10,
    .gso = -1,
    .blocks = -1,
    .block_size = -1,
    .dump = NULL,
    .rcvbuf_mb = 128,
    .verbose = 0,
};

/* ------------------------------------------------------------------ */
/* receive state                                                       */

struct counters
{
    uint64_t dgrams;    /* valid, unique datagrams */
    uint64_t bytes;     /* payload bytes of those */
    int64_t lost;       /* seq gaps minus late arrivals that filled them */
    uint64_t reordered; /* late (seq < expected) but not seen before */
    uint64_t dups;      /* seq seen before (within SEEN_BITS window) */
    uint64_t bad_magic;
    uint64_t bad_len;    /* payload != negotiated payload_len */
    uint64_t runt;       /* shorter than the header */
    uint64_t trunc;      /* larger than DGRAM_BUF */
    uint64_t foreign;    /* not from the board address */
    uint64_t offset_err; /* header offset behind the one implied by seq */
    uint64_t board_gaps; /* forward offset jumps beyond the seq gap (board-side loss) */
    uint64_t board_lost; /* bytes skipped by those jumps */
    uint64_t restarts;   /* seq == 0 && offset == 0 while a stream was active */
    /* CS12 sync */
    uint64_t sync_found;
    uint64_t sync_ok;
    uint64_t sync_counter_err;
    uint64_t sync_period_err;
    uint64_t sync_missing;   /* expected sync position received, no magic */
    uint64_t sync_phase_chg; /* magic found at a different offset mod 24 */
    uint64_t sync_unchecked; /* loss since previous sync, or late datagram */
};

static struct counters c;

static unsigned payload_len = IQNET_DEFAULT_PAYLOAD; /* from OK reply */
static unsigned block_size_reply;

static int have_seq; /* at least one valid datagram */
static uint32_t expected_seq;
static uint64_t expected_off;
static uint64_t gap_events; /* forward seq jumps + board offset jumps + restarts */
static uint64_t seen[SEEN_BITS / 64];
static uint32_t max_rxq_ovfl; /* SO_RXQ_OVFL: socket drops (host side) */

/* CS12 sync tracker */
static int sync_have_last;
static uint64_t sync_last_off;
static uint32_t sync_last_counter;
static uint64_t sync_last_gap_events;
static int sync_missing_flagged;
static int sync_phase_known;
static unsigned sync_phase;
/* last bytes of the previous in-order datagram, for sync bursts that
 * straddle two datagrams (only when the burst phase is not 0) */
static uint8_t tail[IQNET_BURST - 1];
static unsigned tail_len;
static uint64_t tail_end;
static int tail_valid;

/* raw payload dump */
static int dump_fd = -1;
static uint8_t *dump_buf;
static size_t dump_fill;
static uint64_t dump_base;
static uint64_t dump_stream_base; /* file offset of stream offset 0 (moves on restart) */
static uint64_t stream_end;       /* highest stream offset + 1 of the current stream */

/* board STATS reply, parsed tolerantly (key=value, unknown keys kept) */
#define STATS_MAX 32
struct stats_kv
{
    char key[32];
    uint64_t val;
    char str[16]; /* non-numeric value (e.g. path=pl), "" for numbers */
};
static struct stats_kv board_stats[STATS_MAX];
static unsigned board_stats_n;
static int board_stats_ok;

static volatile sig_atomic_t stop_req;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void on_signal(int sig)
{
    (void)sig;
    if (stop_req)
        _exit(130); /* second Ctrl+C: closing TCP stops the stream anyway */
    stop_req = 1;
}

static void vlog(const char *fmt, ...)
{
    va_list ap;

    if (!o.verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* dump file: file offset == stream offset (gaps stay as sparse zeros)  */

static void dump_flush(void)
{
    size_t done = 0;

    while (dump_fd >= 0 && done < dump_fill)
    {
        ssize_t r = pwrite(dump_fd, dump_buf + done, dump_fill - done, (off_t)(dump_base + done));
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "dump: pwrite: %s (dump disabled)\n", strerror(errno));
            close(dump_fd);
            dump_fd = -1;
            break;
        }
        done += (size_t)r;
    }
    dump_fill = 0;
}

static void dump_put(uint64_t off, const uint8_t *p, size_t len)
{
    if (dump_fd < 0)
        return;
    off += dump_stream_base;
    if (dump_fill && (off != dump_base + dump_fill || dump_fill + len > DUMP_BUF))
        dump_flush();
    if (dump_fd < 0)
        return;
    if (!dump_fill)
        dump_base = off;
    memcpy(dump_buf + dump_fill, p, len);
    dump_fill += len;
}

/* ------------------------------------------------------------------ */
/* CS12 sync check                                                     */

static void sync_hit(uint64_t abs, uint32_t counter, int in_order)
{
    unsigned ph = (unsigned)(abs % IQNET_BURST);

    c.sync_found++;
    if (!sync_phase_known)
    {
        sync_phase_known = 1;
        sync_phase = ph;
        printf("cs12: first sync burst at offset %" PRIu64 " (burst phase %u), counter %" PRIu32
               "\n",
               abs, ph, counter);
    }
    else if (ph != sync_phase)
    {
        c.sync_phase_chg++;
        fprintf(stderr, "cs12: sync burst phase changed %u -> %u at offset %" PRIu64 "\n",
                sync_phase, ph, abs);
        sync_phase = ph;
    }

    if (!in_order)
    {
        /* older than the last sync we tracked: count, do not check */
        c.sync_unchecked++;
        return;
    }

    if (sync_have_last)
    {
        if (gap_events != sync_last_gap_events)
        {
            c.sync_unchecked++;
        }
        else
        {
            uint64_t dist = abs - sync_last_off;
            int bad = 0;

            if (counter != sync_last_counter + 1)
            {
                c.sync_counter_err++;
                bad = 1;
            }
            if (dist != CS12_SYNC_PERIOD)
            {
                c.sync_period_err++;
                bad = 1;
            }
            if (bad)
                fprintf(stderr,
                        "cs12: sync error at offset %" PRIu64 ": counter %" PRIu32 " -> %" PRIu32
                        " (delta %" PRId64 "), distance %" PRIu64 " B (expected %" PRIu64
                        ") with no datagram lost\n",
                        abs, sync_last_counter, counter,
                        (int64_t)counter - (int64_t)sync_last_counter, dist, CS12_SYNC_PERIOD);
            else
                c.sync_ok++;
        }
    }
    sync_have_last = 1;
    sync_last_off = abs;
    sync_last_counter = counter;
    sync_last_gap_events = gap_events;
    sync_missing_flagged = 0;
}

static void sync_scan(const uint8_t *p, unsigned len, uint64_t off, int in_order)
{
    unsigned pos = 0;

    /* burst that started in the previous datagram and ends in this one */
    if (in_order && tail_valid && tail_end == off)
    {
        uint8_t w[2 * (IQNET_BURST - 1)];
        unsigned head = len < IQNET_BURST - 1 ? len : IQNET_BURST - 1;
        unsigned wl = tail_len + head;

        memcpy(w, tail, tail_len);
        memcpy(w + tail_len, p, head);
        for (unsigned s = 0; s < tail_len; s++)
        {
            if (s + IQNET_BURST <= tail_len || s + IQNET_BURST > wl)
                continue;
            if (memcmp(w + s, CS12_MAGIC, sizeof(CS12_MAGIC)) == 0)
            {
                uint32_t cnt;

                memcpy(&cnt, w + s + 20, 4);
                sync_hit(off - tail_len + s, le32toh(cnt), 1);
            }
        }
    }

    while (pos + IQNET_BURST <= len)
    {
        const uint8_t *m = memmem(p + pos, len - pos, CS12_MAGIC, sizeof(CS12_MAGIC));
        unsigned mp;
        uint32_t cnt;

        if (!m)
            break;
        mp = (unsigned)(m - p);
        if (mp + IQNET_BURST > len)
            break; /* counter in the next datagram: straddle path */
        memcpy(&cnt, m + 20, 4);
        sync_hit(off + mp, le32toh(cnt), in_order);
        pos = mp + sizeof(CS12_MAGIC);
    }

    if (!in_order)
        return;

    /* the next sync position is fully received, gap-free, but no magic */
    if (sync_have_last && !sync_missing_flagged && gap_events == sync_last_gap_events &&
        off + len >= sync_last_off + CS12_SYNC_PERIOD + IQNET_BURST)
    {
        c.sync_missing++;
        sync_missing_flagged = 1;
        fprintf(stderr,
                "cs12: no sync burst at expected offset %" PRIu64
                " (no datagram lost): alignment lost\n",
                sync_last_off + CS12_SYNC_PERIOD);
    }

    tail_len = len < IQNET_BURST - 1 ? len : IQNET_BURST - 1;
    memcpy(tail, p + len - tail_len, tail_len);
    tail_end = off + len;
    tail_valid = 1;
}

/* ------------------------------------------------------------------ */
/* datagram processing                                                 */

static inline int seen_test(uint32_t s)
{
    return (seen[(s % SEEN_BITS) / 64] >> (s % 64)) & 1;
}

static inline void seen_set(uint32_t s)
{
    seen[(s % SEEN_BITS) / 64] |= 1ull << (s % 64);
}

static inline void seen_clear(uint32_t s)
{
    seen[(s % SEEN_BITS) / 64] &= ~(1ull << (s % 64));
}

/* seq == 0 && offset == 0 while a stream is active: the board started a new
 * stream. Keep the counters, reset the sequence / sync / tail state. */
static void stream_restart(void)
{
    c.restarts++;
    gap_events++;
    fprintf(stderr,
            "stream: restart (seq 0, offset 0) after seq %" PRIu32 ", offset %" PRIu64 "\n",
            expected_seq - 1, expected_off);
    expected_seq = 0;
    expected_off = 0;
    memset(seen, 0, sizeof(seen));
    sync_have_last = 0;
    sync_missing_flagged = 0;
    sync_phase_known = 0; /* the new stream may start at another burst phase */
    tail_valid = 0;
    /* dump: append the new stream behind the previous one */
    dump_flush();
    dump_stream_base += (stream_end + IQNET_BURST - 1) / IQNET_BURST * IQNET_BURST;
    stream_end = 0;
}

static void process(const uint8_t *buf, unsigned n)
{
    struct iqnet_hdr h;
    uint32_t seq;
    uint64_t off;
    unsigned len;
    int32_t d;
    int in_order;

    if (n < IQNET_HDR_LEN)
    {
        c.runt++;
        return;
    }
    memcpy(&h, buf, sizeof(h));
    if (le32toh(h.magic) != IQNET_MAGIC)
    {
        c.bad_magic++;
        return;
    }
    seq = le32toh(h.seq);
    off = le64toh(h.offset);
    len = n - IQNET_HDR_LEN;
    if (len != payload_len)
    {
        c.bad_len++;
        vlog("seq %" PRIu32 ": payload %u B, expected %u\n", seq, len, payload_len);
        if (len == 0 || len % IQNET_BURST)
            return;
    }

    if (have_seq && seq == 0 && off == 0)
        stream_restart();

    d = (int32_t)(seq - expected_seq);
    if (d >= 0)
    {
        uint64_t exp_off = expected_off + (uint64_t)d * payload_len;

        if (d > 0)
        {
            uint32_t nclr = (uint32_t)d < SEEN_BITS ? (uint32_t)d : SEEN_BITS;

            c.lost += d;
            gap_events++;
            for (uint32_t i = 0; i < nclr; i++)
                seen_clear(expected_seq + i);
            vlog("gap: seq %" PRIu32 " .. %" PRIu32 " missing (%" PRId32 ")\n", expected_seq,
                 seq - 1, d);
        }
        if (off > exp_off && (off - exp_off) % IQNET_BURST == 0)
        {
            /* board-side loss: DMA overflow or dropped short block */
            c.board_gaps++;
            c.board_lost += off - exp_off;
            gap_events++;
            vlog("seq %" PRIu32 ": board skipped %" PRIu64 " B at offset %" PRIu64 " (overflow)\n",
                 seq, off - exp_off, exp_off);
        }
        else if (off != exp_off)
        {
            c.offset_err++;
            vlog("seq %" PRIu32 ": offset %" PRIu64 ", expected %" PRIu64 "\n", seq, off, exp_off);
        }
        seen_set(seq);
        expected_seq = seq + 1;
        expected_off = off + len;
        have_seq = 1;
        in_order = 1;
        if (expected_off > stream_end)
            stream_end = expected_off;
    }
    else
    {
        if ((uint32_t)(-(int64_t)d) < SEEN_BITS && seen_test(seq))
        {
            c.dups++;
            return;
        }
        c.reordered++;
        if (c.lost > 0)
            c.lost--;
        if ((uint32_t)(-(int64_t)d) < SEEN_BITS)
            seen_set(seq);
        in_order = 0;
    }

    c.dgrams++;
    c.bytes += len;
    if (o.cs12)
        sync_scan(buf + IQNET_HDR_LEN, len, off, in_order);
    dump_put(off, buf + IQNET_HDR_LEN, len);
}

/* ------------------------------------------------------------------ */
/* TCP control                                                         */

static char ctrl_rx[1024];
static size_t ctrl_rx_len;

/* read one '\n'-terminated line; 1 = ok, 0 = closed, -1 = error/timeout */
static int ctrl_readline(int fd, char *line, size_t sz, int timeout_ms)
{
    double deadline = now_s() + timeout_ms / 1000.0;

    for (;;)
    {
        char *nl = memchr(ctrl_rx, '\n', ctrl_rx_len);

        if (nl)
        {
            size_t l = (size_t)(nl - ctrl_rx);
            size_t cp = l < sz - 1 ? l : sz - 1;

            memcpy(line, ctrl_rx, cp);
            line[cp] = 0;
            if (cp && line[cp - 1] == '\r')
                line[cp - 1] = 0;
            memmove(ctrl_rx, nl + 1, ctrl_rx_len - l - 1);
            ctrl_rx_len -= l + 1;
            return 1;
        }
        if (ctrl_rx_len == sizeof(ctrl_rx))
            ctrl_rx_len = 0; /* overlong garbage line: drop it */

        int left = (int)((deadline - now_s()) * 1000);
        struct pollfd pfd = {.fd = fd, .events = POLLIN};

        if (left <= 0)
        {
            fprintf(stderr, "control: timeout waiting for reply\n");
            return -1;
        }
        int pr = poll(&pfd, 1, left);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            perror("control: poll");
            return -1;
        }
        if (pr == 0)
            continue;
        ssize_t r = recv(fd, ctrl_rx + ctrl_rx_len, sizeof(ctrl_rx) - ctrl_rx_len, 0);
        if (r == 0)
            return 0;
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            perror("control: recv");
            return -1;
        }
        ctrl_rx_len += (size_t)r;
    }
}

static int ctrl_send(int fd, const char *cmd)
{
    size_t len = strlen(cmd), done = 0;

    vlog("control > %s", cmd);
    while (done < len)
    {
        ssize_t r = send(fd, cmd + done, len - done, MSG_NOSIGNAL);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            perror("control: send");
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

static int ctrl_cmd(int fd, const char *cmd, char *reply, size_t sz, int timeout_ms)
{
    int r;

    if (ctrl_send(fd, cmd))
        return -1;
    r = ctrl_readline(fd, reply, sz, timeout_ms);
    if (r == 0)
        fprintf(stderr, "control: connection closed by board\n");
    if (r <= 0)
        return -1;
    vlog("control < %s\n", reply);
    return 0;
}

/* "STATS key=value key=value ...": tolerant, unknown keys are kept, tokens
 * without '=' are ignored, non-numeric values (path=pl) are kept as strings.
 * 0 = parsed. */
static int parse_stats(const char *line)
{
    const char *p = line;

    board_stats_n = 0;
    board_stats_ok = 0;
    if (strncmp(p, "STATS", 5) != 0 || (p[5] != ' ' && p[5] != '\t' && p[5] != 0))
        return -1;
    p += 5;
    for (;;)
    {
        const char *tok, *eq, *end;
        struct stats_kv *kv = NULL;
        size_t kl, vl;
        char *e;
        uint64_t v;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        end = p;
        eq = memchr(tok, '=', (size_t)(end - tok));
        if (!eq || eq == tok || eq + 1 == end)
            continue;
        kl = (size_t)(eq - tok);
        vl = (size_t)(end - eq - 1);
        if (kl >= sizeof(kv->key) || board_stats_n == STATS_MAX)
            continue;
        kv = &board_stats[board_stats_n];
        memset(kv, 0, sizeof(*kv));
        errno = 0;
        v = strtoull(eq + 1, &e, 10);
        if (errno || e != end || eq[1] < '0' || eq[1] > '9')
        {
            if (vl >= sizeof(kv->str))
                continue;
            memcpy(kv->str, eq + 1, vl);
            kv->str[vl] = 0;
        }
        else
        {
            kv->val = v;
        }
        memcpy(kv->key, tok, kl);
        kv->key[kl] = 0;
        board_stats_n++;
    }
    board_stats_ok = 1;
    return 0;
}

static const struct stats_kv *stats_find(const char *key)
{
    for (unsigned i = 0; i < board_stats_n; i++)
    {
        if (!strcmp(board_stats[i].key, key))
            return &board_stats[i];
    }
    return NULL;
}

/* 1 = key present with a numeric value (value in *v) */
static int stats_get(const char *key, uint64_t *v)
{
    const struct stats_kv *kv = stats_find(key);

    if (!kv || kv->str[0])
        return 0;
    *v = kv->val;
    return 1;
}

/* value of key as text ("n/a" when missing); buf holds numeric values */
static const char *stats_str(const char *key, char *buf, size_t sz)
{
    const struct stats_kv *kv = stats_find(key);

    if (!kv)
        return "n/a";
    if (kv->str[0])
        return kv->str;
    snprintf(buf, sz, "%" PRIu64, kv->val);
    return buf;
}

static void print_stats(void)
{
    static const struct
    {
        const char *key;
        const char *desc;
    } known[] = {
        {"datagrams", "datagrams handed to UDP"},
        {"bytes", "IQ payload bytes handed to UDP"},
        {"blocks", "IIO blocks sent and returned"},
        {"send_errors", "sendmsg failures (datagrams dropped)"},
        {"zc_copied", "zero-copy completions that copied"},
        {"overflows", "board-side overflows (seen as offset jumps)"},
        {"short_blocks", "short/aborted blocks dropped"},
        {"copy_batches", "batches sent in copy mode"},
        {"inflight", "blocks in flight"},
        {"running", "stream running"},
        {"path", "streaming path (none | kernel | pl)"},
        {"linux_frames", "pl: GEM frames from Linux passed through"},
        {"linux_drops", "pl: GEM frames from Linux dropped (must be 0)"},
        {"fifo_hwm", "pl: data FIFO high-water mark, bytes"},
    };
    char nb[24];
    int first = 1;

    if (!board_stats_ok)
        return;
    printf("board stats:\n");
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    {
        const char *val = stats_find(known[i].key) ? stats_str(known[i].key, nb, sizeof(nb)) : "-";

        printf("  %-13s %-14s %s\n", known[i].key, val, known[i].desc);
    }
    for (unsigned i = 0; i < board_stats_n; i++)
    {
        size_t k;

        for (k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (!strcmp(board_stats[i].key, known[k].key))
                break;
        if (k < sizeof(known) / sizeof(known[0]))
            continue;
        printf("%s%s=%s", first ? "  other         " : " ", board_stats[i].key,
               stats_str(board_stats[i].key, nb, sizeof(nb)));
        first = 0;
    }
    if (!first)
        printf("\n");
}

/* -v: board-side loss as the board counted it next to the offset gaps seen here */
static void print_loss_check(void)
{
    char b1[24], b2[24], b3[24], b4[24], b5[24];
    uint64_t drops;

    printf("loss check    receiver: %" PRIu64 " offset gaps, %" PRIu64 " B skipped\n",
           c.board_gaps, c.board_lost);
    if (!board_stats_ok)
    {
        printf("              board: no STATS reply\n");
        return;
    }
    printf("              board: overflows=%s path=%s linux_frames=%s linux_drops=%s "
           "fifo_hwm=%s\n",
           stats_str("overflows", b1, sizeof(b1)), stats_str("path", b2, sizeof(b2)),
           stats_str("linux_frames", b3, sizeof(b3)), stats_str("linux_drops", b4, sizeof(b4)),
           stats_str("fifo_hwm", b5, sizeof(b5)));
    if (stats_get("linux_drops", &drops) && drops)
        printf("              WARNING: the PL streamer dropped %" PRIu64
               " Linux frames (must be 0)\n",
               drops);
}

static int ctrl_connect(const struct sockaddr_in *board)
{
    struct sockaddr_in sa = *board;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int one = 1, err = 0;
    socklen_t el = sizeof(err);
    struct pollfd pfd;

    if (fd < 0)
    {
        perror("socket(tcp)");
        return -1;
    }
    sa.sin_port = htons((uint16_t)o.ctrl_port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)
    {
        perror("control: connect");
        goto fail;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 3000) <= 0)
    {
        fprintf(stderr, "control: connect to %s:%u timed out\n", o.host, o.ctrl_port);
        goto fail;
    }
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err)
    {
        fprintf(stderr, "control: connect to %s:%u: %s\n", o.host, o.ctrl_port, strerror(err));
        goto fail;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
fail:
    close(fd);
    return -1;
}

/* ------------------------------------------------------------------ */
/* UDP socket                                                          */

static long read_rmem_max(void)
{
    FILE *f = fopen("/proc/sys/net/core/rmem_max", "r");
    long v = -1;

    if (f)
    {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static int udp_open(void)
{
    struct sockaddr_in sa = {0};
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int want = (int)((o.rcvbuf_mb > 1023 ? 1023 : o.rcvbuf_mb) << 20);
    int eff = 0, one = 1, forced;
    socklen_t l = sizeof(eff);
    long rmax = read_rmem_max();
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};

    if (fd < 0)
    {
        perror("socket(udp)");
        return -1;
    }
    /* SO_RCVBUFFORCE ignores rmem_max but needs CAP_NET_ADMIN */
    forced = setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof(want)) == 0;
    if (!forced)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &eff, &l);
    /* getsockopt reports twice the set value (skb bookkeeping overhead) */
    printf("udp: SO_RCVBUF requested %d MiB, effective %.1f MiB (kernel value incl. 2x "
           "overhead; %s)\n",
           want >> 20, eff / 1048576.0,
           forced ? "SO_RCVBUFFORCE" : "SO_RCVBUF, capped by net.core.rmem_max");
    if (!forced && eff / 2 < want)
        printf("udp: hint: net.core.rmem_max=%ld limits the buffer; raise it with\n"
               "       sudo sysctl -w net.core.rmem_max=%d   (or run as root)\n",
               rmax, want);
    setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)o.udp_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        fprintf(stderr, "udp: bind port %u: %s\n", o.udp_port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* receive loop                                                        */

static struct mmsghdr msgs[BATCH];
static struct iovec iovs[BATCH];
static uint8_t bufs[BATCH][DGRAM_BUF];
static struct sockaddr_in srcs[BATCH];
static union
{
    struct cmsghdr align;
    uint8_t b[CMSG_SPACE(sizeof(uint32_t))];
} cbufs[BATCH];

static uint32_t board_addr; /* network order */

static int recv_batch(int fd)
{
    int n;

    for (int i = 0; i < BATCH; i++)
    {
        iovs[i].iov_base = bufs[i];
        iovs[i].iov_len = DGRAM_BUF;
        msgs[i].msg_hdr.msg_name = &srcs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(srcs[i]);
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_control = cbufs[i].b;
        msgs[i].msg_hdr.msg_controllen = sizeof(cbufs[i].b);
        msgs[i].msg_hdr.msg_flags = 0;
    }
    n = recvmmsg(fd, msgs, BATCH, MSG_WAITFORONE, NULL);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        perror("recvmmsg");
        return -1;
    }
    for (int i = 0; i < n; i++)
    {
        struct msghdr *mh = &msgs[i].msg_hdr;

        for (struct cmsghdr *cm = CMSG_FIRSTHDR(mh); cm; cm = CMSG_NXTHDR(mh, cm))
        {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_RXQ_OVFL)
            {
                uint32_t v;

                memcpy(&v, CMSG_DATA(cm), sizeof(v));
                if (v > max_rxq_ovfl)
                    max_rxq_ovfl = v;
            }
        }
        if (srcs[i].sin_addr.s_addr != board_addr)
        {
            c.foreign++;
            continue;
        }
        if (mh->msg_flags & MSG_TRUNC)
        {
            c.trunc++;
            continue;
        }
        process(bufs[i], msgs[i].msg_len);
    }
    return n;
}

static void report(double t, double dt, const struct counters *prev)
{
    uint64_t dg = c.dgrams - prev->dgrams;
    uint64_t by = c.bytes - prev->bytes;
    int64_t lo = c.lost - prev->lost;
    double tot = (double)dg + (lo > 0 ? (double)lo : 0);

    printf("[%6.1fs] %7.2f MB/s %7.3f Msps %7.0f dgram/s  lost %6" PRId64
           " (%6.3f%%) reord %" PRIu64,
           t, by / dt / 1e6, by / dt / o.bytes_per_iq / 1e6, dg / dt, lo,
           tot > 0 ? 100.0 * (lo > 0 ? lo : 0) / tot : 0.0, c.reordered - prev->reordered);
    if (o.cs12)
        printf("  | sync %" PRIu64 " ok %" PRIu64 " err %" PRIu64 " unchk %" PRIu64, c.sync_found,
               c.sync_ok,
               c.sync_counter_err + c.sync_period_err + c.sync_missing + c.sync_phase_chg,
               c.sync_unchecked);
    if (c.board_gaps)
        printf("  | board ovf %" PRIu64 " (%" PRIu64 " B)", c.board_gaps, c.board_lost);
    if (c.restarts)
        printf("  | restarts %" PRIu64, c.restarts);
    if (max_rxq_ovfl)
        printf("  | sock drops %" PRIu32, max_rxq_ovfl);
    printf("\n");
    fflush(stdout);
}

static void drain_stale(int fd)
{
    uint8_t b[DGRAM_BUF];
    unsigned n = 0;

    while (recv(fd, b, sizeof(b), MSG_DONTWAIT) >= 0)
        n++;
    if (n)
        printf("udp: discarded %u stale datagrams before START\n", n);
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0, FILE *f)
{
    fprintf(f,
            "Usage: %s [options]\n"
            "Receive and check the iqnet UDP IQ stream from the board.\n\n"
            "  -H, --host ADDR        board address (default 10.10.11.20)\n"
            "  -p, --port PORT        local UDP port to receive on (default 30432)\n"
            "  -P, --ctrl-port PORT   board TCP control port (default %u)\n"
            "  -m, --mode MODE        cs12 | cs8 | cs16 (default cs12)\n"
            "  -T, --path PATH        board: kernel (iqnet.ko) | pl (PL streamer);\n"
            "                         board default kernel\n"
            "  -l, --payload BYTES    board: IQ bytes per datagram, multiple of %u,\n"
            "                         %u..%u (kernel: <= %u; > %u needs MTU 9000)\n"
            "  -s, --seconds SEC      run time, 0 = until Ctrl+C (default 10)\n"
            "  -g, --gso SEGS         board: datagrams per sendmsg (UDP GSO), 0..%u\n"
            "  -b, --blocks N         board: number of IIO DMA blocks, 1..%u\n"
            "  -B, --block-size BYTES board: IIO block size (multiple of payload)\n"
            "                         (-g/-b/-B apply to path=kernel only)\n"
            "  -o, --output FILE      dump raw IQ payload; file offset = stream offset,\n"
            "                         lost datagrams leave zero-filled (sparse) holes\n"
            "  -r, --rcvbuf MIB       requested SO_RCVBUF in MiB (default 128)\n"
            "  -v, --verbose          log gaps, control traffic, etc.; at the end compare\n"
            "                         the board's overflows with the receiver's offset gaps\n"
            "  -h, --help             this help\n\n"
            "Options -T/-l/-g/-b/-B are only sent in START when given (board defaults\n"
            "otherwise). The datagram size is taken from the board's OK reply.\n"
            "Ctrl+C stops cleanly (STATS + STOP); a second Ctrl+C exits immediately.\n",
            argv0, IQNET_CTRL_PORT, IQNET_BURST, IQNET_BURST, IQNET_MAX_PAYLOAD, IQNET_STD_PAYLOAD,
            IQNET_STD_PAYLOAD, IQNET_MAX_GSO, IQNET_MAX_BLOCKS);
}

static long parse_num(const char *s, const char *what, long min, long max)
{
    char *e;
    long v;

    errno = 0;
    v = strtol(s, &e, 0);
    if (errno || *e || v < min || v > max)
    {
        fprintf(stderr, "invalid %s '%s' (allowed %ld..%ld)\n", what, s, min, max);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'p'},
        {"ctrl-port", required_argument, 0, 'P'},
        {"mode", required_argument, 0, 'm'},
        {"path", required_argument, 0, 'T'},
        {"payload", required_argument, 0, 'l'},
        {"seconds", required_argument, 0, 's'},
        {"gso", required_argument, 0, 'g'},
        {"blocks", required_argument, 0, 'b'},
        {"block-size", required_argument, 0, 'B'},
        {"output", required_argument, 0, 'o'},
        {"rcvbuf", required_argument, 0, 'r'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    struct sockaddr_in board = {0};
    struct addrinfo hints = {0}, *ai = NULL;
    struct sigaction sa = {0};
    char cmd[256], line[sizeof(ctrl_rx)];
    int udp = -1, tcp = -1, ch, rc = 0, gai;
    double t0, t_end, next_rep, t_prev, t_stop;
    struct counters prev;

    while ((ch = getopt_long(argc, argv, "H:p:P:m:T:l:s:g:b:B:o:r:vh", lopts, NULL)) != -1)
    {
        switch (ch)
        {
            case 'H':
                o.host = optarg;
                break;
            case 'p':
                o.udp_port = (unsigned)parse_num(optarg, "UDP port", 1, 65535);
                break;
            case 'P':
                o.ctrl_port = (unsigned)parse_num(optarg, "control port", 1, 65535);
                break;
            case 'm':
                o.mode = optarg;
                break;
            case 'T':
                if (strcmp(optarg, "kernel") && strcmp(optarg, "pl"))
                {
                    fprintf(stderr, "invalid path '%s' (kernel|pl)\n", optarg);
                    return 2;
                }
                o.path = optarg;
                break;
            case 'l':
                o.payload = parse_num(optarg, "payload", IQNET_BURST, IQNET_MAX_PAYLOAD);
                if (o.payload % IQNET_BURST)
                {
                    fprintf(stderr, "invalid payload '%s' (must be a multiple of %u)\n", optarg,
                            IQNET_BURST);
                    return 2;
                }
                break;
            case 's': {
                char *e;

                o.seconds = strtod(optarg, &e);
                if (*e || o.seconds < 0)
                {
                    fprintf(stderr, "invalid seconds '%s'\n", optarg);
                    return 2;
                }
                break;
            }
            case 'g':
                o.gso = parse_num(optarg, "gso", 0, IQNET_MAX_GSO);
                break;
            case 'b':
                o.blocks = parse_num(optarg, "blocks", 1, IQNET_MAX_BLOCKS);
                break;
            case 'B':
                o.block_size = parse_num(optarg, "block size", IQNET_BURST, 0x7fffffffL);
                break;
            case 'o':
                o.dump = optarg;
                break;
            case 'r':
                o.rcvbuf_mb = (unsigned)parse_num(optarg, "rcvbuf", 1, 1023);
                break;
            case 'v':
                o.verbose = 1;
                break;
            case 'h':
                usage(argv[0], stdout);
                return 0;
            default:
                usage(argv[0], stderr);
                return 2;
        }
    }
    if (optind != argc)
    {
        usage(argv[0], stderr);
        return 2;
    }
    if (!strcmp(o.mode, "cs12"))
    {
        o.bytes_per_iq = 3;
        o.cs12 = 1;
    }
    else if (!strcmp(o.mode, "cs8"))
    {
        o.bytes_per_iq = 2;
        o.cs12 = 0;
    }
    else if (!strcmp(o.mode, "cs16"))
    {
        o.bytes_per_iq = 4;
        o.cs12 = 0;
    }
    else
    {
        fprintf(stderr, "invalid mode '%s' (cs12|cs8|cs16)\n", o.mode);
        return 2;
    }

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    gai = getaddrinfo(o.host, NULL, &hints, &ai);
    if (gai || !ai)
    {
        fprintf(stderr, "cannot resolve '%s': %s\n", o.host, gai_strerror(gai));
        return 1;
    }
    memcpy(&board, ai->ai_addr, sizeof(board));
    freeaddrinfo(ai);
    board_addr = board.sin_addr.s_addr;

    sa.sa_handler = on_signal; /* no SA_RESTART: recvmmsg/poll return EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (o.dump)
    {
        dump_fd = open(o.dump, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        dump_buf = malloc(DUMP_BUF);
        if (dump_fd < 0 || !dump_buf)
        {
            fprintf(stderr, "cannot open dump file '%s': %s\n", o.dump, strerror(errno));
            return 1;
        }
    }

    /* 1. UDP socket first, so no early datagram is lost */
    udp = udp_open();
    if (udp < 0)
        return 1;

    /* 2. control connection + START */
    tcp = ctrl_connect(&board);
    if (tcp < 0)
    {
        rc = 1;
        goto out;
    }
    drain_stale(udp);
    {
        int l = snprintf(cmd, sizeof(cmd), "START %u %s", o.udp_port, o.mode);

        if (o.path)
            l += snprintf(cmd + l, sizeof(cmd) - (size_t)l, " path=%s", o.path);
        if (o.payload >= 0)
            l += snprintf(cmd + l, sizeof(cmd) - (size_t)l, " payload=%ld", o.payload);
        if (o.blocks >= 0)
            l += snprintf(cmd + l, sizeof(cmd) - (size_t)l, " blocks=%ld", o.blocks);
        if (o.block_size >= 0)
            l += snprintf(cmd + l, sizeof(cmd) - (size_t)l, " block_size=%ld", o.block_size);
        if (o.gso >= 0)
            l += snprintf(cmd + l, sizeof(cmd) - (size_t)l, " gso=%ld", o.gso);
        snprintf(cmd + l, sizeof(cmd) - (size_t)l, "\n");
    }
    printf("control: %s:%u > %s", o.host, o.ctrl_port, cmd);
    if (ctrl_cmd(tcp, cmd, line, sizeof(line), 10000))
    {
        rc = 1;
        goto out;
    }
    printf("control: < %s\n", line);
    {
        unsigned pl = 0, bs = 0;

        /* block_size 0: path=pl, no IIO blocks behind the datagrams */
        if (sscanf(line, "OK %u %u", &pl, &bs) != 2 || pl == 0 || pl % IQNET_BURST ||
            pl > IQNET_MAX_PAYLOAD)
        {
            fprintf(stderr, "START failed: %s\n", line);
            rc = 1;
            goto out;
        }
        payload_len = pl;
        block_size_reply = bs;
    }
    if (o.payload >= 0 && payload_len != (unsigned)o.payload)
        printf("stream: board uses payload %u B, not the requested %ld B\n", payload_len,
               o.payload);
    if (block_size_reply)
        printf("stream: path %s, mode %s, payload %u B/datagram, block %u B (%u datagrams/block), "
               "%u B/IQ\n",
               o.path ? o.path : "kernel", o.mode, payload_len, block_size_reply,
               block_size_reply / payload_len, o.bytes_per_iq);
    else
        printf("stream: path %s, mode %s, payload %u B/datagram, no IIO blocks (PL streamer), "
               "%u B/IQ\n",
               o.path ? o.path : "pl", o.mode, payload_len, o.bytes_per_iq);

    /* 3. receive loop */
    t0 = now_s();
    t_prev = t0;
    next_rep = t0 + 1.0;
    t_end = o.seconds > 0 ? t0 + o.seconds : 0;
    memset(&prev, 0, sizeof(prev));
    while (!stop_req)
    {
        double t;

        if (recv_batch(udp) < 0)
        {
            rc = 1;
            break;
        }
        t = now_s();
        if (t >= next_rep)
        {
            struct pollfd pfd = {.fd = tcp, .events = POLLIN};

            report(t - t0, t - t_prev, &prev);
            prev = c;
            t_prev = t;
            next_rep += 1.0;
            if (next_rep < t)
                next_rep = t + 1.0;
            /* board closed the control connection? */
            if (poll(&pfd, 1, 0) > 0)
            {
                ssize_t r =
                    recv(tcp, ctrl_rx + ctrl_rx_len, sizeof(ctrl_rx) - ctrl_rx_len, MSG_DONTWAIT);
                if (r == 0)
                {
                    fprintf(stderr, "control: connection closed by board\n");
                    rc = 1;
                    break;
                }
                if (r > 0)
                {
                    ctrl_rx_len += (size_t)r;
                    if (ctrl_rx_len == sizeof(ctrl_rx))
                        ctrl_rx_len = 0;
                }
            }
        }
        if (t_end && t >= t_end)
            break;
    }
    t_stop = now_s();
    if (stop_req)
        printf("\ninterrupted, stopping\n");

    /* 4. STATS, STOP */
    if (rc == 0 || stop_req)
    {
        /* discard unsolicited lines received during the run */
        while (memchr(ctrl_rx, '\n', ctrl_rx_len) &&
               ctrl_readline(tcp, line, sizeof(line), 0) == 1)
            vlog("control < (unsolicited) %s\n", line);
        if (ctrl_cmd(tcp, "STATS\n", line, sizeof(line), 3000) == 0)
        {
            vlog("board: %s\n", line);
            if (parse_stats(line))
                printf("board: unexpected STATS reply: %s\n", line);
        }
        /* the board may take up to 10 s to get every in-flight block back */
        if (ctrl_cmd(tcp, "STOP\n", line, sizeof(line), 12000) == 0)
            printf("control: STOP -> %s\n", line);
    }

    /* datagrams still queued in the socket were sent before STOP */
    {
        double td = now_s() + 0.3;

        stop_req = 0;
        while (now_s() < td && !stop_req)
            if (recv_batch(udp) <= 0)
                break;
    }

    /* 5. summary */
    {
        double dt = t_stop - t0;
        double tot = (double)c.dgrams + (c.lost > 0 ? (double)c.lost : 0);

        if (dt <= 0)
            dt = 1e-9;
        printf("\n==== summary (%.1f s) ====\n", dt);
        printf("datagrams     %" PRIu64 "  (%.0f /s)\n", c.dgrams, c.dgrams / dt);
        printf("payload       %" PRIu64 " B  (%.2f MB/s, %.3f Msps %s)\n", c.bytes,
               c.bytes / dt / 1e6, c.bytes / dt / o.bytes_per_iq / 1e6, o.mode);
        printf("lost          %" PRId64 "  (%.4f%%), gap events %" PRIu64 "\n", c.lost,
               tot > 0 ? 100.0 * (c.lost > 0 ? c.lost : 0) / tot : 0.0, gap_events);
        printf("reordered     %" PRIu64 ", duplicates %" PRIu64 "\n", c.reordered, c.dups);
        printf("board loss    %" PRIu64 " offset jumps, %" PRIu64
               " B (DMA overflow / dropped short blocks)\n",
               c.board_gaps, c.board_lost);
        printf("restarts      %" PRIu64 "  (seq 0, offset 0 while streaming)\n", c.restarts);
        printf("bad magic     %" PRIu64 ", bad length %" PRIu64 ", runt %" PRIu64
               ", truncated %" PRIu64 ", foreign %" PRIu64 ", offset errors %" PRIu64 "\n",
               c.bad_magic, c.bad_len, c.runt, c.trunc, c.foreign, c.offset_err);
        printf("socket drops  %" PRIu32 "  (SO_RXQ_OVFL: host receive buffer overflow)\n",
               max_rxq_ovfl);
        if (have_seq)
            printf("last seq      %" PRIu32 ", stream end offset %" PRIu64 "\n", expected_seq - 1,
                   expected_off);
        if (o.cs12)
        {
            printf("cs12 sync     found %" PRIu64 ", ok %" PRIu64 ", counter errors %" PRIu64
                   ", period errors %" PRIu64 ", missing %" PRIu64 ", phase changes %" PRIu64
                   ", unchecked %" PRIu64 "\n",
                   c.sync_found, c.sync_ok, c.sync_counter_err, c.sync_period_err, c.sync_missing,
                   c.sync_phase_chg, c.sync_unchecked);
            if (sync_phase_known)
                printf("cs12 phase    %u (stream offset of sync bursts mod %u)\n", sync_phase,
                       IQNET_BURST);
            else if (c.bytes >= CS12_SYNC_PERIOD + IQNET_BURST)
                printf("cs12 WARNING  no sync burst found in %" PRIu64 " B: not a CS12 stream?\n",
                       c.bytes);
        }
        print_stats();
        {
            uint64_t ovf = 0, sb = 0;
            int h1 = stats_get("overflows", &ovf), h2 = stats_get("short_blocks", &sb);

            if ((h1 || h2) && !c.board_gaps && (ovf || sb))
                printf("note          board reports overflows=%" PRIu64 " short_blocks=%" PRIu64
                       " but no offset jump was received\n",
                       ovf, sb);
        }
        if (o.verbose)
            print_loss_check();
        if (c.lost > 0 || c.board_gaps || c.restarts || c.sync_counter_err || c.sync_period_err ||
            c.sync_missing || c.sync_phase_chg || c.bad_magic || c.bad_len || c.offset_err)
            rc = rc ? rc : 3;
    }

out:
    if (dump_fd >= 0)
    {
        dump_flush();
        if (dump_fd >= 0)
        {
            close(dump_fd);
            printf("dump: %s (byte N = stream offset N, lost data = zeros)\n", o.dump);
        }
    }
    free(dump_buf);
    if (tcp >= 0)
        close(tcp);
    if (udp >= 0)
        close(udp);
    return rc;
}
