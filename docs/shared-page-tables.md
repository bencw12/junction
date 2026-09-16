# Sharing the LibOS's page tables

## The problem pooling does not solve

Junction's guests live in separate address spaces, each created by cloning the
host process. A mapping made *after* a clone exists only in the address space
that made it. Reserving Caladan's stack and large-page pools up front hides the
two cases we knew about; it does not remove the class. Anything else the LibOS
maps at runtime — glibc extending the heap past its pinned configuration, a
`dlopen`, a new memory-map reservation, a memfd — still lands in exactly one
address space, and faults in every guest forked before it.

The fix is to stop copying the LibOS's page tables and start sharing them. If
every cloned `mm_struct` points at the *same* page-table subtree for the LibOS's
address range, then a mapping created while any address space is on a core is
immediately visible from all of them, because there is only one set of tables.

This is what Linux already does for kernel memory: every `mm` carries the same
top-level entries for the kernel half. It is also what Dune does, though by a
different route — see the end of this document.

## Layout: the LibOS and guests must not share a top-level slot

Page tables can only be shared at the granularity of a paging level. On x86-64
with 4-level paging, one top-level entry covers 512 GB, so the LibOS and the
guests must occupy disjoint 512 GB slots.

This machine's CPU does not support `la57`, so 4-level paging is active and a
top-level entry covers 512 GB. The kernel is built `CONFIG_PGTABLE_LEVELS=5`
with p4d folded, so code written against `p4d_offset()` is correct either way:
when p4d is folded it resolves to the pgd entry, and when 5-level paging is
active it is the entry that covers 512 GB. That is the level we share.

Guests are confined below `kVirtualAreaMax` (`0x500000000000`, slot 160), so
everything the kernel places for us — the binary and heap, glibc, the main
stack — is already above the guests and needs no relocation. Only Caladan's two
pools sat inside the guest range, and they are placed by us, so moving them is a
change of two constants.

| region | before | after | slot |
| --- | --- | --- | --- |
| Caladan large-page pool | `0x100000000000` | `0x510000000000` | 162 |
| Caladan stack pool | `0x200000000000` | `0x520000000000` | 164 |

The low fixed mappings at `0x200000` (the zpoline trampoline) stay where they
are and are *not* shared: they are read-only after initialization, so inheriting
them by copy-on-write is correct and cheaper.

## TLB: the part that decides whether this is safe

### What Linux actually does

Linux does not flush all TLBs. On x86:

- `flush_tlb_mm_range()` chooses between per-page `INVLPG` and a full flush at
  `tlb_single_page_flush_ceiling`, which is 33 pages on this machine
  (`/sys/kernel/debug/x86/tlb_single_page_flush_ceiling`).
- Shootdown IPIs go only to the CPUs in `mm_cpumask(mm)`, via
  `on_each_cpu_mask()` — not to every CPU.
- The receiving CPU's `flush_tlb_func()` compares the flush against
  `cpu_tlbstate.loaded_mm`. If that CPU is not currently running the mm being
  flushed, it records a TLB generation and returns **without invalidating
  anything**.
- PCID and INVPCID are available and in use here, so switching address spaces
  does not flush. Staleness is caught later by comparing `mm->context.tlb_gen`
  when the mm is switched back in.
- The single exception is `flush_tlb_kernel_range()`, which does go to every
  CPU — precisely because kernel mappings live in every page table.

### Why that is a hazard for shared tables

A guest's address space has no VMA covering the LibOS; only the root address
space does. So when the kernel unmaps a LibOS page, reverse mapping finds the
root's VMA, and the PTE it clears is in the shared table, so the clearing itself
is visible everywhere — that part is fine.

The flush is not. It targets `mm_cpumask(root_mm)`, and a core running a guest
address space is not in it. Worse, even if the IPI were delivered,
`flush_tlb_func()` would see `loaded_mm` is the guest's mm, not the root's, and
skip the invalidation as a lazy-TLB case. The core keeps a valid TLB entry for a
page that has been freed and reused: a use-after-free.

Adding those cores to `mm_cpumask(root_mm)` does **not** fix this, for the
second reason above — the IPI arrives and is then discarded. Making the flush
unconditional would mean treating the range like kernel memory, which means
intruding on the flush path itself.

### The resolution: make the range never need a flush

Rather than make shootdowns correct across shared tables, remove the events that
cause them. A TLB flush of a user address is required when, and only when:

| event | needed? | how it is eliminated |
| --- | --- | --- |
| unmap / free | yes | the LibOS range is allocate-only; nothing is unmapped until the process exits |
| permission downgrade (`mprotect` to less) | yes | no `mprotect` on the range after initialization |
| reclaim / swap-out | yes | the range is pinned (`mlock`) or hugetlb |
| migration / compaction / THP collapse | yes | pinned; THP disabled for the range |
| copy-on-write break | yes | the range is shared, never COW |
| dirty-tracking write-protect | yes | anonymous and hugetlb only, no writeback |
| **populating a not-present PTE** | **no** | — |

The last row is what makes this work. Establishing a new mapping requires no
invalidation on x86: nothing needs to be evicted, because nothing was cached for
a non-present entry. A spurious fault from a stale non-present translation is
handled by `flush_tlb_fix_spurious_fault()` on the faulting core.

So the case this whole design exists to fix — the LibOS allocating memory after
an address space has been cloned — propagates through the shared tables with
**no invalidation and no IPI at all**.

### Why this does not cost Junction what it avoids

Junction and Caladan go out of their way to avoid IPIs, so it is worth being
precise about what is added: nothing, in the steady state. Guest execution,
guest faults, guest system calls, and LibOS allocation all proceed without
shootdowns. Flushes remain possible only for the six events above, all of which
are eliminated by pinning the range, and all of which are *already* a hazard in
the existing design.

In fact this removes an IPI source that exists today: `stack_reclaim()` calls
`MADV_REMOVE` on freed Caladan stacks, which is a real unmap and therefore a
real shootdown, on a path that runs during fork-heavy workloads. Pooled stacks
now stay resident, bounded by the high-water mark of concurrent uthreads.

The invariant to hold onto, and to check when changing this code:

> **No mapping in the LibOS range is ever unmapped, moved, write-protected, or
> reclaimed while Junction is running.**

## Lifetime: clearing the shared entries before teardown

A cloned address space must not be destroyed while it still points at the shared
subtree. `exit_mmap()` walks the page tables and frees them, so it would free the
LibOS's tables and drop references on the LibOS's pages.

The entries are therefore cleared from an `mmu_notifier` `->release` callback,
which `exit_mmap()` invokes before it walks anything. Clearing at that point is
race-free: the address space is already dying, so no core can still be bound to
it. Clearing eagerly at release time instead would be wrong, because a
scheduling core may still be bound to the address space it is releasing.

## Relationship to prior work

**Dune** does not do this. `dune_vm_clone()` (`libdune/vm.c`) walks the whole
tree to the leaves and allocates fresh intermediate tables in the new root; only
the leaf physical pages are shared, refcounted and copy-on-write via `PTE_COW`.
Dune does not need shared tables because libdune *owns* its page tables —
nothing allocates into them behind its back, so it can always propagate. Our
problem exists because Linux owns them and will not. Dune's answer to the
"map everything" problem is `dune_init(map_full)`, which pre-maps the entire
address space — the same trick as pooling, with the same weakness.

**mshare** is the upstream attempt at general page-table sharing between
unrelated processes, originally from Oracle and now carried by Anthony Yznaga.
It exposes an `msharefs` filesystem where a region gets its own `mm_struct` to
own the page tables, and each sharer maps it through a "window VMA". That window
VMA is how mshare keeps reverse mapping working; we avoid needing one by making
the range flush-free instead. mshare has been in review for years without
landing, which is the right calibration for how carefully this needs to be done
— though mshare must support general shared memory, while we need only an
allocate-only, pinned region.

**hugetlbfs** already shares page tables at PMD granularity in-tree
(`huge_pmd_share`), which is the existing proof that the technique is viable in
mainline.


## Revisited: why the memfd does not inherit the TLB problem

Asked later in the design discussion: if shared maple-tree nodes were rejected
over TLB shootdowns, why does a shared memfd not have the same flaw? If mm A
maps part of the memfd and touches it, and mm B punches a hole, does A's TLB
entry get shot down?

**It does.** Measured (`docs/traces/tlb_punch.c`): the child touches the page on
CPU 3, then spins in a pure userspace loop -- no syscalls, nothing that would
force a TLB refill -- while the parent punches the hole from CPU 1.

```
child : observed 0x00 after 2617.7 us -> shootdown reached this mm
```

Had the entry been stale, the child would have read `'A'` forever; the test
fails after 3 s in that case. (Separate processes rather than Junction address
spaces, but structurally identical for this purpose: distinct `mm_struct`s,
distinct PCIDs, one memfd.)

### The mechanism, and why it is not the same case

The invalidation protocol is per-mm. TLB entries are tagged with a **PCID**, a
per-CPU slot keyed by `mm->context.ctx_id`:

```c
struct tlb_context { u64 ctx_id; u64 tlb_gen; };
struct tlb_state { ... struct tlb_context ctxs[TLB_NR_DYN_ASIDS]; };  /* 6 */
```

To invalidate, the kernel bumps that mm's `tlb_gen` and IPIs `mm_cpumask(mm)`.
A CPU not currently running the mm need not flush now -- it notices the
generation gap when it next switches in. So the correctness requirement is not
"send an IPI"; it is **every mm holding the page must have its `tlb_gen`
bumped**. Missing the bump is the bug.

Who gets bumped is decided by the rmap walk, which enumerates **per VMA**:

```c
bool (*rmap_one)(struct folio *folio, struct vm_area_struct *vma,
                 unsigned long addr, void *arg);
```

and a VMA belongs to exactly one mm:

```c
struct mm_struct *vm_mm;	/* The address space we belong to. */
```

**Shared maple-tree nodes / shared page tables**: N mms, *one* VMA. rmap finds
one `vm_mm`, bumps one `tlb_gen`. The other N-1 mms never learn, see no
generation gap on their next switch-in, and reuse stale entries under their own
PCIDs.

**Shared memfd**: N mms, *N* VMAs, all present in the file's `i_mmap` ("Tree of
private and shared mappings"). The hole punch walks `i_mmap` and calls
`rmap_one` once per VMA, each with a distinct `vm_mm`. Every mm gets its PTEs
cleared and its `tlb_gen` bumped. Native, no special handling.

So the distinction is not "memfd versus page tables" -- it is **N VMAs versus
one VMA**. What the memfd buys is making N separate VMAs both *legal* (they need
a shared backing object to all refer to) and *free* (they resolve to the same
physical pages). Page-table sharing tries to let one VMA stand in for N mms, and
rmap has no way to express that.

### Could the sharing approach be fixed?

Only by making `vma->vm_mm` a set, which changes rmap's core interface: every
`rmap_one` implementation -- `try_to_unmap_one`, `page_mkclean_one`,
`folio_referenced_one`, migration -- assumes one mm per VMA. That is a core-mm
patch series, not a module.

What it would buy over the memfd: no per-mm VMA memory (~100 VMAs at ~200 bytes
is negligible) and no per-mm fault to populate (a few hundred faults per address
space, one-time, sub-millisecond). Neither justifies the change.
