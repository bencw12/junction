#!/bin/bash
#
# heap_overflow_test.sh - Junction's own glibc heap across address spaces.
#
# Junction's glibc reaches the kernel with mmap (brk is forced to 0), and
# seccomp traps it. It used to be executed natively, into whichever address
# space the calling core was bound to; glibc's free lists are shared LibOS
# state, so that chunk could then be handed to LibOS code running under a
# different address space, which crashed on it. Junction papered over this by
# pre-growing its heap 64 MB at init and stopping glibc from mapping more --
# a pre-reserved pool with a cliff.
#
# Phase 3b of docs/fork-implementation-plan.md: the trapped mmap is served by
# the LibOS arena instead, and the pool is gone. An arena range is absent from
# an address space that never touched it and repaired on first touch, so the
# check is not "is it in the map" but "does a touch from another address space
# read back the bytes the first one wrote".
#
# Part 1 forces the allocation with --debug_libos_alloc under a forked guest's
# address space and touches it from another. Part 2 asks whether a *guest* can
# drive LibOS heap growth on its own, for the record.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_stacktest.config
MB=${ALLOC_MB:-128}
ATTEMPTS=${ATTEMPTS:-3}
SPLITS=${VMA_SPLITS:-400000}

[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"
cc -O1 -w -o "${BUILD}/heap_overflow_race" \
    "${ROOT}/docs/traces/heap_overflow_race.c" || exit 2
cc -O1 -w -o "${BUILD}/probe_guest" "${ROOT}/docs/traces/probe_guest.c" || exit 2

echo "------------------------------------------------------------"
echo "Junction glibc heap across address spaces"
echo "------------------------------------------------------------"
echo "[1] forced: LibOS allocates ${MB} MB under a forked guest's address"
echo "    space, then another address space dereferences it"
fail=0
for i in $(seq 1 "${ATTEMPTS}"); do
    out=$(cd "${BUILD}" && timeout 120 ./junction_run "${CFG}" \
          --debug_libos_alloc "${MB}" -- ./probe_guest 2>&1)
    if echo "${out}" | grep -q "Aborting on signal\|WRONG BYTES"; then
        why=$(echo "${out}" | grep -oE "at 0x[0-9a-f]+|WRONG BYTES" | head -1)
        echo "    attempt ${i}: CRASH/WRONG ${why}"
        fail=$((fail + 1))
    elif echo "${out}" | grep -q "REPAIRED ON TOUCH (read 0xab"; then
        echo "    attempt ${i}: ok - absent, repaired on touch, bytes match"
    elif echo "${out}" | grep -q "PRESENT"; then
        echo "    attempt ${i}: ok - already present in the other address space"
    else
        echo "    attempt ${i}: inconclusive"
        fail=$((fail + 1))
    fi
done

echo
echo "[2] guest-driven: can a guest grow the LibOS heap past the reserve?"
out=$(cd "${BUILD}" && timeout 240 \
      ./junction_run "${CFG}" --trace_libos_mem /tmp/heap_trace.txt -- \
      ./heap_overflow_race "${SPLITS}" $((SPLITS / 8)) 3 2>&1)
echo "${out}" | grep -E "parent:|child:" | sed 's/^/    /'
grew=$(grep -c junction-libc /tmp/heap_trace.txt 2>/dev/null); grew=${grew:-0}
echo "    LibOS heap growth events: ${grew}"
echo "    (each one is an arena mapping now, visible to every address space)"

echo
if [ "${fail}" -eq 0 ]; then
    echo "PASS - the heap reached every address space in all ${ATTEMPTS} attempts"
    exit 0
fi
echo "FAIL - crashed in ${fail}/${ATTEMPTS} forced attempts"
exit 1
