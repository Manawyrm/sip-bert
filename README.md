# sip-bert

> [!CAUTION]
> 🤖☢️ Vibecoded software!
> This entire repo was written by Claude Code and it does not meet my
> usual quality & care standards. It was not well tested, probably has
> bugs and I take no responsibility if it behaves badly or even hurts
> your computer in any way. Treat this project with the appropriate caution.
> 
> I have tested it myself and it does work, the results seem to be correct at
> least for my test usecase.

A PRBS-11 **bit-error-rate tester** for SIP / G.711a calls — the classic
64 kbit/s ISDN B-channel BERT, reincarnated on VoIP.

A G.711 a-law call is exactly a 64 kbit/s octet pipe (8 kHz × 8 bits/sample).
`sip-bert` serialises an ITU-T O.153 **PRBS-11** pattern (`x¹¹ + x⁹ + 1`,
period 2047) straight into the RTP payload octets, transmits it, and at the same
time synchronises a reference PRBS-11 to whatever it receives and counts bit
errors. That is a true *bit-transparency* test of the media path (RTP loss,
transcoding, DTMF/codec mangling all show up as a non-zero BER).

## Build

```
make            # builds ./sip-bert
make check      # offline self-test of the PRBS generator + checker (no SIP)
```

Requires `sofia-sip-ua` (via `pkg-config`). The SIP/SDP/RTP modules are copied
from `../softmodem/apps/sip-modem`.

## Usage

```
./sip-bert --sip-host H --sip-user U --sip-pass P --test-number N [options]

  --sip-host H       registrar / PBX host
  --sip-user U       auth user + AOR (e.g. 4011)
  --sip-pass P       auth password
  --sip-port N       our local SIP UDP port (default 5060)
  --local-addr A     our IP for SIP/RTP (default 127.0.0.1)
  --test-number N    number to dial (e.g. 8003 BERT line, 8004 echo)
  --duration S       measure for S seconds then hang up (default: until BYE/^C)
  --report S         stats print interval in seconds (default 1)
  --tx-silence       send a-law silence instead of PRBS (RX-only test)
```

### Examples

```
# Receive-side validation against a PBX PRBS generator (bit-perfect if the
# network is clean):
./sip-bert --local-addr 10.23.6.128 --sip-host 10.4.99.1 \
           --sip-user 4011 --sip-pass ****** --sip-port 5069 \
           --test-number 8003 --duration 30

# Full round-trip against an echo / loopback endpoint:
./sip-bert ... --test-number 8004 --duration 30
```

## How it works

* **TX**: a 20 ms pump (160 octets = one RTP/PCMA packet) emits a free-running
  PRBS-11, MSB-first per octet.
* **RX**: each received octet is treated as 8 raw B-channel bits. Because the
  far end's bit packing is not known a priori, **four checkers run in parallel**
  — MSB/LSB octet order × normal/inverted — and the one that locks wins (the
  PBX PRBS line locks `MSB-first, normal`).
* The checker is **self-synchronising and non error-multiplying**: it seeds its
  LFSR from 11 received bits, verifies the seed against 64 following bits, then
  free-runs its own reference and compares. A channel bit error is therefore
  counted exactly once and never corrupts the reference — so the BER number is
  meaningful.
* **Loss of sync is detected by error *rate*, not consecutive errors.** A packet
  loss or gateway loss-concealment event can slip the pattern phase while leaving
  RTP sequence numbers contiguous; a slipped stream mispredicts at ~50% (not
  100%), so counting consecutive errors is blind to it. Instead the checker
  watches the error rate over a sliding 1000-bit window: a window is committed to
  the totals only if it passes, and a window above the loss threshold (30%) is
  discarded as a slip — the checker re-hunts and re-acquires. So a disturbance
  shows up as a bounded step in the error count plus a `resync`, and the BER
  recovers instead of climbing forever.
* RTP sequence numbers give ordered, de-duplicated delivery; a sequence gap
  (whole lost frame) also forces a re-sync and is reported separately as
  `frames lost`.
* Set `BERT_DEBUG=1` in the environment to dump per-checker internal state each
  report interval.

## Output

```
[   5.0s] LOCK(msb    ) bits=319925 err=0 BER=0.00e+00 | rtp tx=250 rx=250 lost=0 reord=0 resync=0
...
---- BERT result ----
  pattern     : PRBS-11 (x^11+x^9+1), MSB-first octet order
  bits        : 639925
  bit errors  : 0
  BER         : 0.000e+00  (error-free)
  resyncs     : 0
  rtp packets : tx=500 rx=500 bad=0
  frames lost : 0  (reordered/dup dropped: 0)
```

`HUNT` in place of `LOCK` means no PRBS-11 stream has been acquired yet (or the
media is not a PRBS pattern at all).
