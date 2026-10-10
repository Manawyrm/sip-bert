/* sdp.c -- see sdp.h */
#include "sdp.h"

#include <sofia-sip/sdp.h>

#include <stdio.h>
#include <string.h>

bool sdp_pcma_parse(const char *sdp_str, struct sdp_media *out)
{
    if (!sdp_str)
        return false;
    sdp_parser_t *p = sdp_parse(NULL, sdp_str, strlen(sdp_str), 0);
    if (!p)
        return false;

    sdp_session_t *sdp = sdp_session(p);
    bool found = false;
    for (sdp_media_t *m = sdp ? sdp->sdp_media : NULL; m && !found; m = m->m_next)
    {
        if (m->m_proto != sdp_proto_rtp || m->m_type != sdp_media_audio)
            continue;
        for (sdp_rtpmap_t *rm = m->m_rtpmaps; rm; rm = rm->rm_next)
        {
            if (rm->rm_pt != 8 && strcasecmp(rm->rm_encoding, "PCMA") != 0)
                continue;
            sdp_connection_t *c = m->m_connections;
            if (!c)
                c = sdp->sdp_connection;
            if (!c || !c->c_address)
                continue;
            snprintf(out->addr, sizeof(out->addr), "%s", c->c_address);
            out->port = m->m_port;
            out->payload_type = rm->rm_pt;
            found = true;
            break;
        }
    }
    sdp_parser_free(p);
    return found;
}

int sdp_pcma_create(char *buf, size_t len, const char *local_addr, int local_port)
{
    return snprintf(buf, len,
                    "v=0\r\n"
                    "o=sipmodem 0 0 IN IP4 %s\r\n"
                    "s=modem\r\n"
                    "c=IN IP4 %s\r\n"
                    "t=0 0\r\n"
                    "m=audio %d RTP/AVP 8\r\n"
                    "a=rtpmap:8 PCMA/8000\r\n"
                    "a=ptime:20\r\n"
                    "a=silenceSupp:off - - - -\r\n",
                    local_addr, local_addr, local_port);
}
