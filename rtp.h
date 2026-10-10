/*
 * rtp.h -- minimal RTP/UDP endpoint for G.711 payloads.
 *
 * Modelled on osmo-csd-iwf/src/rtp.c but standalone (plain BSD sockets, no
 * osmocom). Loop-agnostic: the caller registers rtp_ep_fd() in its event loop
 * and calls rtp_ep_recv() when it is readable.
 */
#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <sys/socket.h>

struct rtp_ep;

/* payload, length, and RTP sequence number of a received packet */
typedef void (*rtp_rx_cb)(void *user, const uint8_t *pl, int len, uint16_t seq);

struct rtp_ep *rtp_ep_create(const char *bind_addr, int bind_port, int payload_type,
                             rtp_rx_cb cb, void *user);
int rtp_ep_fd(const struct rtp_ep *ep);
int rtp_ep_local_port(const struct rtp_ep *ep);
int rtp_ep_set_remote(struct rtp_ep *ep, const char *addr, int port);
void rtp_ep_clear_remote(struct rtp_ep *ep); /* stop sending on hangup */
void rtp_ep_recv(struct rtp_ep *ep);         /* call when fd readable */
int rtp_ep_send(struct rtp_ep *ep, const uint8_t *payload, int len);
void rtp_ep_stats(const struct rtp_ep *ep, unsigned *tx, unsigned *rx, unsigned *bad);
void rtp_ep_free(struct rtp_ep *ep);

#endif
