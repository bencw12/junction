# memfs extents never reached a forked child

`echo hi > /tmp/f; ( cat /tmp/f )` killed the subshell with SIGSEGV.

That is the whole reproducer. No scale, no stress, no unusual flags -- an
ordinary shell idiom, and the only propagation failure reachable without
thousands of pipes or threads. `docs/traces/memfs_race.c` is the deterministic
form of it.

```
segfault in syscall handler: addr=380000000000 rip=7f3be21a164d
                             rsp=520000cf9ed8 pid=2 as=4294967297
```

## What was happening

Junction's `/tmp` is memfs, and memfs backs each file with a 256 MB extent of
one large memfd, mapped `MAP_SHARED` at a fixed address. The mapping is made
when the file is created -- which is to say, whenever a guest happens to create
a file, in whichever address space that guest is running in.

A guest address space is a clone of the host process, so it is a snapshot. A
mapping made after the clone exists only in the address space that made it.
When the child then read the file, `MemInode::Read` memcpy'd out of an extent
that its address space had never received.

`MAP_SHARED` is not the point here and does not help: it makes the *bytes* the
same in every address space that maps the extent. It does nothing about an
address space that has no mapping at all. Coherence is not presence.

## Why it hid for so long

The propagation hazard detector classified memfs extents as *guest* memory, and
so said nothing about them across five traced workloads.

It classified by address, and memfs extents started at `0x380000000000` --
below `kVirtualAreaMax` (`0x500000000000`), inside the region Junction hands to
guests. By the only test available at the point of the call, they looked like a
guest's own memory, which is supposed to exist in exactly one address space.

## Root cause: two allocators for one thing

A fault in an extent could not be repaired, because nothing at the fault site
could say what belonged at the faulting address. memfs had two independent
allocators:

| | |
| --- | --- |
| `next_memfs_faddr` | a monotonic address counter, never recycled |
| `allocated_file_slots` | a bitmap of memfd offsets, recycled |

A file's address and its memfd offset were handed out separately and had no
relationship to each other. Measured over 40 create/delete cycles of a single
file: **1 offset, 40 addresses**.

The counter also never recycling is a second bug in its own right: at 256 MB
per creation, the 6144th file creation walks past `kVirtualAreaMax` and starts
handing out addresses inside the guest region.

## The fix

**One allocator.** The slot bitmap is now the only allocator, and the address
is derived from it:

```
addr   = MemFsBase() + slot * kMaxSizeBytes
offset = addr - MemFsBase()
```

The binding for any address in the region is now immutable: it is always
`(memfs_fd, addr - MemFsBase(), RW)`, whatever file owns the slot at the time.
That is what makes lazy repair correct -- see `junction/kernel/arena.h`.

**Its own region, above the guests.** The region is reserved at startup as a
single `PROT_NONE` mapping, before the first clone, so it exists in every
address space and nothing else can be placed inside it. A fault in it is
therefore always a missing *extent*, never a missing region.

The base is chosen at runtime, with `0x600000000000` preferred. A compile-time
constant was tried first and was wrong: the kernel places the LibOS image and
its heap wherever ASLR puts them, and they were observed at `0x575e1a723000`
and `0x58b31b64a000` -- both inside the fixed region that had been picked. The
preferred base is above that band and below the shared libraries at the top of
the address space; if it is ever taken, the kernel picks and the result is
validated.

**A fault handler.** `RepairManagedFault()` maps the enclosing extent at the
matching offset and retries the faulting instruction. No lookup, no lock, and
idempotent, so two cores faulting on the same extent race harmlessly.

**Extents are no longer unmapped when a file is deleted.** `MADV_REMOVE`
punches the object, which propagates through `i_mmap` to every address space
and actually returns the memory; the mapping itself stays. Unmapping would be
the unsafe option: other address spaces materialise extents on demand and would
keep theirs regardless, so an unmap here desynchronises only this one -- and it
punches a hole in the reserved region that the next unrelated `mmap` could be
handed.

## What this changes for users

`kMaxFiles` drops from 128K to 16384, because the region is now reserved
contiguously and 4 TiB is what reliably fits above `kVirtualAreaMax`. 32 TiB
was attempted and could not be reserved: on some runs there was no hole that
size, and on others the region ended at 127.5 TiB, a coin flip at startup.

The old 128K was nominal anyway. The address counter broke at 6144 file
*creations*, so the file count could never get near its own limit. 16384 is now
a limit on concurrently live files, with slots recycled on delete.

## Tests

`scripts/memfs_test.sh`, 13 checks, each run against native Linux first because
"does not crash" is not the bar:

```
native: file created after fork / recycled slot / eight extents      PASS
file created after fork, child reads it                              PASS
  ... and the child exits cleanly                                    PASS
  ... via the fault handler, not by luck                             PASS
a recycled slot serves the new file                                  PASS
eight extents, eight offsets                                         PASS
sh: write a file, read it in a subshell                              PASS
sh: background subshell reads it                                     PASS
extents live above kVirtualAreaMax                                   PASS
no address space diverges afterwards                                 PASS
```

The third check matters: it asserts `arena: repaired a fault in memfs` appears,
so the test cannot pass because the extent happened to be present already.

`docs/traces/memfs_reuse.c` covers the recycled slot -- child materialises a
file's extent, parent deletes it and creates another on the same slot, child
must read the new file. `docs/traces/memfs_many.c` walks eight extents at eight
offsets, which a handler that repaired with a constant offset would fail.
