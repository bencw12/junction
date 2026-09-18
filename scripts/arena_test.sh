#!/bin/bash
#
# arena_test.sh - Phase 2 of docs/fork-implementation-plan.md: the LibOS arena.
#
# The arena is where LibOS memory goes so that an address space which never
# received a mapping can repair it on first touch, from the memfd behind it,
# by looking the range up in a shadow map. --debug_arena_probe exercises that
# on demand: after the first fork it maps four ranges of different shapes here,
# changes one's protection, frees one (which must be quarantined, not reused),
# and reads all of them back from every other address space -- each read
# faults, and each fault has to be repaired through the preemption-disabled
# path, the one the LibOS's own allocators take under their spinlocks.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_stacktest.config
fail=0

[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"

run() { (cd "${BUILD}" && timeout 120 "$@" 2>&1); }

want() { # name, pattern, command...
    local name=$1 pat=$2; shift 2
    local out
    out=$(run "$@")
    if echo "${out}" | grep -q -- "${pat}"; then
        printf "  %-56s PASS\n" "${name}"
    else
        printf "  %-56s FAIL (expected %s)\n" "${name}" "${pat}"
        echo "${out}" | grep -E "arena|fault|segfault|Panic|Abort|FAIL" |
            head -4 | sed 's/^/        /'
        fail=1
    fi
}

reject() { # name, pattern, command...
    local name=$1 pat=$2; shift 2
    local out
    out=$(run "$@")
    if echo "${out}" | grep -qE -- "${pat}"; then
        printf "  %-56s FAIL (false positive)\n" "${name}"
        echo "${out}" | grep -E -- "${pat}" | head -2 | sed 's/^/        /'
        fail=1
    else
        printf "  %-56s PASS\n" "${name}"
    fi
}

# A backgrounded job: dash fork()s for '&' but vfork()s for a plain command,
# and a vfork child shares its parent's address space until it execs -- so only
# a real fork creates the second address space the probe needs to visit. Same
# idiom as guardrail_test.sh.
ONE_FORK=(/bin/sh -c 'sleep 0.2 & wait; echo ok')

echo "------------------------------------------------------------"
echo "2a/2b: the arena exists and is registered as a managed slot"
echo "------------------------------------------------------------"
want "arena slot registered (shadow map, memfd-backed)" \
     "libos-arena manages 0x" \
     ./junction_run "${CFG}" -- "${ONE_FORK[@]}"

echo
echo "------------------------------------------------------------"
echo "2c: faults in the arena are repaired from the shadow map"
echo "------------------------------------------------------------"
want "probe: cross-address-space read-back repaired" \
     "arena probe: PASS" \
     ./junction_run "${CFG}" --debug_arena_probe -- "${ONE_FORK[@]}"
reject "probe: no fatal fault on the preempt-disabled path" \
       "fault with preemption disabled|unhandled segfault|segfault in syscall" \
       ./junction_run "${CFG}" --debug_arena_probe -- "${ONE_FORK[@]}"
want "probe: the repair went through the arena, not memfs" \
     "repaired a fault in the LibOS arena" \
     ./junction_run "${CFG}" --debug_arena_probe -- "${ONE_FORK[@]}"

echo
echo "------------------------------------------------------------"
echo "2d: a freed range is quarantined, and lazy absence is not divergence"
echo "------------------------------------------------------------"
# The probe fails outright if the freed range is handed out again.
reject "probe: quarantined range not reused" \
       "was reused|still reported live" \
       ./junction_run "${CFG}" --debug_arena_probe -- "${ONE_FORK[@]}"
# Arena ranges are absent from other address spaces until touched. That is
# the design, so the coherence audit must not count it.
reject "audit does not report arena absences as divergence" \
       "LibOS mapping\(s\) absent" \
       ./junction_run "${CFG}" --debug_as_audit --debug_arena_probe -- \
           "${ONE_FORK[@]}"

echo
echo "------------------------------------------------------------"
echo "nothing else regressed"
echo "------------------------------------------------------------"
want "shell pipeline across a fork still works" "hi" \
     ./junction_run "${CFG}" -- /bin/sh -c 'echo hi > /tmp/f; ( cat /tmp/f )'
want "guest runs to completion with the arena present" "^ok$" \
     ./junction_run "${CFG}" -- /bin/echo ok

echo
if [ "${fail}" -eq 0 ]; then echo "PASS"; else echo "FAIL"; fi
exit "${fail}"
