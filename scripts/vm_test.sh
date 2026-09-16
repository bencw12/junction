#!/bin/bash
#
# vm_test.sh - run the address-space module's tests inside a throwaway VM.
#
# Kernel work here has a failure mode userspace work does not: a bad page-table
# change corrupts memory, the machine hangs, and the evidence is destroyed by
# the same event that produced it -- there is no oops left on disk to read.
# This boots the host's own kernel image under QEMU with a minimal initramfs
# containing the module and as_test, so a bad change costs about a minute.
#
#     No build of junction_as.ko is loaded on the host until that exact build
#     has passed here.
#
# Usage:  scripts/vm_test.sh [-k]     (-k keeps the scratch directory)
#
# Exits 0 only if the module loaded and every check passed.

set -u

ROOT=$(dirname "$(readlink -f "$0")")/..
KERN=${ROOT}/kern
WORK=${TMPDIR:-/tmp}/junction-vmtest.$$
KVER=$(uname -r)
KEEP=0
[ "${1:-}" = "-k" ] && KEEP=1

cleanup() { [ "${KEEP}" = 1 ] || rm -rf "${WORK}"; }
trap cleanup EXIT

fail() { echo "vm_test: $*" >&2; exit 1; }

command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not installed"
command -v cpio >/dev/null || fail "cpio not installed"

mkdir -p "${WORK}/root"/{proc,sys,dev}

# ---- build the module and the test against the running kernel ------------

echo "vm_test: building module and test for ${KVER}"
make -s -C "${KERN}" junction_as.ko >/dev/null || fail "module build failed"
gcc -O2 -static -Wall -o "${WORK}/root/as_test" \
    -I"${KERN}" "${KERN}/as_test.c" || fail "as_test build failed"
cp "${KERN}/junction_as.ko" "${WORK}/root/" || fail "no junction_as.ko"

# ---- an init that loads the module, runs the test, and powers off --------

cat > "${WORK}/init.c" <<'EOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int status = 0, fd;
    pid_t p;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sys", "/sys", "sysfs", 0, NULL);
    mount("dev", "/dev", "devtmpfs", 0, NULL);

    fd = open("/junction_as.ko", O_RDONLY);
    if (fd < 0) { perror("open module"); goto out; }
    if (syscall(SYS_finit_module, fd, "", 0) != 0) {
        perror("finit_module");
        goto out;
    }
    close(fd);
    printf("VMTEST: module loaded\n");
    fflush(stdout);

    p = fork();
    if (p == 0) {
        execl("/as_test", "as_test", (char *)NULL);
        perror("exec as_test");
        _exit(127);
    }
    waitpid(p, &status, 0);
    printf("VMTEST: as_test exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

out:
    printf("VMTEST: done\n");
    fflush(stdout);
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
}
EOF
gcc -O2 -static -o "${WORK}/root/init" "${WORK}/init.c" || fail "init build failed"

# ---- pack the initramfs --------------------------------------------------

(cd "${WORK}/root" && find . | cpio -o -H newc 2>/dev/null | gzip -1 \
    > "${WORK}/initramfs.cpio.gz") || fail "initramfs build failed"

# The kernel image is usually root-only; take a readable copy.
if [ -r "/boot/vmlinuz-${KVER}" ]; then
    cp "/boot/vmlinuz-${KVER}" "${WORK}/vmlinuz"
else
    sudo cp "/boot/vmlinuz-${KVER}" "${WORK}/vmlinuz" || fail "cannot read kernel image"
    sudo chown "$(id -u)" "${WORK}/vmlinuz"
fi

# ---- boot ----------------------------------------------------------------

echo "vm_test: booting ${KVER} under qemu"
timeout 900 qemu-system-x86_64 \
    -enable-kvm -machine q35 -cpu Skylake-Server -smp 2 -m 2G \
    -nographic -no-reboot -device virtio-rng-pci \
    -kernel "${WORK}/vmlinuz" -initrd "${WORK}/initramfs.cpio.gz" \
    -append "console=ttyS0,115200 panic=3 nokaslr loglevel=4 \
             tsc=reliable no_timer_check" \
    > "${WORK}/vm.log" 2>&1

echo
sed -n '/VMTEST: module loaded/,$p' "${WORK}/vm.log" | grep -vE '^\[' | head -60

# ---- verdict -------------------------------------------------------------

echo
if grep -qE "BUG:|Oops|kernel panic|general protection" "${WORK}/vm.log"; then
    echo "vm_test: FAIL - the kernel complained:"
    grep -E "BUG:|Oops|kernel panic|general protection" "${WORK}/vm.log" | head -5
    exit 1
fi
if ! grep -q "VMTEST: module loaded" "${WORK}/vm.log"; then
    echo "vm_test: FAIL - module did not load (see ${WORK}/vm.log)"
    [ "${KEEP}" = 1 ] || echo "  re-run with -k to keep the log"
    exit 1
fi
if ! grep -q "VMTEST: as_test exit 0" "${WORK}/vm.log"; then
    echo "vm_test: FAIL - as_test did not pass"
    exit 1
fi

echo "vm_test: PASS - safe to load this build on the host"
