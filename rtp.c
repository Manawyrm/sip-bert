/* rtp.c -- see rtp.h */
#define _GNU_SOURCE
#include "rtp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Per-process-seeded 32-bit random: SSRC/seq/ts must be unique per stream.
 * Unseeded random() yields the same sequence in every process -> identical
 * SSRC across daemons (RFC 3550 collision; breaks PBX media relay). */
static uint32_t rtp_rand32(void)
{
    static int seeded = 0;
    if (!seeded)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        srandom((unsigned)(ts.tv_nsec ^ (ts.tv_sec << 8) ^ ((unsigned)getpid() << 16)));
        seeded = 1;
    }
    return ((uint32_t)random() << 16) ^ (uint32_t)random();
}

#define RTP_HDR_LEN 12

struct rtp_ep
{
    int fd;
    int local_port;
    int payload_type;
    struct sockaddr_storage remote;
    socklen_t remote_len;
    int remote_valid;
    uint16_t tx_seq;
    uint32_t tx_ts, tx_ssrc;
    int tx_started;
    /* receive source lock: pin one SSRC so a stray/overlapping stream (e.g. PBX
     * early-media/ringback) can't interleave its sequence space into the jitter
     * buffer. Re-latch only if the pinned stream goes quiet. */
    uint32_t rx_ssrc;
    int rx_locked;
    int rx_miss;
    rtp_rx_cb cb;
    void *user;
    unsigned tx_pkts, rx_pkts, rx_bad;
};

#define RX_SWITCH_MISSES 5 /* consecutive foreign packets before re-latching */

struct rtp_ep *rtp_ep_create(const char *bind_addr, int bind_port, int payload_type,
                             rtp_rx_cb cb, void *user)
{
    struct rtp_ep *ep = calloc(1, sizeof(*ep));
    if (!ep)
        return NULL;
    ep->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ep->fd < 0)
    {
        free(ep);
        return NULL;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(bind_port);
    sa.sin_addr.s_addr = bind_addr ? inet_addr(bind_addr) : htonl(INADDR_ANY);
    if (bind(ep->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        close(ep->fd);
        free(ep);
        return NULL;
    }
    socklen_t sl = sizeof(sa);
    if (getsockname(ep->fd, (struct sockaddr *)&sa, &sl) == 0)
        ep->local_port = ntohs(sa.sin_port);
    else
        ep->local_port = bind_port;

    fcntl(ep->fd, F_SETFL, fcntl(ep->fd, F_GETFL) | O_NONBLOCK);
    ep->payload_type = payload_type;
    ep->cb = cb;
    ep->user = user;
    ep->tx_ssrc = rtp_rand32();         /* unique per stream */
    ep->tx_seq = rtp_rand32() & 0xffff; /* RFC 3550: random initial seq/ts */
    ep->tx_ts = rtp_rand32();
    return ep;
}

int rtp_ep_fd(const struct rtp_ep *ep)
{
    return ep ? ep->fd : -1;
}
int rtp_ep_local_port(const struct rtp_ep *ep)
{
    return ep ? ep->local_port : -1;
}

int rtp_ep_set_remote(struct rtp_ep *ep, const char *addr, int port)
{
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
        return -1;
    memcpy(&ep->remote, &sa, sizeof(sa));
    ep->remote_len = sizeof(sa);
    ep->remote_valid = 1;
    ep->rx_locked = 0; /* re-latch the receive stream for this call */
    return 0;
}

void rtp_ep_clear_remote(struct rtp_ep *ep)
{
    if (ep)
    {
        ep->remote_valid = 0;
        ep->rx_locked = 0;
    }
}

void rtp_ep_recv(struct rtp_ep *ep)
{
    uint8_t buf[2048];
    for (;;)
    {
        ssize_t rc = recv(ep->fd, buf, sizeof(buf), 0);
        if (rc < 0)
            break; /* EAGAIN: done */
        if (rc < RTP_HDR_LEN || (buf[0] >> 6) != 2)
        {
            ep->rx_bad++;
            continue;
        }
        unsigned cc = buf[0] & 0x0f;
        size_t hdr = RTP_HDR_LEN + cc * 4;
        if ((size_t)rc <= hdr)
        {
            ep->rx_bad++;
            continue;
        }
        uint16_t seq = ((uint16_t)buf[2] << 8) | buf[3];
        uint32_t ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                        ((uint32_t)buf[10] << 8) | buf[11];

        /* source lock: accept exactly one SSRC. A different SSRC is dropped
         * unless the pinned one has gone quiet (RX_SWITCH_MISSES in a row), in
         * which case we re-latch -- the jitter buffer resyncs on the seq jump. */
        if (!ep->rx_locked)
        {
            ep->rx_ssrc = ssrc;
            ep->rx_locked = 1;
            ep->rx_miss = 0;
        }
        else if (ssrc == ep->rx_ssrc)
        {
            ep->rx_miss = 0;
        }
        else if (++ep->rx_miss > RX_SWITCH_MISSES)
        {
            ep->rx_ssrc = ssrc;
            ep->rx_miss = 0;
        }
        else
        {
            ep->rx_bad++;
            continue; /* foreign stream while ours is active */
        }

        ep->rx_pkts++;
        if (ep->cb)
            ep->cb(ep->user, buf + hdr, (int)(rc - hdr), seq);
    }
}

int rtp_ep_send(struct rtp_ep *ep, const uint8_t *payload, int len)
{
    if (!ep->remote_valid)
        return -ENOTCONN;
    uint8_t buf[RTP_HDR_LEN + 2048];
    if (len > (int)sizeof(buf) - RTP_HDR_LEN)
        return -EMSGSIZE;

    buf[0] = 0x80;
    buf[1] =
        (ep->payload_type & 0x7f) | (ep->tx_started ? 0 : 0x80); /* marker on first */
    buf[2] = ep->tx_seq >> 8;
    buf[3] = ep->tx_seq & 0xff;
    buf[4] = ep->tx_ts >> 24;
    buf[5] = ep->tx_ts >> 16;
    buf[6] = ep->tx_ts >> 8;
    buf[7] = ep->tx_ts & 0xff;
    buf[8] = ep->tx_ssrc >> 24;
    buf[9] = ep->tx_ssrc >> 16;
    buf[10] = ep->tx_ssrc >> 8;
    buf[11] = ep->tx_ssrc & 0xff;
    memcpy(buf + RTP_HDR_LEN, payload, len);

    ep->tx_seq++;
    ep->tx_ts += len; /* 8 kHz, one octet per timestamp unit */
    ep->tx_started = 1;

    ssize_t rc = sendto(ep->fd, buf, RTP_HDR_LEN + len, 0, (struct sockaddr *)&ep->remote,
                        ep->remote_len);
    if (rc < 0)
        return -errno;
    ep->tx_pkts++;
    return 0;
}

void rtp_ep_stats(const struct rtp_ep *ep, unsigned *tx, unsigned *rx, unsigned *bad)
{
    if (tx)
        *tx = ep->tx_pkts;
    if (rx)
        *rx = ep->rx_pkts;
    if (bad)
        *bad = ep->rx_bad;
}

void rtp_ep_free(struct rtp_ep *ep)
{
    if (!ep)
        return;
    if (ep->fd >= 0)
        close(ep->fd);
    free(ep);
}
