// SPDX-License-Identifier: GPL-2.0
/*
 * junction_as.c - multiple address spaces for the Junction LibOS
 *
 * Junction runs many guest processes inside a single Linux process so that one
 * userspace scheduler can multiplex them all. That design has one hard limit:
 * every guest shares one address space, so no guest can be given the same
 * virtual addresses as another, and fork() -- which by definition hands the
 * child the parent's addresses -- cannot be implemented.
 *
 * Dune solved the equivalent problem by letting the library OS own its page
 * tables outright (VT-x, ring 0, its own CR3). We take the same idea but keep
 * Linux's mm as the page-table implementation, which means copy-on-write,
 * reverse mapping, reclaim, and accounting all keep working exactly as they do
 * for a normal process. All this module adds is the one operation Linux does
 * not expose to userspace:
 *
 *     let a thread bind itself to a different mm_struct.
 *
 * Junction creates a new address space by cloning the host process (so Linux
 * itself performs the copy-on-write duplication), adopting the clone's mm
 * through this module, and then discarding the clone. Guest threads are then
 * run by switching the scheduling kthread onto the adopted mm.
 *
 * The LibOS itself must be mapped identically in every address space. Junction
 * arranges that by keeping its own writable memory in MAP_SHARED mappings,
 * which a clone inherits by reference rather than by copy. This module offers
 * an ioctl to audit that invariant.
 *
 * ioctls (see junction_as.h):
 *   ADOPT   - take a reference to another task's mm, return a handle
 *   SWITCH  - bind the calling thread to a handle (0 = the original mm)
 *   CURRENT - which handle the calling thread is bound to
 *   RELEASE - drop a handle
 *   SHARE_RANGE - share this range's page tables with every later address space
 */

#define pr_fmt(fmt) "junction_as: " fmt

#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmu_context.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "junction_as.h"

/* ---------------------------------------------------------------------- */
/* Kernel symbols that are not exported to modules                        */
/* ---------------------------------------------------------------------- */

static void (*p_switch_mm_irqs_off)(struct mm_struct *prev,
				    struct mm_struct *next,
				    struct task_struct *tsk);
static struct mm_struct *(*p_mm_access)(struct task_struct *task,
					unsigned int mode);
static void (*p_membarrier_update_current_mm)(struct mm_struct *next_mm);
static void (*p_sched_mm_cid_before_execve)(struct task_struct *t);
static void (*p_sched_mm_cid_after_execve)(struct task_struct *t);
static struct task_struct *(*p_find_get_task_by_vpid)(pid_t nr);
static int (*p___pud_alloc)(struct mm_struct *mm, p4d_t *p4d,
			    unsigned long address);

/*
 * kallsyms_lookup_name() stopped being exported in 5.7. The kprobe trick is
 * the standard way back in: register a probe on the symbol by name, read the
 * resolved address out of the kprobe, and unregister.
 */
static unsigned long jas_lookup_name(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

#define RESOLVE(ptr, name, required)					\
	do {								\
		*(unsigned long *)&(ptr) = jas_lookup_name(name);	\
		if (!(ptr) && (required)) {				\
			pr_err("cannot resolve %s\n", name);		\
			return -ENOENT;					\
		}							\
	} while (0)

static int jas_resolve_symbols(void)
{
	RESOLVE(p_switch_mm_irqs_off, "switch_mm_irqs_off", true);
	RESOLVE(p_mm_access, "mm_access", true);
	RESOLVE(p_find_get_task_by_vpid, "find_get_task_by_vpid", true);
	RESOLVE(p___pud_alloc, "__pud_alloc", true);
	/* The rest keep bookkeeping tidy but are not load-bearing. */
	RESOLVE(p_membarrier_update_current_mm, "membarrier_update_current_mm",
		false);
	RESOLVE(p_sched_mm_cid_before_execve, "sched_mm_cid_before_execve",
		false);
	RESOLVE(p_sched_mm_cid_after_execve, "sched_mm_cid_after_execve",
		false);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Per-open state                                                         */
/* ---------------------------------------------------------------------- */

/*
 * One context per open of /dev/junction_as. Junction opens it once; because
 * Junction's threads share a file descriptor table, every thread addresses the
 * same handle space.
 */
/*
 * One adopted address space.
 *
 * Handles are never reused. Junction caches "which address space is this core
 * bound to" per core and skips the switch when the handle already matches, so a
 * recycled handle would let a core keep running on a freed address space. The
 * handle therefore carries a generation counter in its upper bits, and only its
 * lower bits index the IDR.
 */
struct jas_space {
	struct mm_struct *mm;
	u64 handle;
	struct rcu_head rcu;		/* freed a grace period after release ... */
	struct work_struct work;	/* ... and then in process context */
};

static struct workqueue_struct *jas_wq;

#define JAS_HANDLE_ID(h)	((int)((h) & 0xffffffffULL))
#define JAS_MAKE_HANDLE(gen, id) ((((u64)(gen)) << 32) | (u32)(id))

/*
 * Slots of the address space whose page tables every adopted address space
 * shares with the original. One entry per paging slot (P4D_SIZE, 512 GB on
 * x86-64 with 4-level paging), recorded by its base address.
 */
#define JAS_MAX_SHARED_SLOTS 64

struct jas_ctx {
	struct mm_struct *root_mm;	/* the mm at open() time; handle 0 */
	struct idr as_idr;		/* id -> struct jas_space * */
	u32 next_gen;			/* never repeats a handle */
	unsigned long shared[JAS_MAX_SHARED_SLOTS];
	unsigned int nr_shared;
	struct mutex lock;		/* protects as_idr, next_gen, shared */
};

static struct jas_ctx *jas_ctx_of(struct file *f)
{
	return f->private_data;
}

/* ---------------------------------------------------------------------- */
/* Sharing the caller's page tables for a range                           */
/* ---------------------------------------------------------------------- */

/*
 * Junction's library OS has to be visible from every guest address space, and
 * copying it into each one does not work: memory the LibOS maps after an
 * address space has been created would never appear there. So the page tables
 * themselves are shared. Every adopted address space is given the *same*
 * next-level table for the LibOS's slots, which means a mapping established
 * through any of them is established for all of them.
 *
 * This is the same arrangement Linux uses for kernel memory, where every mm
 * carries identical top-level entries.
 *
 * Correctness rests on one invariant, which Junction is responsible for and
 * which is documented at length in docs/shared-page-tables.md:
 *
 *     nothing in a shared range is ever unmapped, moved, write-protected or
 *     reclaimed while it is shared.
 *
 * The reason is TLB invalidation. Reverse mapping only knows about the
 * original address space, so a shootdown targets mm_cpumask() of that mm and
 * is then discarded by flush_tlb_func() on any core running a different mm.
 * A core running a guest would keep a stale entry for a page that had been
 * freed. Establishing a *new* mapping needs no invalidation at all, which is
 * why sharing is safe for a range that is only ever added to.
 */

/* Makes the shared next-level table exist, so later clones copy a live entry. */
static int jas_prepare_slot(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	int ret = 0;

	if (mmap_write_lock_killable(mm))
		return -EINTR;
	pgd = pgd_offset(mm, addr);
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) && p___pud_alloc(mm, p4d, addr))
		ret = -ENOMEM;
	(void)pgd;
	mmap_write_unlock(mm);
	return ret;
}

/*
 * Points @mm at the original address space's tables for every shared slot.
 *
 * Junction marks the shared ranges MADV_DONTFORK, so a freshly cloned address
 * space has no tables of its own for these slots and nothing is leaked by
 * installing the shared ones. Refuse rather than leak if that is not true.
 */
/*
 * Always operate at the p4d level, never the pgd level.
 *
 * On x86-64 built for 5-level paging but running on a CPU without la57, the
 * p4d level is folded at *runtime*, and the pgd accessors are not symmetric
 * under that folding:
 *
 *     #define pgd_clear(pgd) (pgtable_l5_enabled() ? native_pgd_clear(pgd) : 0)
 *
 * set_pgd() writes unconditionally but pgd_clear() is a no-op, so installing an
 * entry with one and removing it with the other installs it permanently. The
 * address space is then torn down still pointing at the shared tables, and
 * exit_mmap() frees the LibOS's page tables while the LibOS is running on them.
 * That is not a theoretical failure; it took a machine down during development.
 *
 * native_p4d_clear() goes through native_set_p4d(), which writes unconditionally
 * and also performs the PTI mirroring that folding requires. p4d_offset()
 * resolves to the pgd entry when folded and to the 512 GB-stride entry when not,
 * so p4d-level code is correct on both.
 */
static p4d_t *jas_slot_entry(struct mm_struct *mm, unsigned long addr)
{
	return p4d_offset(pgd_offset(mm, addr), addr);
}

static int jas_share_into(struct jas_ctx *ctx, struct mm_struct *mm)
{
	unsigned int i, done = 0;
	int ret = 0;

	if (!ctx->nr_shared)
		return 0;

	spin_lock(&mm->page_table_lock);
	for (i = 0; i < ctx->nr_shared; i++) {
		unsigned long addr = ctx->shared[i];
		p4d_t *src = jas_slot_entry(ctx->root_mm, addr);
		p4d_t *dst = jas_slot_entry(mm, addr);

		/*
		 * The destination must be empty: Junction marks the shared
		 * ranges MADV_DONTFORK so a clone brings no tables of its own
		 * for them. Overwriting a populated entry would leak that
		 * subtree and its page references.
		 */
		if (WARN_ON_ONCE(p4d_none(*src)) ||
		    WARN_ON_ONCE(!p4d_none(*dst))) {
			ret = -EINVAL;
			break;
		}
		set_p4d(dst, *src);
		done++;
	}

	/* A partly shared address space is as dangerous as a fully shared one
	 * that is never detached, so undo the installs on failure. */
	if (ret)
		for (i = 0; i < done; i++)
			p4d_clear(jas_slot_entry(mm, ctx->shared[i]));

	spin_unlock(&mm->page_table_lock);
	return ret;
}

/*
 * Detaches @mm from the shared tables.
 *
 * Must happen before the address space is torn down: exit_mmap() walks the
 * tables and would otherwise free the LibOS's. It must also happen only when
 * no core is bound to @mm any more, or that core would lose the LibOS out from
 * under itself.
 */
static void jas_unshare_from(struct jas_ctx *ctx, struct mm_struct *mm)
{
	unsigned int i;

	if (!ctx->nr_shared)
		return;

	spin_lock(&mm->page_table_lock);
	for (i = 0; i < ctx->nr_shared; i++)
		p4d_clear(jas_slot_entry(mm, ctx->shared[i]));
	spin_unlock(&mm->page_table_lock);
}

/*
 * Page table sharing is off by default. It works (see scripts/vm_test.sh), but
 * it rests on an invariant Junction must uphold and nothing enforces -- that
 * nothing in a shared range is ever unmapped, moved or write-protected -- and
 * the memfd-arena design in docs/libos-memory-plan.md removes the need for it.
 * Kept behind a parameter so it can be evaluated, not so it can be stumbled on.
 */
static bool enable_pgtable_sharing;
module_param(enable_pgtable_sharing, bool, 0444);
MODULE_PARM_DESC(enable_pgtable_sharing,
		 "share LibOS page tables across address spaces (experimental)");

static long jas_share_range(struct jas_ctx *ctx, void __user *uarg)
{
	struct junction_as_share arg;
	unsigned long addr, end;
	long ret = 0;

	if (!enable_pgtable_sharing)
		return -EOPNOTSUPP;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || !arg.len)
		return -EINVAL;

	/*
	 * 5-level paging would mean sharing p4d entries out of a pgd page that
	 * a cloned address space allocates for itself. That is a small
	 * extension, but it is not exercised on 4-level hardware, so refuse it
	 * rather than ship it untested.
	 */
	if (pgtable_l5_enabled()) {
		pr_warn_once("5-level paging: page table sharing unsupported\n");
		return -EOPNOTSUPP;
	}

	addr = arg.start & P4D_MASK;
	end = ALIGN(arg.start + arg.len, P4D_SIZE);

	mutex_lock(&ctx->lock);
	for (; addr < end && !ret; addr += P4D_SIZE) {
		unsigned int i;
		bool dup = false;

		for (i = 0; i < ctx->nr_shared; i++)
			if (ctx->shared[i] == addr)
				dup = true;
		if (dup)
			continue;
		if (ctx->nr_shared >= JAS_MAX_SHARED_SLOTS) {
			ret = -ENOSPC;
			break;
		}
		ret = jas_prepare_slot(ctx->root_mm, addr);
		if (!ret)
			ctx->shared[ctx->nr_shared++] = addr;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* The address-space switch                                               */
/* ---------------------------------------------------------------------- */

/*
 * jas_bind_mm - point the calling thread at @new_mm.
 *
 * This mirrors exec_mmap(), which is the kernel's own "replace the current
 * task's address space" path. The differences are deliberate:
 *
 *   - We do not call exec_mm_release(): we are not exec'ing, so the thread's
 *     robust futex list, vfork_done, and clear_child_tid all still refer to
 *     state that remains valid.
 *   - We take an mm_users reference for the thread and drop the old one, so
 *     the thread always owns exactly one reference. exec_mmap() gets that
 *     reference handed to it by the caller instead.
 *   - active_mm always equals mm here, because the caller is always a user
 *     thread. None of the lazy-TLB refcounting in exec_mmap() applies.
 */
/* Consumes the caller's user reference on @new_mm, whatever it returns. */
static int jas_bind_mm(struct mm_struct *new_mm)
{
	struct task_struct *tsk = current;
	struct mm_struct *old_mm, *active_mm;

	if (WARN_ON_ONCE(!new_mm))
		return -EINVAL;

	old_mm = tsk->mm;
	if (old_mm == new_mm) {
		mmput(new_mm);	/* already ours; the thread has its own */
		return 0;
	}
	if (WARN_ON_ONCE(!old_mm)) {
		mmput(new_mm);
		return -EINVAL;	/* kernel threads are not our business */
	}

	/*
	 * The caller's reference becomes the thread's own reference on the mm
	 * it is about to run on.
	 */

	if (p_sched_mm_cid_before_execve)
		p_sched_mm_cid_before_execve(tsk);

	task_lock(tsk);
	if (p_membarrier_update_current_mm)
		p_membarrier_update_current_mm(new_mm);
	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = new_mm;
	tsk->mm = new_mm;
	p_switch_mm_irqs_off(active_mm, new_mm, tsk);
	local_irq_enable();
	task_unlock(tsk);

	if (p_sched_mm_cid_after_execve)
		p_sched_mm_cid_after_execve(tsk);

	mmput(old_mm);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* ioctl handlers                                                         */
/* ---------------------------------------------------------------------- */

static void jas_space_free_rcu(struct rcu_head *head);

static long jas_adopt(struct jas_ctx *ctx, void __user *uarg)
{
	struct junction_as_adopt arg;
	struct task_struct *task;
	struct mm_struct *mm;
	struct jas_space *sp;
	int id;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	task = p_find_get_task_by_vpid(arg.pid);
	if (!task)
		return -ESRCH;

	/*
	 * Only adopt an address space we would be allowed to ptrace. In
	 * Junction's own use the target is a clone of the caller, but the
	 * device may be world-accessible, so check properly.
	 */
	mm = p_mm_access(task, PTRACE_MODE_ATTACH_REALCREDS);
	put_task_struct(task);
	if (IS_ERR_OR_NULL(mm))
		return mm ? PTR_ERR(mm) : -EINVAL;

	if (mm == ctx->root_mm) {
		/* Adopting our own mm would alias handle 0; refuse. */
		mmput(mm);
		return -EINVAL;
	}

	sp = kzalloc(sizeof(*sp), GFP_KERNEL);
	if (!sp) {
		mmput(mm);
		return -ENOMEM;
	}
	sp->mm = mm;
	mmgrab(mm);	/* the record's hold on the structure; see jas_space_free_rcu */

	/* Give it the LibOS's page tables before anything can run on it. */
	id = jas_share_into(ctx, mm);
	if (id) {
		mmdrop(mm);
		kfree(sp);
		mmput(mm);
		return id;
	}

	mutex_lock(&ctx->lock);
	id = idr_alloc(&ctx->as_idr, sp, 1, 0, GFP_KERNEL);
	if (id >= 0)
		sp->handle = JAS_MAKE_HANDLE(++ctx->next_gen, id);
	mutex_unlock(&ctx->lock);
	if (id < 0) {
		mmdrop(mm);
		kfree(sp);
		mmput(mm);
		return id;
	}

	/*
	 * Do NOT lru_gen_add_mm(mm) here, as exec_mmap() does for the mm it
	 * creates. This mm came from clone() without CLONE_VM, and kernel_clone()
	 * already put it on the multi-gen LRU's mm_list. Adding it a second
	 * time links the same list node twice; when the mm is later freed one
	 * link is removed and the other keeps pointing at freed memory, and
	 * every later add/del writes through it -- into whatever the slab has
	 * since handed out (KFENCE: use-after-free writes in lru_gen_add_mm from
	 * fork() and lru_gen_del_mm from mmput(), into names_cache, sigqueue and
	 * mm_struct objects). That was the source of a whole family of host
	 * crashes under many concurrent sandboxes: an i_readcount underflow in
	 * __fput, a vfs_link that never returned, __destroy_inode on a tmpfs
	 * umount, a NULL page in ext4_finish_bio.
	 */

	arg.handle = sp->handle;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		mutex_lock(&ctx->lock);
		idr_remove(&ctx->as_idr, id);
		mutex_unlock(&ctx->lock);
		mmput(mm);
		call_rcu(&sp->rcu, jas_space_free_rcu);	/* it was findable */
		return -EFAULT;
	}
	return 0;
}

/*
 * A handle's record holds two things on its address space: a user reference
 * (mm_users, mmget/mmput), which keeps the address space alive, and a
 * structure reference (mm_count, mmgrab/mmdrop), which only keeps struct
 * mm_struct's memory valid. Release drops the first at once and the second,
 * with the record itself, a grace period later -- so that a SWITCH racing
 * with the RELEASE of the same handle, which Junction does routinely (its
 * arena collector and its memory-map destructor both "try" address spaces that
 * another thread may be releasing), finds either a live address space or
 * nothing, and never one that is being torn down.
 *
 * It used to find the third. SWITCH looked the mm up and took its reference
 * afterwards, with nothing held in between; RELEASE meanwhile dropped the last
 * reference, exit_mmap() closed every mapped file, and SWITCH then revived the
 * corpse with mmget() and ran on it. Its own mmput() tore the address space
 * down a second time: files released twice, "kernel BUG at fs.h:2906"
 * (i_readcount underflow in __fput), panic. It took 128 sandboxes with four
 * kthreads each, three hours in, to lose that race.
 */
/*
 * The drop itself runs in process context, never in the RCU callback: that
 * is softirq context, and when this is the last structure reference,
 * __mmdrop() -> pgd_free() takes pgd_lock -- the spinlock pgd_alloc() holds
 * with interrupts enabled on every fork(). A softirq landing on a CPU inside
 * pgd_alloc() then deadlocks that CPU against itself and every forking CPU
 * behind it ("softlockup: hung tasks", 128 sandboxes booting at once). The
 * kernel's own mmdrop_async() exists for this reason and is not exported, so
 * this is the same thing by hand.
 */
static void jas_space_free_work(struct work_struct *work)
{
	struct jas_space *sp = container_of(work, struct jas_space, work);

	mmdrop(sp->mm);
	kfree(sp);
}

static void jas_space_free_rcu(struct rcu_head *head)
{
	struct jas_space *sp = container_of(head, struct jas_space, rcu);

	INIT_WORK(&sp->work, jas_space_free_work);
	queue_work(jas_wq, &sp->work);
}

/*
 * Looks up an address space by handle, rejecting a stale generation, and
 * returns it with a user reference held -- or NULL if it is gone or going.
 */
static struct mm_struct *jas_lookup(struct jas_ctx *ctx, unsigned long handle)
{
	struct jas_space *sp;

	struct mm_struct *mm = NULL;

	rcu_read_lock();
	sp = idr_find(&ctx->as_idr, JAS_HANDLE_ID(handle));
	if (sp && sp->handle == (u64)handle && mmget_not_zero(sp->mm))
		mm = sp->mm;
	rcu_read_unlock();
	return mm;
}

static long jas_switch(struct jas_ctx *ctx, unsigned long handle)
{
	struct mm_struct *mm;

	if (handle == 0) {
		mm = ctx->root_mm;	/* held by the context for its lifetime */
		mmget(mm);
	} else {
		mm = jas_lookup(ctx, handle);
		if (!mm)
			return -ENOENT;
	}

	return jas_bind_mm(mm);
}

static long jas_current(struct jas_ctx *ctx, void __user *uarg)
{
	struct mm_struct *mm = current->mm;
	unsigned long handle = 0;
	struct jas_space *sp;
	int id;

	if (mm != ctx->root_mm) {
		handle = (unsigned long)-1;
		mutex_lock(&ctx->lock);
		idr_for_each_entry(&ctx->as_idr, sp, id) {
			if (sp->mm == mm) {
				handle = (unsigned long)sp->handle;
				break;
			}
		}
		mutex_unlock(&ctx->lock);
		if (handle == (unsigned long)-1)
			return -ENOENT;
	}

	if (copy_to_user(uarg, &handle, sizeof(handle)))
		return -EFAULT;
	return 0;
}

static long jas_release(struct jas_ctx *ctx, unsigned long handle)
{
	struct jas_space *sp;
	struct mm_struct *mm;

	if (handle == 0)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	sp = idr_find(&ctx->as_idr, JAS_HANDLE_ID(handle));
	if (sp && sp->handle != (u64)handle)
		sp = NULL;
	if (sp)
		idr_remove(&ctx->as_idr, JAS_HANDLE_ID(handle));
	mutex_unlock(&ctx->lock);
	if (!sp)
		return -ENOENT;
	mm = sp->mm;

	/*
	 * Detach from the shared tables before this address space can be torn
	 * down, or exit_mmap() would walk into them and free the LibOS's page
	 * tables. Junction releases a handle only once the guest is gone and no
	 * core is bound to it.
	 */
	jas_unshare_from(ctx, mm);

	/*
	 * Threads still bound to this mm hold their own references, so the
	 * address space survives until the last of them switches away or
	 * exits -- in practice an idle runtime kthread, which keeps the last
	 * address space it ran until it schedules a thread of another; that is
	 * routine, and harmless without page-table sharing.
	 */
	mmput(mm);
	call_rcu(&sp->rcu, jas_space_free_rcu);
	return 0;
}

static long jas_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct jas_ctx *ctx = jas_ctx_of(f);

	switch (cmd) {
	case JUNCTION_AS_ADOPT:
		return jas_adopt(ctx, (void __user *)arg);
	case JUNCTION_AS_SWITCH:
		return jas_switch(ctx, arg);
	case JUNCTION_AS_CURRENT:
		return jas_current(ctx, (void __user *)arg);
	case JUNCTION_AS_RELEASE:
		return jas_release(ctx, arg);
	case JUNCTION_AS_SHARE_RANGE:
		return jas_share_range(ctx, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

/* ---------------------------------------------------------------------- */
/* Device lifecycle                                                       */
/* ---------------------------------------------------------------------- */

static int jas_open(struct inode *ino, struct file *f)
{
	struct jas_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->root_mm = get_task_mm(current);
	if (!ctx->root_mm) {
		kfree(ctx);
		return -EINVAL;
	}

	idr_init(&ctx->as_idr);
	mutex_init(&ctx->lock);
	f->private_data = ctx;
	return 0;
}

static int jas_release_file(struct inode *ino, struct file *f)
{
	struct jas_ctx *ctx = jas_ctx_of(f);
	struct jas_space *sp;
	int id;

	/*
	 * Drop the context's reference on every adopted address space. Any
	 * thread still running on one keeps it alive through its own
	 * reference, and releases it when it switches away or exits.
	 */
	idr_for_each_entry(&ctx->as_idr, sp, id) {
		jas_unshare_from(ctx, sp->mm);
		mmput(sp->mm);
		call_rcu(&sp->rcu, jas_space_free_rcu);
	}
	idr_destroy(&ctx->as_idr);

	mmput(ctx->root_mm);
	kfree(ctx);
	return 0;
}

static const struct file_operations jas_fops = {
	.owner		= THIS_MODULE,
	.open		= jas_open,
	.release	= jas_release_file,
	.unlocked_ioctl	= jas_ioctl,
	.compat_ioctl	= jas_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice jas_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "junction_as",
	.fops	= &jas_fops,
	.mode	= 0666,
};

static int __init jas_init(void)
{
	int ret;

	ret = jas_resolve_symbols();
	if (ret)
		return ret;

	jas_wq = alloc_workqueue("junction_as", 0, 0);
	if (!jas_wq)
		return -ENOMEM;
	ret = misc_register(&jas_dev);
	if (ret) {
		pr_err("failed to register /dev/junction_as: %d\n", ret);
		destroy_workqueue(jas_wq);
		return ret;
	}

	pr_info("loaded (/dev/junction_as)\n");
	return 0;
}

static void __exit jas_exit(void)
{
	misc_deregister(&jas_dev);
	rcu_barrier();	/* jas_space_free_rcu() lives in this module ... */
	destroy_workqueue(jas_wq);	/* ... and flushes into this workqueue */
	pr_info("unloaded\n");
}

module_init(jas_init);
module_exit(jas_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multiple address spaces for the Junction LibOS");
