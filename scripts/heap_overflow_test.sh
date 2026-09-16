#!/bin/bash
#
# heap_overflow_test.sh - Junction's own glibc heap across address spaces.
#
# Junction pre-grows its heap by 64 MB at init and the address-space sweep makes
# that region MAP_SHARED, so every address space sees it. Past the reserve glibc
# cannot use brk (the seccomp handler forces it to 0), so it falls back to mmap,
# which the SIGSYS handler executes natively into whichever address space the
# calling core is bound to. glibc's free lists are shared LibOS state, so that
# chunk can then be handed to LibOS code running under a different address
# space.
#
# Part 1 forces the allocation with --debug_libos_alloc and dereferences it from
# another address space. Part 2 asks whether a *guest* can drive the same heap
# growth on its own -- it cannot, at the sizes tried, which is the more useful
# result and the reason this ranks below the stack pool.
#
# EXPECTED TODAY: part 1 crashes. Regression test for the fix in
# docs/fork-implementation-plan.md.
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
    if echo "${out}" | grep -q "Aborting on signal"; then
        addr=$(echo "${out}" | grep -oE "at 0x[0-9a-f]+" | head -1)
        echo "    attempt ${i}: CRASH ${addr} (ABSENT, then dereferenced)"
        fail=$((fail + 1))
    elif echo "${out}" | grep -q "PRESENT"; then
        echo "    attempt ${i}: ok - mapping reached the other address space"
    else
        echo "    attempt ${i}: inconclusive"
    fi
done

echo
echo "[2] guest-driven: can a guest grow the LibOS heap past the reserve?"
out=$(cd "${BUILD}" && JUNCTION_DEBUG_HEAP_RESERVE_MB=0 timeout 240 \
      ./junction_run "${CFG}" --trace_libos_mem /tmp/heap_trace.txt -- \
      ./heap_overflow_race "${SPLITS}" $((SPLITS / 8)) 3 2>&1)
echo "${out}" | grep -E "parent:|child:" | sed 's/^/    /'
grew=$(grep -c junction-libc /tmp/heap_trace.txt 2>/dev/null); grew=${grew:-0}
echo "    LibOS heap growth events: ${grew}"
[ "${grew}" -eq 0 ] && echo "    (reserve absorbed it - see docs/shared-memory-audit.md)"

echo
if [ "${fail}" -eq 0 ]; then
    echo "PASS - the heap reached every address space in all ${ATTEMPTS} attempts"
    exit 0
fi
echo "FAIL - crashed in ${fail}/${ATTEMPTS} forced attempts"
exit 1
