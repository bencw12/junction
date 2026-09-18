# The pre-reserved pools are load-bearing, not an optimisation

> **Status: the pools are gone.** Phase 3a of `docs/fork-implementation-plan.md`
> replaced both with `runtime_mem_region` (`lib/caladan/base/mem.h`): the whole
> index space reserved `PROT_NONE`, each slice mapped from a memfd on first use
> at a fixed `offset = addr - base`, freed by punching with the mapping kept,
> and repaired on fault from any address space. Everything below is the
> measurement that showed why the pools could not simply be removed -- and why
> the fix had to make their absence *repairable* rather than pre-reserve more.
> The costs listed under "What the pools cost" are the ones it removes.

Two branches in `lib/caladan` make the point:

| branch | what it is |
| --- | --- |
| `mas-pools` | the work: stack and large-page pools pre-reserved as single `MAP_SHARED` mappings before the first clone |
| `no-pools` | identical, minus the up-front reservation — addresses stay relocated above the guest region, `mem_map_anom` stays `MAP_SHARED`, allocation is on demand as upstream did |

The branches differ in exactly one property: whether the region exists before
the first address space is cloned.

## One ordinary shell command separates them

```
$ junction_run cfg -- /bin/sh -c 'echo hi > /tmp/f; ( cat /tmp/f )'

mas-pools    hi          (3/3)
no-pools     fault with preemption disabled: addr=510000a0e9d0
             pid=1 as=0 want_as=0          (3/3)
```

`0x510000a0e9d0` is inside `PAGE_BASE_ADDR`, the large-page pool. A LibOS object
was allocated in one address space and dereferenced from another.

**`MAP_SHARED` gives coherence but not presence.** On `no-pools` the mapping is
still shared; it is simply absent from address spaces that were cloned before it
was made. Sharing the backing object is not enough — the *mapping* has to exist
in each address space, which is what pre-reserving before the first clone
achieves.

Not everything breaks without the pools — backgrounded execs, pipelines, command
substitution and `python3` subprocess all still pass, 0/3. The exposure needs a
LibOS object to be allocated under one address space and used from another.

## What the pools cost

**RSS becomes a high-water mark, not current usage.** Upstream returned freed
memory to the OS; the pooled path does not.

```c
/* upstream */                         /* mas-pools */
static void lgpage_destroy(...) {      static void lgpage_destroy(...) {
    munmap(lgpage_to_addr(pg),             if (index >= nr_pooled_lgpages)
           PGSIZE_2MB);                        munmap(...);
```

Measured, holding then closing 3000 pipes (64 KB of LibOS buffer each):

```
t=2s   pool Rss=190.3 MB   3000 pipes open
t=8s   pool Rss=190.3 MB   all closed
t=17s  pool Rss=190.3 MB   all closed
```

Nothing comes back. The stack pool behaves the same way — `stack_reclaim()` does
call `MADV_REMOVE` for pooled stacks, which is correct, but instrumenting it
showed it is **never called**: freed stacks stay in per-kthread tcache magazines
and never reach `stack_tcache_free()`. 8000 threads created and joined produced
zero calls, and the pool held 31.5 MB throughout.

Slab fragmentation compounds this: a 2 MB page is only free when every object in
it is free, so the high-water is worse than peak live bytes.

The other costs are 17 GB of reserved address space (16 GB stacks, 1 GB pages),
sized by guesswork, and a hard cliff at each pool's edge
(`docs/bug-stack-pool-overflow.md`,
`docs/bug-guest-driven-libos-allocation.md`).

## Why this matters for the plan

The pools are the current mitigation for the propagation bug, and they are not
free: unreturned memory, fragmentation, 17 GB of address space, and a crash past
each edge. Fixing propagation properly
(`docs/fork-implementation-plan.md`) is what lets them be deleted — so the full
fix is not additional work on top of the pools, it is what removes them.
