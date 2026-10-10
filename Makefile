# sip-bert -- PRBS-11 BER tester over SIP / G.711a.
# Reuses the SIP/SDP/RTP modules from apps/sip-modem; links sofia-sip.

CC       ?= gcc
SOFIA_CFLAGS := $(shell pkg-config --cflags sofia-sip-ua)
SOFIA_LIBS   := $(shell pkg-config --libs sofia-sip-ua)
CFLAGS   += -Wall -Wextra -O2 -g $(SOFIA_CFLAGS)

OBJS := main.o prbs.o sip.o sdp.o rtp.o

all: sip-bert

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

sip-bert: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SOFIA_LIBS)

# quick offline self-test of the PRBS generator/checker (no SIP)
prbs_test: prbs_test.o prbs.o
	$(CC) $(CFLAGS) -o $@ prbs_test.o prbs.o

check: prbs_test
	./prbs_test

clean:
	$(RM) *.o sip-bert prbs_test

.PHONY: all check clean
