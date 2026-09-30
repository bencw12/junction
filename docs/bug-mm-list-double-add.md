# Host panics under many sandboxes: a double `lru_gen_add_mm()` in junction_as.ko

**Root cause found and fixed** (kern/junction_as.c, `jas_adopt()`). Five kernel
panics and two host lockups on this machine (2026-09-21 .. 09-23), all under
128 or more concurrent Junction sandboxes with 4 kthreads each, had one cause.

## Symptoms, in the order they were chased

| when | what the kernel said | where |
| --- | --- | --- |
| 09-21 07:44 | `kernel BUG at fs.h:2906` (i_readcount underflow) in `__fput` | ovl backing file read |
| 09-21 08:41 | task blocked forever in `vfs_link` (inode rwsem never released) | ovl_link on a tmpfs upper |
| 09-21 09:57, 09-23 02:08 | soft lockup, many CPUs | *(separate bug, see below)* |
| 09-23 07:31 | NULL deref in `__destroy_inode` from `kill_litter_super` | umount of a tmpfs upper |
| 09-23 09:14 | NULL page in `ext4_finish_bio` | ext4 writeback kworker |

Every symptom is a filesystem object -- a file, an inode, a page -- freed or
clobbered under someone still using it. None pointed at the culprit. Each one
was in a different subsystem, so each in turn looked like an overlayfs bug, a
tmpfs bug, then an ext4 bug; bisecting the upper layer between tmpfs and disk
gave four clean cells in a row and then a crash on disk too. The real
discriminator was `kthreads=4` cells (rate of forks and address-space
releases), not the filesystem.

## What found it

KFENCE (`/sys/module/kfence/parameters/sample_interval`, off by default on
this kernel) at a 2 ms sampling interval reported within ten minutes:

```
BUG: KFENCE: use-after-free write in lru_gen_add_mm+0xf8/0x120
  lru_gen_add_mm <- kernel_clone <- __do_sys_clone            (into a freed names_cache object)
BUG: KFENCE: use-after-free write in lru_gen_del_mm+0x104/0x1a0
  lru_gen_del_mm <- __mmput <- mmput <- jas_ioctl [junction_as]  (into a freed sigqueue)
BUG: KFENCE: use-after-free write in lru_gen_del_mm+0x104/0x1a0
  lru_gen_del_mm <- __mmput <- mmput <- jas_ioctl [junction_as]  (into a freed mm_struct, allocated by dup_mm)
```

The third one is the tell: a `struct mm_struct` freed while a list still
pointed at it.

## The bug

`jas_adopt()` was modelled on `exec_mmap()`, which calls `lru_gen_add_mm()`
for the fresh mm it installs. An adopted mm is not fresh: it came from
`clone()` without `CLONE_VM`, and `kernel_clone()` has already added it to the
multi-gen LRU's per-memcg `mm_list`. The second `list_add_tail()` links the
same node twice. `lru_gen_del_mm()` at teardown removes one link; the other
keeps pointing at the freed mm, and every later `lru_gen_add_mm()` (any
`fork()` on the host) and `lru_gen_del_mm()` writes through it into whatever
the slab has since reused. Silent without `CONFIG_DEBUG_LIST`/`DEBUG_VM`.

## The fix

Delete the call. Nothing else: the mm is on the list from birth to
`__mmput()`, exactly like any other forked mm.

## Two real bugs found on the way (also fixed)

- `jas_switch` looked a handle up and took its `mm_users` reference afterwards,
  so a concurrent `jas_release` could drop the last reference in between; the
  switcher then revived and later double-tore-down the mm. Fixed with
  `mmget_not_zero()` under RCU and an RCU-deferred record.
- The first version of that fix dropped the last `mm_count` from the RCU
  callback (softirq). `__mmdrop() -> pgd_free()` takes `pgd_lock`, the same
  spinlock `pgd_alloc()` holds with interrupts on during every `fork()`; a
  softirq landing on a CPU inside `pgd_alloc()` deadlocked it against itself
  and every forking CPU behind it -- the two soft lockups. Fixed by dropping
  from a workqueue (the kernel's own `mmdrop_async()` pattern).

## Lessons

- A kernel that dies in five different subsystems has one corruptor, not five
  bugs. Bisecting filesystems was the wrong instinct; a memory-safety tool was
  the right one. Turn KFENCE on first (`scripts/junction-host-bringup.sh` now
  does).
- Crash capture (`softlockup_panic`, `hung_task_panic`, `panic_on_oops`,
  all-CPU backtraces, ERST pstore) turned "the machine hung" into a stack
  trace every time. Also in the bring-up script.
- Everything `exec_mmap()` does is not everything an mm swap needs; the
  correct question for each line is "does this mm already have it?".
