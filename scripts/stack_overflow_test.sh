#!/bin/bash
#
# stack_overflow_test.sh - the Caladan stack pool overflow crashes Junction.
#
# WHAT IS BROKEN
#
# Caladan pre-reserves a pool of uthread stacks as one MAP_SHARED mapping, so
# every address space sees them. That reservation is the only thing keeping
# stack allocation from mapping memory into a single address space. Past it,
# stack_create() mmaps into whichever mm the allocating core is bound to, the
# stack is later returned to a global free list, and a uthread handed it under
# a *different* address space faults on its own stack -- with no frame to
# report the fault from.
#
# HOW THIS REPRODUCES IT
#
# Ordering matters, and the test program encodes it:
#   1. fork() while the pool is still intact, so the child's address space is
#      cloned before any overflow stack exists;
#   2. the parent spawns enough threads to push the high-water mark up. Those
#      stacks are mapped into the parent's address space only;
#   3. the parent joins, returning them to the free list;
#   4. the child spawns threads and is handed them.
#
# The pool is 16384 stacks, so an honest reproduction needs that many
# concurrent uthreads. JUNCTION_DEBUG_STACK_POOL_ENTRIES shrinks the
# reservation, reaching the identical code path at a testable size.
#
# WHY IT RETRIES
#
# Whether the child draws a parent-only stack depends on per-kthread tcache
# magazine state, so a single attempt crashes about 80% of the time. The
# property under test is "Junction must never crash here", so N attempts is
# strictly more sensitive than one, and after the fix every attempt must pass.
# More rounds inside one attempt does NOT help: once round 1 warms the child's
# magazines with safe stacks, later rounds reuse them.
#
# EXPECTED TODAY: native passes, Junction crashes. This is a regression test
# for the fix in docs/fork-implementation-plan.md and should flip to PASS.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
SRC=${ROOT}/docs/traces/stack_overflow_race.c
BIN=${BUILD}/stack_overflow_race
CFG=${BUILD}/caladan_stacktest.config
POOL=${JUNCTION_DEBUG_STACK_POOL_ENTRIES:-1}
PARENT=${PARENT_THREADS:-2048}
CHILD=${CHILD_THREADS:-512}
ROUNDS=${CHILD_ROUNDS:-8}
ATTEMPTS=${ATTEMPTS:-5}

cc -O1 -w -pthread -o "${BIN}" "${SRC}" || exit 2
sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}" || exit 2

echo "------------------------------------------------------------"
echo "Caladan stack pool overflow across address spaces"
echo "  pool=${POOL} stacks  parent=${PARENT}  child=${CHILD}x${ROUNDS}"
echo "------------------------------------------------------------"

echo "[native linux]"
if ! "${BIN}" "${PARENT}" "${CHILD}" "${ROUNDS}"; then
    echo "  FAIL - the test itself is broken, not Junction"
    exit 2
fi
echo "  PASS"
echo

echo "[junction, ${ATTEMPTS} attempts - any crash is a failure]"
crashes=0
for i in $(seq 1 "${ATTEMPTS}"); do
    out=$(cd "${BUILD}" && JUNCTION_DEBUG_STACK_POOL_ENTRIES=${POOL} \
          timeout 300 ./junction_run "${CFG}" -- \
          "${BIN}" "${PARENT}" "${CHILD}" "${ROUNDS}" 2>&1)
    if echo "${out}" | grep -q "child: ok"; then
        echo "  attempt ${i}: ok"
    else
        crashes=$((crashes + 1))
        fault=$(echo "${out}" | grep -oE "addr=[0-9a-f]+ .*" | head -1)
        echo "  attempt ${i}: CRASH  ${fault}"
    fi
done

echo
if [ "${crashes}" -eq 0 ]; then
    echo "PASS - junction matched native in all ${ATTEMPTS} attempts"
    exit 0
fi
echo "FAIL - crashed in ${crashes}/${ATTEMPTS} attempts"
echo "       The fault address lies in the runtime stack region"
echo "       (STACK_BASE_ADDR 0x520000000000): a uthread stack the child's"
echo "       address space never had mapped. This is the bug the memfd arena"
echo "       and lazy propagation are meant to fix."
exit 1
