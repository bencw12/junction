#!/bin/bash
#
# stack_overflow_test.sh - uthread stacks across address spaces.
#
# WHAT THIS GUARDS
#
# Caladan's uthread stacks used to come from a pre-reserved pool: one
# MAP_SHARED mapping made before the first clone, so every address space saw
# it. Past the pool, stack_create() mmapped into whichever mm the allocating
# core was bound to, the stack was later returned to a global free list, and a
# uthread handed it under a *different* address space faulted on its own stack
# -- with no frame to report the fault from.
#
# The pool is gone (Phase 3a of docs/fork-implementation-plan.md). Stacks now
# live in a region backed 1:1 by a memfd, reserved over the whole index space
# and mapped on first use, so an address space that never received a stack
# repairs it on first touch. There is no reservation to overflow.
#
# HOW THIS EXERCISES IT
#
# The ordering that used to crash, kept exactly:
#   1. fork() first, so the child's address space is cloned before any of the
#      parent's stacks below exist;
#   2. the parent spawns enough threads to create thousands of new stacks,
#      all mapped into the parent's address space only;
#   3. the parent joins, returning them to the free list;
#   4. the child spawns threads and is handed them -- each first touch is a
#      fault the child's address space has to repair.
#
# WHY IT RETRIES
#
# Whether the child draws a parent-created stack depends on per-kthread tcache
# magazine state, so one attempt is not a guarantee. The property is "Junction
# must never crash here", so N attempts is strictly more sensitive than one.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
SRC=${ROOT}/docs/traces/stack_overflow_race.c
BIN=${BUILD}/stack_overflow_race
CFG=${BUILD}/caladan_stacktest.config
PARENT=${PARENT_THREADS:-2048}
CHILD=${CHILD_THREADS:-512}
ROUNDS=${CHILD_ROUNDS:-8}
ATTEMPTS=${ATTEMPTS:-5}

cc -O1 -w -pthread -o "${BIN}" "${SRC}" || exit 2
sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}" || exit 2

echo "------------------------------------------------------------"
echo "Caladan uthread stacks across address spaces"
echo "  parent=${PARENT}  child=${CHILD}x${ROUNDS}"
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
    out=$(cd "${BUILD}" && timeout 300 ./junction_run "${CFG}" -- \
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
echo "       address space never had mapped, and the fault handler did not"
echo "       repair it."
exit 1
