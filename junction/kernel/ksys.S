/*
 * ksys.S - support for Linux system calls
 */

#include <asm/unistd_64.h>

.section        .note.GNU-stack,"",@progbits
.text

.macro SYSCALL_123 name number
.align 16
.globl ksys_\name
.type ksys_\name, @function
ksys_\name:
	movl	$\number, %eax
	syscall
	ret
.endm

.macro SYSCALL_456 name number
.align 16
.globl ksys_\name
.type ksys_\name, @function
ksys_\name:
	movl	$\number, %eax
	movq	%rcx, %r10
	syscall
	ret
.endm

.globl ksys_start
ksys_start:

SYSCALL_456 mmap __NR_mmap
SYSCALL_123 munmap __NR_munmap
SYSCALL_123 mprotect __NR_mprotect
SYSCALL_123 madvise __NR_madvise
SYSCALL_456 mremap __NR_mremap

SYSCALL_456 openat __NR_openat
SYSCALL_123 close __NR_close
// SYSCALL_123 readv __NR_readv
SYSCALL_456 pread __NR_pread64

SYSCALL_456 newfstatat __NR_newfstatat
SYSCALL_123 getdents64 __NR_getdents64
SYSCALL_456 readlinkat __NR_readlinkat

SYSCALL_123 tgkill __NR_tgkill
SYSCALL_123 exit __NR_exit_group

.align 16
.globl ksys_default
.type ksys_default, @function
ksys_default:
	movq	8(%rsp), %rax
	movq	%rcx, %r10
	syscall
	ret

/*
 * long junction_as_clone_parked(unsigned long flags, void *child_stack)
 *
 * Clones the host process to materialize a new address space. Linux does the
 * copy-on-write duplication; the clone exists only so that something owns the
 * resulting mm until Junction can take a reference to it.
 *
 * The parent gets the clone's pid. The clone parks forever and never returns.
 *
 * It must not touch a single byte of memory Junction still owns. Junction's
 * stacks are MAP_SHARED so that they are the same memory in every address
 * space, which means a clone left running on the caller's stack would overwrite
 * the caller's live frames. It therefore starts on the caller-supplied private
 * stack and executes nothing but register-only instructions and system calls.
 * That is also why this is assembly: a compiler is free to spill.
 *
 * It lives here, between ksys_start and ksys_end, because the seccomp filter
 * only permits Junction's own system calls from this address range.
 */
.align 16
.globl junction_as_clone_parked
.type junction_as_clone_parked, @function
junction_as_clone_parked:
	/* clone(flags, child_stack, parent_tid, child_tid, tls) */
	xorl	%edx, %edx		/* parent_tid = NULL */
	xorl	%r10d, %r10d		/* child_tid  = NULL */
	xorl	%r8d, %r8d		/* tls        = 0 */
	movl	$__NR_clone, %eax
	syscall

	testq	%rax, %rax
	jz	1f
	ret				/* parent: the pid, or -errno */

1:	movl	$__NR_pause, %eax	/* clone: stay alive, off the CPU */
	syscall
	jmp	1b

.globl ksys_end
ksys_end:
