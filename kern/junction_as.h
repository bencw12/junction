/* SPDX-License-Identifier: GPL-2.0 */
/*
 * junction_as.h - userspace ABI for the Junction address-space device.
 *
 * Shared verbatim between the kernel module and Junction.
 */

#ifndef _JUNCTION_AS_H_
#define _JUNCTION_AS_H_

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define JUNCTION_AS_DEVICE "/dev/junction_as"

/* Handle 0 always refers to the address space the device was opened in. */
#define JUNCTION_AS_ROOT 0ULL

struct junction_as_adopt {
	__u32 pid;	/* in: thread ID whose mm should be adopted */
	__u32 flags;	/* in: must be zero */
	__u64 handle;	/* out: handle for the adopted address space */
};

/*
 * A range of the caller's address space whose page tables should be shared by
 * every address space adopted afterwards. Junction registers the LibOS's
 * memory here, so that memory it maps later is visible from every guest
 * address space without being copied into each one.
 *
 * The range is rounded out to whole paging slots (512 GB on x86-64), so it must
 * not share a slot with anything that should stay private.
 */
struct junction_as_share {
	__u64 start;
	__u64 len;
	__u64 flags;	/* must be zero */
};

#define JUNCTION_AS_MAGIC 'J'

/* Take a reference to another task's address space. */
#define JUNCTION_AS_ADOPT	_IOWR(JUNCTION_AS_MAGIC, 1, struct junction_as_adopt)
/* Bind the calling thread to an address space (arg is the handle). */
#define JUNCTION_AS_SWITCH	_IO(JUNCTION_AS_MAGIC, 2)
/* Report which handle the calling thread is bound to (arg is __u64 *). */
#define JUNCTION_AS_CURRENT	_IOR(JUNCTION_AS_MAGIC, 3, __u64)
/* Drop a handle (arg is the handle). */
#define JUNCTION_AS_RELEASE	_IO(JUNCTION_AS_MAGIC, 4)
/* Share this range's page tables with every address space adopted later. */
#define JUNCTION_AS_SHARE_RANGE	_IOW(JUNCTION_AS_MAGIC, 5, struct junction_as_share)

#endif /* _JUNCTION_AS_H_ */
