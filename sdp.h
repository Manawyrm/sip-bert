/*
 * sdp.h -- minimal SDP offer/answer for a single G.711 a-law (PCMA) stream.
 * The modem line format is a-law @ 8000 Hz, so PCMA/8 is all we need.
 */
#ifndef SIP_MODEM_SDP_H
#define SIP_MODEM_SDP_H

#include <stdbool.h>
#include <stddef.h>

struct sdp_media
{
    char addr[64];
    int port;
    int payload_type; /* 8 for static PCMA */
};

/* Parse the first PCMA audio stream from an SDP blob. */
bool sdp_pcma_parse(const char *sdp, struct sdp_media *out);

/* Write a PCMA offer/answer into buf. Returns bytes written (excl. NUL). */
int sdp_pcma_create(char *buf, size_t len, const char *local_addr, int local_port);

#endif
