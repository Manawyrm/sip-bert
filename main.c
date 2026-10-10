/*
 * main.c -- sip-bert: a PRBS-11 bit-error-rate tester over SIP / G.711a.
 *
 * The 64 kbit/s ISDN B-channel BERT, reincarnated on VoIP. A G.711 a-law call
 * is exactly a 64 kbit/s octet pipe (8 kHz x 8 bits), so we serialise a PRBS-11
 * pattern (ITU-T O.153, x^11 + x^9 + 1) straight into the RTP payload octets,
 * transmit it, and at the same time synchronise a reference PRBS-11 to whatever
 * we receive and count bit errors -- a true bit-transparency test of the path.
 *
 * Two companion endpoints:
 *   - a PRBS line (e.g. 8003): the PBX digitally streams PRBS-11 at us. We only
 *     verify the RX direction; it should be bit-perfect barring RTP loss.
 *   - an echo/loopback (e.g. 8004): our own PRBS comes back, testing the full
 *     round trip.
 *
 * A single sofia su_root loop drives a fixed 20 ms pump (160 octets/frame = one
 * RTP packet) plus the SIP UA and the RTP socket. The RX path treats each octet
 * as 8 raw B-channel bits; because we don't know a priori how the far end packs
 * bits, four checkers run in parallel (MSB/LSB order x normal/inverted) and the
 * one that locks wins.
 *
 * SIP boilerplate (args, UA, SDP/RTP wiring) is lifted from apps/sip-modem.
 */
#include "prbs.h"
#include "rtp.h"
#include "sip.h"

#include <sofia-sip/su.h>
#include <sofia-sip/su_wait.h>

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME 160        /* octets per 20 ms G.711 frame = one RTP packet */
#define ALAW_SILENCE 0xD5
#define N_CHK 4          /* MSB/LSB x normal/inverted */

struct app
{
    /* config */
    struct sip_cfg sipcfg;
    const char *number;   /* test number to dial            */
    double duration;      /* seconds to measure (0 = until hangup / Ctrl-C) */
    double report_every;  /* stats print interval (s)       */
    int tx_silence;       /* send silence instead of PRBS   */

    /* runtime */
    su_root_t *root;
    struct rtp_ep *rtp;
    struct sip_ua *ua;
    struct sip_call *call;
    int dialed;
    int media;            /* RTP peer known: pump + measure */
    double period;        /* pump period (ms), fixed 20      */

    /* TX pattern */
    struct prbs_gen gen;

    /* RX: four interpretations, whichever locks wins */
    struct prbs_chk chk[N_CHK];
    int rx_have_seq;
    uint16_t rx_last_seq;
    unsigned rx_lost;     /* frames missing (seq gaps)      */
    unsigned rx_reorder;  /* out-of-order / duplicate drops */

    double t_answer;      /* monotonic ms when media started */
    double t_next_report;
};

#define LOG(fmt, ...) fprintf(stderr, "sipbert: " fmt "\n", ##__VA_ARGS__)

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ---------- su_root fd registration helper ---------- */
struct watch
{
    struct app *app;
    int fd;
    void (*fn)(struct app *, int);
};

static int watch_cb(su_root_magic_t *m, su_wait_t *w, su_wakeup_arg_t *arg)
{
    (void)m;
    (void)w;
    struct watch *wt = (struct watch *)arg;
    wt->fn(wt->app, wt->fd);
    return 0;
}
static void watch_fd(struct app *app, int fd, void (*fn)(struct app *, int))
{
    struct watch *wt = calloc(1, sizeof(*wt));
    wt->app = app;
    wt->fd = fd;
    wt->fn = fn;
    su_wait_t wait[1];
    su_wait_create(wait, fd, SU_WAIT_IN);
    su_root_register(app->root, wait, watch_cb, (su_wakeup_arg_t *)wt, 0);
}

/* ---------- RX ---------- */
static const char *chk_name(const struct prbs_chk *c)
{
    if (c->lsb_first)
        return c->invert ? "lsb+inv" : "lsb";
    return c->invert ? "msb+inv" : "msb";
}

/* The single checker currently in lock, or NULL while hunting. */
static struct prbs_chk *rx_locked(struct app *app)
{
    for (int i = 0; i < N_CHK; i++)
        if (app->chk[i].locked)
            return &app->chk[i];
    return NULL;
}

static void rtp_rx(void *user, const uint8_t *pl, int len, uint16_t seq)
{
    struct app *app = user;

    /* Ordered, de-duplicated delivery. A sequence gap means whole frames were
     * lost: the far-end PRBS ran on regardless, so our reference would slip --
     * force a re-sync instead of counting the slip as a massive error burst. */
    if (app->rx_have_seq)
    {
        int16_t d = (int16_t)(seq - app->rx_last_seq);
        if (d <= 0)
        {
            app->rx_reorder++;
            return; /* duplicate or reordered straggler */
        }
        if (d > 1)
        {
            app->rx_lost += (unsigned)(d - 1);
            for (int i = 0; i < N_CHK; i++)
                prbs_chk_resync(&app->chk[i]);
        }
    }
    app->rx_last_seq = seq;
    app->rx_have_seq = 1;

    for (int i = 0; i < N_CHK; i++)
        prbs_chk_bytes(&app->chk[i], pl, len);
}

static void rtp_in(struct app *app, int fd)
{
    (void)fd;
    rtp_ep_recv(app->rtp);
}

/* ---------- stats ---------- */
static void print_stats(struct app *app, int final)
{
    double secs = app->t_answer > 0 ? (now_ms() - app->t_answer) / 1000.0 : 0.0;
    unsigned tx = 0, rx = 0, bad = 0;
    rtp_ep_stats(app->rtp, &tx, &rx, &bad);
    struct prbs_chk *c = rx_locked(app);

    if (c)
    {
        double ber = prbs_chk_ber(c);
        fprintf(stderr,
                "[%6.1fs] LOCK(%-7s) bits=%llu err=%llu BER=%.2e"
                " | rtp tx=%u rx=%u lost=%u reord=%u resync=%u\n",
                secs, chk_name(c), (unsigned long long)c->bits,
                (unsigned long long)c->errors, ber, tx, rx, app->rx_lost,
                app->rx_reorder, c->resyncs);
    }
    else
    {
        fprintf(stderr,
                "[%6.1fs] HUNT (no PRBS-11 lock yet)"
                " | rtp tx=%u rx=%u lost=%u reord=%u\n",
                secs, tx, rx, app->rx_lost, app->rx_reorder);
    }

    if (getenv("BERT_DEBUG"))
        for (int i = 0; i < N_CHK; i++)
            fprintf(stderr, "    dbg chk[%d] %-7s state=%d locked=%d fill=%d verify=%d "
                            "win_bits=%u win_errs=%u bits=%llu errs=%llu resync=%u\n",
                    i, chk_name(&app->chk[i]), app->chk[i].state, app->chk[i].locked,
                    app->chk[i].fill, app->chk[i].verify_ok, app->chk[i].win_bits,
                    app->chk[i].win_errs, (unsigned long long)app->chk[i].bits,
                    (unsigned long long)app->chk[i].errors, app->chk[i].resyncs);

    if (final)
    {
        fprintf(stderr, "---- BERT result ----\n");
        if (c)
        {
            double ber = prbs_chk_ber(c);
            fprintf(stderr, "  pattern     : PRBS-11 (x^11+x^9+1), %s octet order%s\n",
                    c->lsb_first ? "LSB-first" : "MSB-first",
                    c->invert ? ", inverted" : "");
            fprintf(stderr, "  measured    : %.1f s\n", secs);
            fprintf(stderr, "  bits        : %llu\n", (unsigned long long)c->bits);
            fprintf(stderr, "  bit errors  : %llu\n", (unsigned long long)c->errors);
            fprintf(stderr, "  BER         : %.3e%s\n", ber,
                    c->errors == 0 ? "  (error-free)" : "");
            fprintf(stderr, "  resyncs     : %u\n", c->resyncs);
        }
        else
        {
            fprintf(stderr, "  NO LOCK -- never synchronised to a PRBS-11 stream.\n");
            fprintf(stderr, "  (RX octets did not match any of MSB/LSB x normal/inverted.)\n");
        }
        fprintf(stderr, "  rtp packets : tx=%u rx=%u bad=%u\n", tx, rx, bad);
        fprintf(stderr, "  frames lost : %u  (reordered/dup dropped: %u)\n", app->rx_lost,
                app->rx_reorder);
    }
}

/* ---------- pump ---------- */
static void pump_tick(struct app *app)
{
    app->period = 20.0;
    if (!app->media)
        return;

    uint8_t frame[FRAME];
    if (app->tx_silence)
        memset(frame, ALAW_SILENCE, FRAME);
    else
        prbs_gen_bytes(&app->gen, frame, FRAME);
    rtp_ep_send(app->rtp, frame, FRAME);
}

/* ---------- SIP glue ---------- */
static const char *our_rtp_addr(struct app *app)
{
    return app->sipcfg.local_addr ? app->sipcfg.local_addr : "127.0.0.1";
}

static void start_call(struct app *app)
{
    if (app->dialed)
        return;
    app->dialed = 1;
    LOG("dialing %s -> INVITE", app->number);
    app->call = sip_call_invite(app->ua, app->number, our_rtp_addr(app),
                                rtp_ep_local_port(app->rtp));
}

static void on_registered(void *u, int ok, int status)
{
    struct app *app = u;
    LOG("REGISTER %s (%d)", ok ? "ok" : "FAILED", status);
    if (ok)
        start_call(app); /* dial once, on first successful registration */
}

static void on_incoming(void *u, struct sip_call *c, const char *from,
                        const char *rtp_addr, int rtp_port)
{
    (void)u;
    (void)from;
    (void)rtp_addr;
    (void)rtp_port;
    sip_call_hangup(c); /* we are the caller only; reject inbound */
}

static void on_ringing(void *u, struct sip_call *c)
{
    (void)u;
    (void)c;
    LOG("ringing...");
}

static void on_answered(void *u, struct sip_call *c, const char *rtp_addr, int rtp_port)
{
    struct app *app = u;
    (void)c;
    LOG("answered (remote RTP %s:%d) -- measuring", rtp_addr, rtp_port);
    rtp_ep_set_remote(app->rtp, rtp_addr, rtp_port);
    app->media = 1;
    app->t_answer = now_ms();
    app->t_next_report = app->t_answer + app->report_every * 1000.0;
}

static void on_failed(void *u, struct sip_call *c, int status)
{
    struct app *app = u;
    (void)c;
    LOG("call failed: %d", status);
    app->call = NULL;
    g_stop = 1;
}

static void on_hangup(void *u, struct sip_call *c)
{
    struct app *app = u;
    (void)c;
    LOG("remote hangup");
    app->call = NULL;
    app->media = 0;
    g_stop = 1;
}

/* ---------- usage ---------- */
static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s --sip-host H --sip-user U --sip-pass P --test-number N [options]\n"
            "  --sip-host H       registrar/PBX host\n"
            "  --sip-user U       auth user + AOR (e.g. 4011)\n"
            "  --sip-pass P       auth password\n"
            "  --sip-port N       our local SIP UDP port (default 5060)\n"
            "  --local-addr A     our IP for SIP/RTP (default 127.0.0.1)\n"
            "  --test-number N    number to dial (e.g. 8003 BERT, 8004 echo)\n"
            "  --duration S       measure for S seconds then hang up (default: until BYE/^C)\n"
            "  --report S         stats print interval in seconds (default 1)\n"
            "  --tx-silence       send a-law silence instead of PRBS (RX-only test)\n",
            p);
}

int main(int argc, char **argv)
{
    struct app app = {0};
    app.sipcfg.local_addr = "127.0.0.1";
    app.sipcfg.local_port = 5060;
    app.report_every = 1.0;
    app.period = 20.0;

    enum
    {
        O_HOST = 256,
        O_USER,
        O_PASS,
        O_SPORT,
        O_LADDR,
        O_NUM,
        O_DUR,
        O_REP,
        O_TXSIL
    };
    static struct option opts[] = {
        {"sip-host", 1, 0, O_HOST},    {"sip-user", 1, 0, O_USER},
        {"sip-pass", 1, 0, O_PASS},    {"sip-port", 1, 0, O_SPORT},
        {"local-addr", 1, 0, O_LADDR}, {"test-number", 1, 0, O_NUM},
        {"duration", 1, 0, O_DUR},     {"report", 1, 0, O_REP},
        {"tx-silence", 0, 0, O_TXSIL}, {"help", 0, 0, 'h'},
        {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1)
    {
        switch (c)
        {
        case O_HOST:
            app.sipcfg.host = optarg;
            break;
        case O_USER:
            app.sipcfg.user = optarg;
            break;
        case O_PASS:
            app.sipcfg.pass = optarg;
            break;
        case O_SPORT:
            app.sipcfg.local_port = atoi(optarg);
            break;
        case O_LADDR:
            app.sipcfg.local_addr = optarg;
            break;
        case O_NUM:
            app.number = optarg;
            break;
        case O_DUR:
            app.duration = atof(optarg);
            break;
        case O_REP:
            app.report_every = atof(optarg);
            break;
        case O_TXSIL:
            app.tx_silence = 1;
            break;
        default:
            usage(argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    if (!app.sipcfg.host || !app.sipcfg.user || !app.sipcfg.pass || !app.number)
    {
        usage(argv[0]);
        return 2;
    }
    if (app.report_every <= 0)
        app.report_every = 1.0;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* TX pattern + the four RX interpretations */
    prbs_gen_init(&app.gen, PRBS11_MASK);
    prbs_chk_init(&app.chk[0], 0, 0); /* MSB first, normal   */
    prbs_chk_init(&app.chk[1], 1, 0); /* LSB first, normal   */
    prbs_chk_init(&app.chk[2], 0, 1); /* MSB first, inverted */
    prbs_chk_init(&app.chk[3], 1, 1); /* LSB first, inverted */

    su_init();
    app.root = su_root_create(NULL);
    app.rtp = rtp_ep_create(our_rtp_addr(&app), 0, 8, rtp_rx, &app);
    if (!app.rtp)
    {
        LOG("rtp_ep_create failed");
        return 1;
    }

    struct sip_cbs cbs = {on_registered, on_incoming, on_ringing,
                          on_answered,   on_failed,   on_hangup};
    app.ua = sip_ua_create(app.root, &app.sipcfg, &cbs, &app);
    if (!app.ua)
    {
        LOG("sip_ua_create failed");
        return 1;
    }
    sip_ua_register(app.ua);

    watch_fd(&app, rtp_ep_fd(app.rtp), rtp_in);

    LOG("ready: %s@%s, RTP :%d, dialing %s", app.sipcfg.user, app.sipcfg.host,
        rtp_ep_local_port(app.rtp), app.number);

    /* Master clock: fire the 20 ms pump on schedule; between ticks service SIP +
     * the RTP socket with a poll timeout bounded by the next tick. */
    double next_tick = now_ms();
    while (!g_stop)
    {
        double wait = next_tick - now_ms();
        if (wait < 0)
            wait = 0;
        su_root_step(app.root, (su_duration_t)(wait + 0.5));
        rtp_ep_recv(app.rtp); /* drain independent of sofia's fd dispatch */

        double t = now_ms();
        if (t >= next_tick)
        {
            pump_tick(&app);
            do
                next_tick += app.period;
            while (next_tick <= now_ms());
        }
        if (app.media && t >= app.t_next_report)
        {
            print_stats(&app, 0);
            app.t_next_report += app.report_every * 1000.0;
        }
        if (app.media && app.duration > 0 && (t - app.t_answer) >= app.duration * 1000.0)
        {
            LOG("duration reached, hanging up");
            if (app.call)
                sip_call_hangup(app.call);
            break;
        }
    }

    print_stats(&app, 1);

    if (app.call)
        sip_call_hangup(app.call);
    su_root_step(app.root, 200); /* let a BYE go out */

    LOG("shutting down");
    _exit(0);
}
