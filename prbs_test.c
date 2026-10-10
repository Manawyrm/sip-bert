/* prbs_test.c -- offline validation of the PRBS-11 generator + checker. */
#include "prbs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg)                                                                  \
    do                                                                                     \
    {                                                                                      \
        if (!(cond))                                                                       \
        {                                                                                  \
            printf("FAIL: %s\n", msg);                                                      \
            fails++;                                                                        \
        }                                                                                  \
        else                                                                               \
            printf("ok:   %s\n", msg);                                                      \
    } while (0)

/* PRBS-11 has period 2^11 - 1 = 2047 bits. Verify the bit sequence repeats with
 * exactly that period and not sooner. */
static void test_period(void)
{
    /* Unpack one contiguous bit stream (2047 isn't a byte multiple, so we must
     * NOT restart per-2047 or we'd drop the odd trailing bits and desync). */
    struct prbs_gen a;
    prbs_gen_init(&a, PRBS11_MASK);
    enum
    {
        NB = 2047 * 2 + 16
    };
    static uint8_t seqa[NB];
    int nbytes = (NB + 7) / 8;
    uint8_t bytes[(NB + 7) / 8];
    prbs_gen_bytes(&a, bytes, nbytes);
    int idx = 0;
    for (int i = 0; i < nbytes && idx < NB; i++)
        for (int k = 7; k >= 0 && idx < NB; k--)
            seqa[idx++] = (bytes[i] >> k) & 1;

    int period_ok = 1;
    for (int i = 0; i < 2047; i++)
        if (seqa[i] != seqa[i + 2047])
            period_ok = 0;
    CHECK(period_ok, "PRBS-11 repeats with period 2047");

    /* and it must NOT repeat at 1023 (a shorter divisor-ish check) */
    int early = 1;
    for (int i = 0; i < 1023; i++)
        if (seqa[i] != seqa[i + 1023])
            early = 0;
    CHECK(early == 0, "PRBS-11 does not repeat early (not degenerate)");

    /* max run of zeros in one period must be exactly 10 for a maximal sequence */
    int run = 0, maxrun = 0;
    for (int i = 0; i < 2047; i++)
    {
        if (seqa[i] == 0)
        {
            run++;
            if (run > maxrun)
                maxrun = run;
        }
        else
            run = 0;
    }
    CHECK(maxrun == 10, "max run of zeros is 10 (maximal-length)");
}

/* Clean stream: the checker must lock and report zero errors. */
static void test_clean_lock(void)
{
    struct prbs_gen g;
    struct prbs_chk c;
    prbs_gen_init(&g, 0x123);
    prbs_chk_init(&c, 0, 0); /* MSB first, normal */

    uint8_t buf[1000];
    prbs_gen_bytes(&g, buf, sizeof(buf));
    prbs_chk_bytes(&c, buf, sizeof(buf));

    CHECK(c.locked, "checker locks on a clean MSB-first stream");
    CHECK(c.errors == 0, "clean stream => zero bit errors");
    CHECK(c.bits >= 6000, "clean stream => most bits measured after lock");
}

/* Inject a known number of bit errors; the non-propagating checker must count
 * exactly that many (each isolated flip = one error, no multiplication). */
static void test_error_count(void)
{
    struct prbs_gen g;
    struct prbs_chk c;
    prbs_gen_init(&g, 0x123);
    prbs_chk_init(&c, 0, 0);

    uint8_t buf[2000];
    prbs_gen_bytes(&g, buf, sizeof(buf));
    /* flip a handful of well-separated bits, all comfortably after lock */
    int flips[] = {600, 700, 900, 1200, 1500};
    int nf = sizeof(flips) / sizeof(flips[0]);
    for (int i = 0; i < nf; i++)
    {
        int byte = flips[i];
        buf[byte] ^= 0x10; /* flip one bit in the octet */
    }
    prbs_chk_bytes(&c, buf, sizeof(buf));

    CHECK(c.locked, "checker stays locked through isolated errors");
    CHECK(c.errors == (unsigned)nf, "counts exactly the injected bit errors (no multiply)");
}

/* LSB-first and inverted streams must be picked up by the matching checker only. */
static void test_interpretations(void)
{
    struct prbs_gen g;
    uint8_t buf[1000];

    /* LSB-first: bit-reverse each octet of a normal MSB stream */
    prbs_gen_init(&g, 0x1);
    prbs_gen_bytes(&g, buf, sizeof(buf));
    uint8_t rev[1000];
    for (int i = 0; i < 1000; i++)
    {
        uint8_t x = buf[i], r = 0;
        for (int k = 0; k < 8; k++)
            r = (r << 1) | ((x >> k) & 1);
        rev[i] = r;
    }
    struct prbs_chk lsb;
    prbs_chk_init(&lsb, 1, 0);
    prbs_chk_bytes(&lsb, rev, sizeof(rev));
    CHECK(lsb.locked && lsb.errors == 0, "LSB-first checker locks on bit-reversed octets");

    /* inverted */
    uint8_t inv[1000];
    for (int i = 0; i < 1000; i++)
        inv[i] = ~buf[i];
    struct prbs_chk mi;
    prbs_chk_init(&mi, 0, 1);
    prbs_chk_bytes(&mi, inv, sizeof(inv));
    CHECK(mi.locked && mi.errors == 0, "inverted checker locks on inverted octets");

    /* the plain MSB checker must NOT lock on inverted data */
    struct prbs_chk plain;
    prbs_chk_init(&plain, 0, 0);
    prbs_chk_bytes(&plain, inv, sizeof(inv));
    CHECK(!plain.locked, "MSB/normal checker does NOT lock on inverted data");
}

/* Phase slip: a run of PRBS, then some octets vanish (as a lost/concealed frame
 * would slip the pattern), then clean PRBS resumes at the new phase. The slip
 * region mispredicts at ~50% -- NOT ~100% -- so consecutive-error detection is
 * blind to it. The checker must instead notice the windowed error rate, drop
 * sync, re-acquire, and NOT flood the error count. (This is the bug the tool
 * showed on a real lossy line.) */
static void test_phase_slip(void)
{
    struct prbs_gen g;
    struct prbs_chk c;
    prbs_gen_init(&g, 0x2AA);
    prbs_chk_init(&c, 0, 0);

    uint8_t buf[4000];

    prbs_gen_bytes(&g, buf, sizeof(buf)); /* clean run -> lock */
    prbs_chk_bytes(&c, buf, sizeof(buf));
    CHECK(c.locked && c.errors == 0, "phase-slip: locked and clean before the slip");

    uint8_t drop[37];
    prbs_gen_bytes(&g, drop, sizeof(drop)); /* octets that never reach us -> slip */

    prbs_gen_bytes(&g, buf, sizeof(buf)); /* clean PRBS at the slipped phase */
    prbs_chk_bytes(&c, buf, sizeof(buf));

    CHECK(c.resyncs >= 1, "phase-slip: loss of sync detected (windowed, not consecutive)");
    CHECK(c.locked, "phase-slip: re-locked on the resumed pattern");
    /* old consecutive-run logic would count ~50% of the ~32000 post-slip bits
     * (~16000 errors) and never recover; the fix keeps it under one window. */
    CHECK(c.errors < 1000, "phase-slip: slip is NOT billed as an error flood");
}

int main(void)
{
    test_period();
    test_clean_lock();
    test_error_count();
    test_interpretations();
    test_phase_slip();
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
