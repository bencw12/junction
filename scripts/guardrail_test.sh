#!/bin/bash
#
# guardrail_test.sh - Phase 0 of docs/fork-implementation-plan.md.
#
# Two checks were added to make address-space divergence loud:
#
#   CheckFrozenViolation()      (0b) a structural change -- munmap, mprotect,
#                               mremap, madvise(DONTNEED) -- applied to LibOS
#                               memory that other address spaces have already
#                               inherited. Always on.
#
#   AuditAddressSpaceCoherence() (0c) a diff of every live address space's
#                               LibOS mappings against the caller's. Behind
#                               --debug_as_audit; also runs at the fatal-fault
#                               path, where it names the missing mapping.
#
# A guardrail is only worth having if it fires on the real bug and stays quiet
# otherwise, so this tests both directions.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_stacktest.config
fail=0

[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"
cc -O1 -w -o "${BUILD}/memfs_extent_teardown" \
    "${ROOT}/docs/traces/memfs_extent_teardown.c" || exit 2
cc -O1 -w -o "${BUILD}/memfs_race" "${ROOT}/docs/traces/memfs_race.c" || exit 2

run() { (cd "${BUILD}" && timeout 120 "$@" 2>&1); }

want() { # name, pattern, command...
    local name=$1 pat=$2; shift 2
    if run "$@" | grep -q -- "${pat}"; then
        printf "  %-52s PASS\n" "${name}"
    else
        printf "  %-52s FAIL (expected %s)\n" "${name}" "${pat}"
        fail=1
    fi
}

reject() { # name, pattern, command...
    local name=$1 pat=$2; shift 2
    local out
    out=$(run "$@")
    if echo "${out}" | grep -q -- "${pat}"; then
        printf "  %-52s FAIL (false positive)\n" "${name}"
        echo "${out}" | grep -- "${pat}" | head -2 | sed 's/^/        /'
        fail=1
    else
        printf "  %-52s PASS\n" "${name}"
    fi
}

echo "------------------------------------------------------------"
echo "0b: the frozen-after-init check fires on a real violation"
echo "------------------------------------------------------------"
# --debug_frozen_probe mprotects a page of the LibOS's own .bss after the first
# fork, to the permissions it already has: a structural change in one address
# space and nothing else. Every violation that occurred naturally has since
# been fixed (memfs was the last), so a deliberate one is what keeps the check
# itself under test.
want "deliberate mprotect of LibOS memory after a fork" "FROZEN VIOLATION: mprotect" \
     ./junction_run "${CFG}" --debug_frozen_probe -- \
     /bin/sh -c 'sleep 0.2 & wait; echo ok'

echo
echo "------------------------------------------------------------"
echo "0b: and stays quiet on workloads that do nothing wrong"
echo "------------------------------------------------------------"
# Guest memory changes in one address space by design; exec unmaps an entire
# image; both must be silent. These are the shapes that produced false
# positives while the check was being written.
reject "fork + exec, parent concurrent" "FROZEN VIOLATION" \
       ./junction_run "${CFG}" -- /bin/sh -c '/bin/true; /bin/true; echo ok'
reject "sh: background job + wait" "FROZEN VIOLATION" \
       ./junction_run "${CFG}" -- /bin/sh -c 'sleep 0.2 & wait; echo ok'
reject "sh: pipeline" "FROZEN VIOLATION" \
       ./junction_run "${CFG}" -- /bin/sh -c 'echo hello | grep hello'

echo
echo "------------------------------------------------------------"
echo "0b: deleting a memfs file is no longer a violation (Phase 1)"
echo "------------------------------------------------------------"
# This is what the check was written against: deleting a memfs file unmapped
# its 256 MB extent in one address space and left every other one holding a
# live mapping of freed memory. Phase 1 fixed it by unmapping everywhere, so
# the same workload must now be silent.
reject "memfs extent teardown with a live child" "FROZEN VIOLATION" \
       ./junction_run "${CFG}" -- ./memfs_extent_teardown

echo
echo "------------------------------------------------------------"
echo "0c: the coherence audit sees a real divergence"
echo "------------------------------------------------------------"
# The probe changes protections in one address space only, which is exactly
# what the audit compares. It must notice, and must say which address space.
want "audit notices the probe's divergence" "with different protections" \
     ./junction_run "${CFG}" --debug_as_audit --debug_frozen_probe -- \
     /bin/sh -c 'sleep 0.2 & wait; echo ok'

echo
echo "------------------------------------------------------------"
echo "0c: and reports nothing when the address spaces do agree"
echo "------------------------------------------------------------"
reject "sh: background job + wait, audited" "diverges from" \
       ./junction_run "${CFG}" --debug_as_audit -- \
       /bin/sh -c 'sleep 0.2 & wait; echo ok'
reject "fork + exec, audited" "diverges from" \
       ./junction_run "${CFG}" --debug_as_audit -- \
       /bin/sh -c '/bin/true; /bin/true; echo ok'

echo
if [ "${fail}" -eq 0 ]; then echo "PASS"; exit 0; fi
echo "FAIL"; exit 1
