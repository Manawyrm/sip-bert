/* sip.c -- see sip.h */
#define _GNU_SOURCE

/* type the nua callback's magic pointers before including nua.h */
#define NUA_MAGIC_T struct sip_ua
#define NUA_HMAGIC_T struct sip_call

#include "sip.h"
#include "sdp.h"

#include <sofia-sip/msg_header.h>
#include <sofia-sip/nua.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/sip_status.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sip_ua
{
    su_root_t *root;
    nua_t *nua;
    nua_handle_t *reg_nh;
    struct sip_cfg cfg;
    struct sip_cbs cbs;
    void *user;
    int shutdown_done;
    /* owned copies of cfg strings */
    char *host, *usr, *pass, *laddr;
};

struct sip_call
{
    struct sip_ua *ua;
    nua_handle_t *nh;
    int is_out;
    int answered;
};

#define SLOG(fmt, ...) fprintf(stderr, "sip[%s]: " fmt "\n", ua->usr, ##__VA_ARGS__)

static const char *sdp_body(const sip_t *sip)
{
    return (sip && sip->sip_payload) ? sip->sip_payload->pl_data : NULL;
}

/* answer a 401/407 challenge with our digest credentials */
static void authenticate(struct sip_ua *ua, nua_handle_t *nh, const sip_t *sip)
{
    const char *scheme = "Digest", *realm = NULL;
    if (sip && sip->sip_www_authenticate)
    {
        scheme = sip->sip_www_authenticate->au_scheme;
        realm = msg_params_find(sip->sip_www_authenticate->au_params, "realm=");
    }
    else if (sip && sip->sip_proxy_authenticate)
    {
        scheme = sip->sip_proxy_authenticate->au_scheme;
        realm = msg_params_find(sip->sip_proxy_authenticate->au_params, "realm=");
    }
    char aut[320];
    /* realm from msg_params_find already carries its quotes, e.g. "asterisk" */
    snprintf(aut, sizeof(aut), "%s:%s:%s:%s", scheme, realm ? realm : "\"\"", ua->usr,
             ua->pass);
    SLOG("auth -> %s", aut);
    nua_authenticate(nh, NUTAG_AUTH(aut), TAG_END());
}

static struct sip_call *call_new(struct sip_ua *ua, nua_handle_t *nh, int is_out)
{
    struct sip_call *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->ua = ua;
    c->nh = nh;
    c->is_out = is_out;
    return c;
}

static void call_free(struct sip_call *c)
{
    if (!c)
        return;
    if (c->nh)
        nua_handle_destroy(c->nh);
    free(c);
}

static void nua_cb(nua_event_t event, int status, char const *phrase, nua_t *nua,
                   struct sip_ua *ua, nua_handle_t *nh, struct sip_call *call,
                   sip_t const *sip, tagi_t tags[])
{
    (void)nua;
    (void)tags;
    SLOG("event %s status %d (%s)", nua_event_name(event), status, phrase ? phrase : "");

    switch (event)
    {
    case nua_r_register:
        if (status == 401 || status == 407)
        {
            authenticate(ua, nh, sip);
            break;
        }
        if (ua->cbs.on_registered)
            ua->cbs.on_registered(ua->user, status == 200, status);
        break;

    case nua_r_invite:
        if (!call)
            break;
        if (status == 401 || status == 407)
        {
            authenticate(ua, nh, sip);
            break;
        }
        if (status == 180 || status == 183)
        {
            if (ua->cbs.on_ringing)
                ua->cbs.on_ringing(ua->user, call);
            break;
        }
        if (status == 200)
        {
            struct sdp_media m = {.port = 0};
            nua_ack(nh, TAG_END());
            call->answered = 1;
            if (sdp_pcma_parse(sdp_body(sip), &m) && ua->cbs.on_answered)
                ua->cbs.on_answered(ua->user, call, m.addr, m.port);
            break;
        }
        if (status >= 300)
        {
            if (ua->cbs.on_failed)
                ua->cbs.on_failed(ua->user, call, status);
            call_free(call);
        }
        break;

    case nua_i_invite:
        if (call)
            break; /* re-INVITE: ignore for now */
        if (status != 100)
            break;
        {
            struct sip_call *c = call_new(ua, nh, 0);
            if (!c)
            {
                nua_respond(nh, SIP_503_SERVICE_UNAVAILABLE, TAG_END());
                break;
            }
            nua_handle_bind(nh, c);
            struct sdp_media m = {.port = 0};
            sdp_pcma_parse(sdp_body(sip), &m);
            const char *from =
                (sip && sip->sip_from) ? sip->sip_from->a_url->url_user : "?";
            nua_respond(nh, SIP_180_RINGING, TAG_END());
            if (ua->cbs.on_incoming)
                ua->cbs.on_incoming(ua->user, c, from, m.addr, m.port);
        }
        break;

    case nua_i_bye:
    case nua_i_cancel:
        if (call)
        {
            if (ua->cbs.on_hangup)
                ua->cbs.on_hangup(ua->user, call);
            call_free(call);
        }
        break;

    case nua_r_bye:
    case nua_r_cancel:
        if (call)
            call_free(call);
        break;

    case nua_r_shutdown:
        if (status >= 200)
            ua->shutdown_done = 1;
        break;

    default:
        break;
    }
}

struct sip_ua *sip_ua_create(su_root_t *root, const struct sip_cfg *cfg,
                             const struct sip_cbs *cbs, void *user)
{
    struct sip_ua *ua = calloc(1, sizeof(*ua));
    if (!ua)
        return NULL;
    ua->root = root;
    ua->cfg = *cfg;
    ua->cbs = *cbs;
    ua->user = user;
    ua->host = strdup(cfg->host);
    ua->usr = strdup(cfg->user);
    ua->pass = strdup(cfg->pass);
    ua->laddr = strdup(cfg->local_addr ? cfg->local_addr : "0.0.0.0");

    char url[128];
    if (cfg->local_port > 0)
        snprintf(url, sizeof(url), "sip:%s:%d", ua->laddr, cfg->local_port);
    else
        snprintf(url, sizeof(url), "sip:%s:*", ua->laddr);

    ua->nua =
        nua_create(root, (nua_callback_f)nua_cb, ua, NUTAG_URL(url), NUTAG_AUTOACK(0),
                   NUTAG_AUTOALERT(0), NUTAG_AUTOANSWER(0), TAG_END());
    if (!ua->nua)
    {
        sip_ua_destroy(ua);
        return NULL;
    }
    return ua;
}

int sip_ua_register(struct sip_ua *ua)
{
    char aor[128], registrar[128];
    snprintf(aor, sizeof(aor), "sip:%s@%s", ua->usr, ua->host);
    if (ua->cfg.port > 0)
        snprintf(registrar, sizeof(registrar), "sip:%s:%d", ua->host, ua->cfg.port);
    else
        snprintf(registrar, sizeof(registrar), "sip:%s", ua->host);

    ua->reg_nh = nua_handle(ua->nua, NULL, TAG_END());
    if (!ua->reg_nh)
        return -1;
    nua_register(ua->reg_nh, NUTAG_REGISTRAR(registrar), SIPTAG_FROM_STR(aor),
                 SIPTAG_TO_STR(aor), NUTAG_M_USERNAME(ua->usr), TAG_END());
    return 0;
}

struct sip_call *sip_call_invite(struct sip_ua *ua, const char *number,
                                 const char *rtp_addr, int rtp_port)
{
    struct sip_call *c = call_new(ua, NULL, 1);
    if (!c)
        return NULL;
    c->nh = nua_handle(ua->nua, c, TAG_END());
    if (!c->nh)
    {
        free(c);
        return NULL;
    }

    char from[128], to[128], sdp[512];
    snprintf(from, sizeof(from), "sip:%s@%s", ua->usr, ua->host);
    snprintf(to, sizeof(to), "sip:%s@%s", number, ua->host);
    sdp_pcma_create(sdp, sizeof(sdp), rtp_addr, rtp_port);

    nua_invite(c->nh, SIPTAG_FROM_STR(from), SIPTAG_TO_STR(to), NUTAG_MEDIA_ENABLE(0),
               SIPTAG_CONTENT_TYPE_STR("application/sdp"), SIPTAG_PAYLOAD_STR(sdp),
               TAG_END());
    return c;
}

void sip_call_answer(struct sip_call *c, const char *rtp_addr, int rtp_port)
{
    char sdp[512];
    sdp_pcma_create(sdp, sizeof(sdp), rtp_addr, rtp_port);
    nua_respond(c->nh, SIP_200_OK, NUTAG_MEDIA_ENABLE(0),
                SIPTAG_CONTENT_TYPE_STR("application/sdp"), SIPTAG_PAYLOAD_STR(sdp),
                TAG_END());
    c->answered = 1;
}

void sip_call_hangup(struct sip_call *c)
{
    if (!c || !c->nh)
        return;
    if (c->answered)
        nua_bye(c->nh, TAG_END());
    else if (c->is_out)
        nua_cancel(c->nh, TAG_END());
    else
        nua_respond(c->nh, SIP_486_BUSY_HERE, TAG_END());
}

void sip_ua_destroy(struct sip_ua *ua)
{
    if (!ua)
        return;
    if (ua->nua)
    {
        nua_shutdown(ua->nua); /* must complete before destroy */
        for (int i = 0; i < 200 && !ua->shutdown_done; i++)
            su_root_step(ua->root, 50); /* pump until nua_r_shutdown (>=200) */
        nua_destroy(ua->nua);
    }
    free(ua->host);
    free(ua->usr);
    free(ua->pass);
    free(ua->laddr);
    free(ua);
}
