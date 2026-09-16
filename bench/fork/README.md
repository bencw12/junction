# fork benchmarks

Four plain Linux programs. Build them once and run the same binary natively and
inside a Junction container — the point is that nothing here knows about
Junction, so agreement between the two runs means something.

```
make                                   # fork_latency fork_stress sched_latency as_demo
make static                            # if the container has a restricted fs view
```

## fork_latency — how long until the child runs?

Measures the interval that actually matters to an application: from the
instruction before `fork()` to the first instruction the child executes.

The parent samples the TSC immediately before forking; the child samples it as
its first act and publishes it through a `MAP_SHARED` page. The TSC is invariant
and shared across cores of a socket, so the two samples are directly comparable.
Three latencies are reported: to the child's first instruction, to the parent's
return, and to the child being reaped. Nothing is subtracted; the cost of a
back-to-back TSC read pair is printed so it can be accounted for.

```
./fork_latency -n 1000                 # libc fork()
./fork_latency -n 1000 -r              # raw clone(SIGCHLD), no glibc wrapper
./fork_latency -n 300 -m 64            # parent holds 64 MB of dirty private memory
./fork_latency -n 1000 -t 8            # parent has 8 other live threads
./fork_latency -n 1000 -c              # CSV, for sweeps
```

`-m` is the interesting one: fork latency should grow with the number of pages
the parent has dirtied, because that is what the page-table copy costs. A
`fork()` that did not really copy page tables would be suspiciously flat.

## fork_stress — is it really fork?

Part 1 asserts the observable behaviour POSIX and Linux promise: return values
and parentage; copy-on-write of `.data`, `.bss`, heap, stack and `MAP_PRIVATE`
mappings, in both directions; `MAP_SHARED` staying shared; mappings made after
the fork staying invisible to the other side; descriptor inheritance and shared
file offsets; `FD_CLOEXEC` surviving fork; signal handlers and mask inherited
but pending signals not; exit codes and death-by-signal reported distinctly;
interval timers not inherited; CPU accounting reset; only the calling thread
surviving; forking from a non-main thread; `pthread_atfork` ordering; orphan
reparenting; and a deep fork chain.

Part 2 hammers it: a simultaneous fan-out where every child scribbles a distinct
token over its private copy and re-verifies it while all its siblings are live,
and fork storms from several threads where each child checksums its inherited
memory before dirtying it. A checksum mismatch is what address-space aliasing
would look like. Throughput is reported.

```
./fork_stress                          # everything
./fork_stress -S                       # semantics only
./fork_stress -O -t 4 -s 2 -k 16 -c 4  # stress only: 4 threads, 2 s
```

Exit status is zero only if every check passed. Checks whose underlying system
call is unimplemented report `skip:` rather than failing, so an unrelated gap
does not masquerade as a fork bug.

## sched_latency — what do address spaces cost the scheduler?

Separate address spaces are not free: moving a core from one process's thread to
another's means rebinding the core. Threads of the *same* process should be
unaffected, since the address space does not change. This measures both, so the
claim can be checked instead of assumed.

```
./sched_latency                        # all three
./sched_latency -m yield               # N threads of one process
./sched_latency -m futex               # two threads, one process
./sched_latency -m procs               # two processes: crosses address spaces
```

Run it with the address-space module loaded and again without it: `yield` and
`futex` should be unchanged.

## as_demo — are they really separate address spaces?

Keeps several forked guests alive at once, each verifying its own private memory
in a loop, so an outside observer can look at them. `scripts/as_demo.sh` reads
`/proc/<junction pid>/task/<tid>/maps` for every scheduling thread: procfs
reports the *task's* mm, so threads bound to different address spaces report
different mappings — which an ordinary Linux process cannot do.

```
../../scripts/as_demo.sh
```
