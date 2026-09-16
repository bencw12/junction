# Fork benchmarks. These are plain Linux binaries: build them once and run the
# same binary natively and inside a Junction container.

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=
LDLIBS  := -lpthread -lm

BINS := fork_latency fork_stress sched_latency as_demo

all: $(BINS)

fork_latency: fork_latency.c timing.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

fork_stress: fork_stress.c timing.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

sched_latency: sched_latency.c timing.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

as_demo: as_demo.c timing.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# Statically linked variants are handy when running under a Junction container
# with a restricted filesystem view.
static: fork_latency_static fork_stress_static

fork_latency_static: fork_latency.c timing.h
	$(CC) $(CFLAGS) -static -o $@ $< $(LDFLAGS) $(LDLIBS)

fork_stress_static: fork_stress.c timing.h
	$(CC) $(CFLAGS) -static -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(BINS) fork_latency_static fork_stress_static sched_latency_static

.PHONY: all clean static
