/*
 * sip.h -- SIP user agent (sofia-sip nua): REGISTER + digest auth, and
 * outbound/inbound call control. Media (RTP) is handled outside this module;
 * SIP only negotiates the remote RTP address/port via SDP (PCMA).
 *
 * One sip_ua = one SIP identity (registers to a PBX). Multiple UAs can share
 * one su_root. Callbacks report call-state changes to the app, which bridges
 * them to the modem's hook/dial/ring signalling.
 */
#ifndef SIP_MODEM_SIP_H
#define SIP_MODEM_SIP_H

#include <sofia-sip/su_wait.h> /* su_root_t */

struct sip_ua;
struct sip_call;

struct sip_cfg
{
    const char *host;       /* registrar / PBX host (e.g. "127.0.0.1")     */
    int port;               /* registrar port (0 -> 5060)                  */
    const char *user;       /* auth username + AOR user (e.g. "6001")      */
    const char *pass;       /* auth password                               */
    const char *local_addr; /* our SIP/contact IP (e.g. "127.0.0.1")       */
    int local_port;         /* our SIP UDP port (0 -> ephemeral)           */
};

struct sip_cbs
{
    void (*on_registered)(void *u, int ok, int status);
    /* inbound INVITE: caller id + remote RTP endpoint. App rings the modem. */
    void (*on_incoming)(void *u, struct sip_call *c, const char *from,
                        const char *rtp_addr, int rtp_port);
    void (*on_ringing)(void *u, struct sip_call *c);
    /* our outbound call was answered; remote RTP endpoint provided. */
    void (*on_answered)(void *u, struct sip_call *c, const char *rtp_addr, int rtp_port);
    void (*on_failed)(void *u, struct sip_call *c, int status); /* busy/decline */
    void (*on_hangup)(void *u, struct sip_call *c);
};

struct sip_ua *sip_ua_create(su_root_t *root, const struct sip_cfg *cfg,
                             const struct sip_cbs *cbs, void *user);
int sip_ua_register(struct sip_ua *ua);
void sip_ua_destroy(struct sip_ua *ua);

/* Place an outbound call to `number`, advertising our local RTP addr/port. */
struct sip_call *sip_call_invite(struct sip_ua *ua, const char *number,
                                 const char *rtp_addr, int rtp_port);
/* Answer an inbound call, advertising our local RTP addr/port. */
void sip_call_answer(struct sip_call *c, const char *rtp_addr, int rtp_port);
void sip_call_hangup(struct sip_call *c);

#endif
