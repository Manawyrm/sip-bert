/* prbs.c -- see prbs.h */
#include "prbs.h"

/* Sync thresholds:
 *   VERIFY_LEN   : consecutive good predictions to accept a seed (2^-64 false lock)
 *   SYNC_WIN     : loss-of-sync monitor window, in bits
 *   SYNC_LOSS_*  : declare loss of sync when errors/window >= NUM/DEN (30%). A
 *                  desynced stream sits near 50%; a genuinely bad-but-synced line
 *                  never approaches 30%, so this only trips on a real slip.
 */
#define VERIFY_LEN 64
#define SYNC_WIN 1000
#define SYNC_LOSS_NUM 3
#define SYNC_LOSS_DEN 10

/* Next PRBS bit predicted from the current LFSR state.
 * Fibonacci LFSR for x^11 + x^9 + 1: the register holds the last 11 output bits
 * (newest in bit0), so the taps 11 and 9 back are bit10 and bit8. */
static inline int prbs_feedback(uint16_t reg)
{
    return ((reg >> 10) ^ (reg >> 8)) & 1;
}

void prbs_gen_init(struct prbs_gen *g, uint16_t seed)
{
    g->reg = seed ? (seed & PRBS11_MASK) : PRBS11_MASK; /* must be non-zero */
}

void prbs_gen_bytes(struct prbs_gen *g, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++)
        {
            int bit = prbs_feedback(g->reg);
            g->reg = ((g->reg << 1) | bit) & PRBS11_MASK;
            b = (b << 1) | bit; /* MSB first */
        }
        out[i] = b;
    }
}

void prbs_chk_init(struct prbs_chk *c, int lsb_first, int invert)
{
    c->reg = 0;
    c->state = PRBS_HUNT;
    c->fill = 0;
    c->verify_ok = 0;
    c->locked = 0;
    c->lsb_first = lsb_first;
    c->invert = invert;
    c->win_bits = 0;
    c->win_errs = 0;
    c->bits = 0;
    c->errors = 0;
    c->resyncs = 0;
}

void prbs_chk_resync(struct prbs_chk *c)
{
    c->state = PRBS_HUNT;
    c->fill = 0;
    c->locked = 0;
    c->win_bits = 0; /* drop the in-flight window (uncommitted) */
    c->win_errs = 0;
}

void prbs_chk_bit(struct prbs_chk *c, int rx_bit)
{
    rx_bit &= 1;
    if (c->invert)
        rx_bit ^= 1;

    int pred = prbs_feedback(c->reg);

    switch (c->state)
    {
    case PRBS_HUNT:
        /* Load received bits until the 11-bit LFSR is seeded. Any 11 consecutive
         * error-free PRBS bits form a valid state. */
        c->reg = ((c->reg << 1) | rx_bit) & PRBS11_MASK;
        if (++c->fill >= 11)
        {
            c->state = PRBS_VERIFY;
            c->verify_ok = 0;
        }
        break;

    case PRBS_VERIFY:
        /* Trial the seed: the reference now free-runs and must keep matching. */
        if (pred == rx_bit)
        {
            c->reg = ((c->reg << 1) | pred) & PRBS11_MASK;
            if (++c->verify_ok >= VERIFY_LEN)
            {
                c->state = PRBS_LOCK;
                c->locked = 1;
                c->win_bits = 0;
                c->win_errs = 0;
            }
        }
        else
        {
            /* Bad seed (wrong phase or an error in the seed bits): restart the
             * hunt, reusing this bit as the first freshly-loaded seed bit. */
            c->reg = ((c->reg << 1) | rx_bit) & PRBS11_MASK;
            c->fill = 1;
            c->state = PRBS_HUNT;
        }
        break;

    case PRBS_LOCK:
        /* Free-running reference: compare and shift in our OWN predicted bit so a
         * channel error never corrupts the reference. Tally into the current
         * window; commit or discard it only when full. */
        c->win_bits++;
        if (pred != rx_bit)
            c->win_errs++;
        c->reg = ((c->reg << 1) | pred) & PRBS11_MASK;

        if (c->win_bits >= SYNC_WIN)
        {
            if (c->win_errs * SYNC_LOSS_DEN >= c->win_bits * SYNC_LOSS_NUM)
            {
                /* error rate over the window is slip-level, not line-level:
                 * loss of sync. Discard the garbage window and re-acquire. */
                c->resyncs++;
                prbs_chk_resync(c);
            }
            else
            {
                c->bits += c->win_bits;
                c->errors += c->win_errs;
                c->win_bits = 0;
                c->win_errs = 0;
            }
        }
        break;
    }
}

void prbs_chk_bytes(struct prbs_chk *c, const uint8_t *in, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint8_t b = in[i];
        if (c->lsb_first)
        {
            for (int k = 0; k < 8; k++)
                prbs_chk_bit(c, (b >> k) & 1);
        }
        else
        {
            for (int k = 7; k >= 0; k--)
                prbs_chk_bit(c, (b >> k) & 1);
        }
    }
}

double prbs_chk_ber(const struct prbs_chk *c)
{
    return c->bits ? (double)c->errors / (double)c->bits : 0.0;
}
