# Status: LibOS memory under multiple address spaces

Checkpoint of a design discussion. The full argument is in
`docs/libos-memory-plan.md`; this is the short version of where things stand.
Raw evidence is in `docs/traces/`.

## Done and verified

- **Multi-address-space `fork()`** — 46/46 semantics checks matching native
  Linux, 13/13 kernel-module checks, ~142 us fork-to-child, ~3.9K forks/s.
  `scripts/fork_test.sh`.
- **Startup sweep** — `ShareLibOSMemory()` converts private-writable LibOS
  mappings to shared before the first guest, and `AuditLibOSMemory()` checks the
  result. At steady state, only two private-writable LibOS mappings remain, both
  deliberate (the clone stack, the initial thread stack).
- **Instrumentation** — `--trace_libos_mem` records every mapping operation with
  attribution; `--debug_libos_alloc` and `--debug_libos_escape` provoke the
  cases that do not occur naturally. `scripts/memtrace_report.sh` summarises.

## Measured

| finding | evidence |
| --- | --- |
| LibOS allocation events after init: **0** across 5 workloads | `trace_{openssl,py,py2,fork,wide}.txt` |
| Caladan pools never overflowed, even at 512 concurrent guests | `trace_wide.txt` |
| Caladan has exactly **2** non-init mapping sites, both pooled and hooked | `stack_create`, `lgpage_create` |
| **memfs extents are a real, reachable bug** — fork, parent creates a file, child reads it, segfault | `memfs_race.c`, `trace_race.txt` |
| `ksys_mmap` never traps, so ~14 call sites cannot be intercepted | `--debug_libos_escape` |
| `!preempt_enabled()` misroutes the LibOS's own `brk` to the guest syscall table | `--debug_libos_alloc` |
| `fsbase` is guest-settable (`usys_arch_prctl` does not validate) | code |
| glibc `realloc` relocates via `mremap(MREMAP_MAYMOVE)` | `realloc_probe.c` |
| `PUNCH_HOLE` shoots down other mms' TLB entries correctly | `tlb_punch.c` |
| memfd enforces its size at **fault** time (SIGBUS), not at mmap time | `memfd_bounds.c` |
| memfs recycles the memfd slot but never the address (40 addrs, 1 offset) | `trace_churn.txt` |

## Known bugs, reproduced

| bug | reproducer | status |
| --- | --- | --- |
| **a guest opening 17000 pipes crashes a forked child** — LibOS objects <256 KB come from Caladan's large-page pool via `new_override.cc` | `scripts/pipe_heap_test.sh` | open, crashes 3/3, **pure guest program** |
| memfs extent mapped after a fork is invisible to the child | `docs/traces/memfs_race.c` | open, segfaults 3/3 |
| Caladan stack pool overflow hands a forked child a stack its address space never mapped | `scripts/stack_overflow_test.sh` | open, crashes 5/5 |
| `!preempt_enabled()` misroutes the LibOS's own `brk` to the guest syscall table | `--debug_libos_alloc` | **fixed** (Phase 0a) |

| `log_*_ratelimited` suppressed the first occurrence for the first second, hiding both pool-overflow hazards | 23 call sites | **fixed** |

`docs/bug-stack-pool-overflow.md` and `docs/bug-ratelimited-logging.md` have the
full write-ups. `docs/shared-memory-audit.md` enumerates every remaining path.

## The design

**Per-mm VMAs over a memfd-backed LibOS arena, with a userspace shadow VMA map
as ground truth, propagated lazily.**

The memfd matters because rmap enumerates **per VMA** and `vma->vm_mm` is
single-valued: N mms with N VMAs over one memfd get N correct TLB flushes, while
N mms sharing one VMA get one. The memfd does not avoid shootdowns — it makes
them visible. That is why page-table and maple-tree sharing were rejected.

Correctness rests on one invariant:

> An address range is never reused for a different `(fd, offset, prot)` binding
> until it has been unmapped from every mm that materialised it.

With it, every divergence becomes an absence, absences fault, and the fault
handler fixes them lazily. Nothing eager, nothing on the mm-switch path.

- **free / mremap** = local operation + `fallocate(PUNCH_HOLE)`. The kernel
  walks `i_mmap` and flushes every mm, so no userspace list of mms is needed.
- **quarantine** = the freed address is not reused. Punch reclaims the memory;
  quarantine guarantees nothing aliases the address.
- **GC** = the only operation that visits other mms. Batched, rare, off every
  critical path.

## Not started

1. Consolidate the sweep onto one arena memfd (it currently makes 27) and seed
   the shadow map from it.
2. Arena allocator: reuse `ExclusiveIntervalSet::FindFreeRange` over the slot,
   pool-allocated and eagerly mapped in every address space (bootstrap: the
   fault handler must not fault).
3. Redirect at the SIGSYS handler for `mmap`/`mremap`/`munmap`, discriminating
   on **trapping RIP** (not `fsbase`, not preemption state).
4. Edit the ~14 `ksys_mmap`/`KernelMMap` call sites.
5. Assertions in `KernelMRemap`/`KernelMProtect` so a new call site fails loudly.
6. Fix memfs: one interval set over a reserved slot, restoring the
   address<->offset identity and the address leak (6144 creations reaches
   `kVirtualAreaMax` today).
7. Extend `AuditLibOSMemory()` to diff the LibOS range across live address
   spaces.

## directpath

Every trace ran with directpath disabled (the iokernel had `--vdev=net_tap0`),
so this was the outstanding gap. Resolved by call-chain analysis rather than
measurement; see below for why the measurement was not run.

**Junction-side mapping sites: two, both in `mlx5_init_ext_late`.**

| site | what | flags |
| --- | --- | --- |
| `mlx5_init_external.c:117` | the iokernel's shared memfd region | `MAP_SHARED` |
| `mlx5_init_external.c:189` | the assigned BAR page (UAR doorbell) | `MAP_SHARED` |

Both are reached by exactly one path, and it is init-only:

```
runtime_init (init.c:240)
  -> ioqueues_register_iokernel (ioqueues.c:359)
    -> setup_external_directpath (ioqueues.c:315)
      -> mlx5_init_ext_late (ioqueues.c:347)
```

`runtime_init` completes before Junction's `init()` runs — Junction's init is
the `main_fn` handed to it — so both mappings exist *before*
`ShareLibOSMemory()` sweeps, and being `MAP_SHARED` the sweep correctly skips
them and `clone()` inherits them coherently. They need nothing new.

The other three directpath mapping sites (`allocate_user_memory`, `free_ctx` in
`iokernel/directpath/core.c`) are in the **iokernel**, a separate process. They
never touch Junction's address space; what reaches Junction is the memfd they
create, which is the `mem_fd` above.

So the answer to "is there something in the networking code that is hard to
reason about": no. Directpath adds two init-time shared mappings, and the design
already covers that class.

### The UAR mapping: inheritance is required, not a hazard

This was framed backwards earlier. The doorbell is **per kthread**, not per
guest:

```
runtime/ioqueues.c:371     hdr->thread_count = maxks;
iokernel/directpath/core.c alloc_raw_ctx(p->thread_count, ...)  /* nr_qs */
```

One queue per kthread, `maxks` fixed at init from `runtime_kthreads`, allocated
once at process registration. Junction's kthreads run whichever address space is
currently bound, so a kthread must be able to ring its doorbell no matter which
guest is loaded. The UAR page therefore *has* to be mapped, at the same address,
in every address space — which is exactly what `clone()` inheritance provides.
It is the required behaviour, not a risk to be checked.

**And the mechanism is the same one the whole design rests on.** The BAR mapping
is a *file* mapping of the VFIO device fd, populated lazily:

```
vfio_pci_mmap_fault        [vfio_pci_core]   /* fault-populated, not eager */
vfio_pci_mmap_ops          [vfio_pci_core]
vfio_pci_zap_and_down_write_memory_lock
$ nm -u vfio-pci-core.ko | grep unmap
                 U unmap_mapping_range
```

`unmap_mapping_range()` walks the file's `address_space->i_mmap`, which contains
one VMA per mm that maps the device. So a device reset zaps every address space
and flushes every mm's TLB — the identical argument to the memfd, for the
identical reason: **N mms, N VMAs, one file, `i_mmap` finds them all.** And
because population is lazy, a cloned mm that has the VMA but no PTE simply
faults and `vfio_pci_mmap_fault` restores it. The memory type rides along in
`vma->vm_page_prot`, which is copied with the VMA.

So directpath is fully accounted for by reading. Nothing about it is a residual
correctness risk for multiple address spaces.

**Do the mappings ever change after init?** No. Two mmap sites in the entire
runtime directpath tree, both in `mlx5_init_ext_late`, reached once from
`runtime_init`. Queue count is fixed at `maxks`; the iokernel grants and revokes
*cores*, so kthreads park and unpark, but the set of kthreads and their queues
does not change.

### Why the measurement was not run

Enabling directpath is not an iokernel restart. `lib/caladan/scripts/setup_vfs.sh`
restarts the Mellanox drivers machine-wide (`/etc/init.d/openibd restart`),
switches the ConnectX-7's eswitch into **switchdev mode**, creates SR-IOV VFs,
binds one to `vfio-pci`, and installs a set of `tc` flower rules across the
uplink and its representors. `400gp1` currently holds `10.40.1.105/16`; after
that procedure the Linux-visible interface is a VF representor instead.

Prerequisites are all present (`intel_iommu=on,sm_on`, vfio loaded, 5192
hugepages, `sriov_totalvfs=16`), and SSH is on `eno1` so the session is not at
risk. But this reconfigures a shared machine's 400G fabric in a way that is
awkward to reverse, so it needs to be a deliberate decision.

To restore the current iokernel afterwards:

```
sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 -- --allow 00:00.0 --vdev=net_tap0
```

### Ready to run

`scripts/directpath_trace.sh` does the whole thing in one command: VF setup,
iokernel with `vfio nicpci <vf>`, the trace workloads against
`caladan_directpath.config`, and restore on exit (including on failure, via a
trap). `docs/traces/nic_restore.sh` restores by hand if the trap is missed;
`docs/traces/nic_restore.txt` is the captured pre-change state
(legacy eswitch, no VFs, mlx5_core, `400gp1` 10.40.1.105/16 mtu 9000).

The two mlx5 mappings are now instrumented via Caladan's existing
`on_runtime_map()` weak hook, so they appear in the trace as
`directpath-memfd` and `directpath-uar-mmio`. Without this they were invisible
to the tracer — raw `mmap` in Caladan, hooked by neither the Junction wrappers
nor anything else — so the run would have shown nothing and looked like a pass.

## Open

- **The directpath run itself.** Blocked in this session: modifying shared
  machine resources (stopping the iokernel, reconfiguring the NIC) is denied by
  the sandbox's auto-mode policy. Needs either a granted permission or a hand
  run of `scripts/directpath_trace.sh`.
- What it would add is now confirmation rather than discovery: that the trace
  shows only the two expected `directpath-*` events and no others, that the
  write-combining memory type survives the clone, and that packets actually flow
  from a forked child. The structural question — does directpath break multiple
  address spaces — is answered: no.
