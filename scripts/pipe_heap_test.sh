#!/bin/bash
#
# pipe_heap_test.sh - a plain guest program crashes Junction.
#
# No LibOS knobs, no forcing, stock configuration. Just a guest that opens
# pipes.
#
# WHY IT WORKS
#
# junction/new_override.cc routes every C++ allocation under 256 KB to
# Caladan's smalloc, not to glibc:
#
#     constexpr size_t kMaxAllocSize = (1UL << 18);      // 256 KB
#     if (unlikely(size > kMaxAllocSize)) return std::malloc(size);
#     return smalloc(size);
#
# smalloc is backed by slabs, slabs by page_alloc, page_alloc by the 1 GB
# reserved large-page pool. And a pipe costs a 64 KB LibOS buffer:
#
#     inline constexpr size_t kPipeSize = 16 * kPageSize;      // limits.h
#     auto pipe = std::make_shared<StreamPipe>(kPipeSize);     // fs/pipe.cc
#     std::vector<std::byte> buf_;                             // byte_channel.h
#
# So ~16000 concurrent pipes exhausts the pool, and lgpage_create() starts
# mmap'ing into whichever address space the allocating core is bound to.
#
# ORDERING
#   1. fork() first, so the child's address space is cloned before the pool
#      has overflowed;
#   2. the parent opens 17000 pipes; the new large pages are mapped into the
#      PARENT's address space only;
#   3. the parent closes them, returning the buffers to smalloc;
#   4. the child opens pipes and Junction writes a pipe buffer into memory the
#      child's address space has never had.
#
# EXPECTED TODAY: native passes, Junction faults inside the large-page pool
# region (PAGE_BASE_ADDR 0x510000000000). Regression test for the fix in
# docs/fork-implementation-plan.md.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
BIN=${BUILD}/pipe_heap_race
CFG=${BUILD}/caladan_stacktest.config
PARENT=${PARENT_PIPES:-17000}
CHILD=${CHILD_PIPES:-2000}
ROUNDS=${CHILD_ROUNDS:-4}
ATTEMPTS=${ATTEMPTS:-3}

cc -O1 -w -o "${BIN}" "${ROOT}/docs/traces/pipe_heap_race.c" || exit 2
[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"

echo "------------------------------------------------------------"
echo "Guest-driven LibOS allocation: ${PARENT} pipes (~$((PARENT * 64 / 1024)) MB)"
echo "------------------------------------------------------------"

echo "[native linux]"
if ! "${BIN}" "${PARENT}" "${CHILD}" "${ROUNDS}" >/dev/null 2>&1; then
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
        echo "  attempt ${i}: CRASH $(echo "${out}" | grep -oE 'addr=[0-9a-f]+ .*' | head -1)"
    fi
done

echo
if [ "${crashes}" -eq 0 ]; then
    echo "PASS - junction matched native in all ${ATTEMPTS} attempts"
    exit 0
fi
echo "FAIL - crashed in ${crashes}/${ATTEMPTS} attempts"
echo "       The fault address lies in the Caladan large-page pool"
echo "       (PAGE_BASE_ADDR 0x510000000000), past the 1 GB reservation:"
echo "       LibOS memory the child's address space never had mapped."
exit 1
