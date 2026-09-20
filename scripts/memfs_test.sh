#!/bin/bash
#
# memfs_test.sh - Phase 1 of docs/fork-implementation-plan.md.
#
# memfs was the one propagation failure reachable without scale: a file created
# after a fork, read by the child, killed the child with SIGSEGV at
# 0x380000000000 -- an ordinary shell idiom, not a stress test.
#
# Three changes fix it, and all three are needed:
#
#   1a  memfs extents moved above kVirtualAreaMax, into a region no guest can
#       be given an address in.
#   1b  one allocator instead of two, so a file's address and its memfd offset
#       are the same number plus a constant. Without this a fault carries no
#       information about what belongs at the address.
#   1c  a fault handler that maps the enclosing extent at the matching offset
#       and retries the instruction.
#
# Every case here is checked against native Linux by the same script, because
# "does not crash" is not the bar -- matching Linux is.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_stacktest.config
fail=0

[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"

for t in memfs_race memfs_reuse memfs_many; do
    cc -O1 -w -o "${BUILD}/${t}" "${ROOT}/docs/traces/${t}.c" || exit 2
done

# Junction's /tmp is memfs; the host's is not. Running the native comparison in
# the host's /tmp would leave files behind that Junction then resolves through
# the read-only linuxfs instead of memfs, and the next run fails with EPERM.
NATIVE_TMP=$(mktemp -d)
trap 'rm -rf "${NATIVE_TMP}"' EXIT

check() { # name, expected-substring, command...
    local name=$1 want=$2; shift 2
    local out
    out=$(cd "${BUILD}" && timeout 120 "$@" 2>&1)
    if echo "${out}" | grep -q -- "${want}"; then
        printf "  %-50s PASS\n" "${name}"
    else
        printf "  %-50s FAIL\n" "${name}"
        echo "${out}" | grep -E "child:|fault|segfault|Panic|Abort|error" |
            head -3 | sed 's/^/        /'
        fail=1
    fi
}

echo "------------------------------------------------------------"
echo "native Linux, for the results Junction has to match"
echo "------------------------------------------------------------"
check "native: file created after fork"  "first=A"                 \
      env TMPDIR="${NATIVE_TMP}" "${BUILD}/memfs_race"
check "native: recycled slot"            "second read = B"         \
      env TMPDIR="${NATIVE_TMP}" "${BUILD}/memfs_reuse"
check "native: eight extents"            "all 8 files correct"     \
      env TMPDIR="${NATIVE_TMP}" "${BUILD}/memfs_many"

echo
echo "------------------------------------------------------------"
echo "Junction: the same programs, over memfs"
echo "------------------------------------------------------------"
check "file created after fork, child reads it" "first=A" \
      ./junction_run "${CFG}" -- ./memfs_race
check "  ... and the child exits cleanly"       "child: exited 0" \
      ./junction_run "${CFG}" -- ./memfs_race
check "  ... via the fault handler, not by luck" "arena: repaired a fault in memfs" \
      ./junction_run "${CFG}" -- ./memfs_race
check "a recycled slot serves the new file"     "second read = B" \
      ./junction_run "${CFG}" -- ./memfs_reuse
check "eight extents, eight offsets"            "all 8 files correct" \
      ./junction_run "${CFG}" -- ./memfs_many

echo
echo "------------------------------------------------------------"
echo "the shell idiom that started this"
echo "------------------------------------------------------------"
check "sh: write a file, read it in a subshell" "hi" \
      ./junction_run "${CFG}" -- /bin/sh -c 'echo hi > /tmp/memfs_sh.dat; ( cat /tmp/memfs_sh.dat )'
check "sh: background subshell reads it"        "hi" \
      ./junction_run "${CFG}" -- /bin/sh -c 'echo hi > /tmp/memfs_sh2.dat; ( cat /tmp/memfs_sh2.dat ) & wait'

echo
echo "------------------------------------------------------------"
echo "the region is the LibOS's, and the audit agrees"
echo "------------------------------------------------------------"
# Any base at or above kVirtualAreaMax (0x500000000000); the region is placed
# at startup and only prefers a fixed address, so do not pin the test to one.
check "extents live above kVirtualAreaMax"      "arena: memfs manages 0x[567][0-9a-f]\{11\}-" \
      ./junction_run "${CFG}" -- /bin/true
check "no address space diverges afterwards"    "child: exited 0" \
      ./junction_run "${CFG}" --debug_as_audit -- ./memfs_race

echo
echo "------------------------------------------------------------"
echo "FIFOs: mkfifo, the open rendezvous, EOF, reopen, poll"
echo "------------------------------------------------------------"
# memfs had no FIFOs (mknod S_IFIFO was EINVAL); dash's process substitution
# idioms, GNU make's jobserver on older versions, and anything using mkfifo(1)
# failed. Each script here runs with T set to a scratch directory: the host's
# for native, memfs's /tmp under Junction. Junction's output must match.
fifo_case() { # name, expected-substring, sh-script
    local name=$1 want=$2 script=$3
    check "native: ${name}" "${want}" \
          /bin/sh -c "T=${NATIVE_TMP}/fifo; rm -rf \$T; mkdir -p \$T; ${script}"
    check "${name}" "${want}" \
          ./junction_run "${CFG}" -- \
          /bin/sh -c "T=/tmp/fifo; rm -rf \$T; mkdir -p \$T; ${script}"
}
fifo_case "writer first: background echo, then cat" "got=hello" \
    'mkfifo $T/f && (echo hello > $T/f &) ; echo got=$(cat $T/f)'
fifo_case "reader first: cat waits in open for a writer" "got=world" \
    'mkfifo $T/f; cat $T/f > $T/o & sleep 0.2; echo world > $T/f; wait; echo got=$(cat $T/o)'
fifo_case "100000 lines through a full pipe, EOF at close" "lines=100000" \
    'mkfifo $T/f; seq 1 100000 > $T/f & echo lines=$(wc -l < $T/f); wait'
fifo_case "reopen: a second reader after the writer closed" "got=one two" \
    'mkfifo $T/f; (echo one > $T/f; sleep 0.2; echo two > $T/f) & a=$(cat $T/f); b=$(cat $T/f); wait; echo "got=$a $b"'
fifo_case "test -p sees S_IFIFO" "isfifo=yes" \
    'mkfifo $T/f && [ -p $T/f ] && echo isfifo=yes'
fifo_case "O_WRONLY|O_NONBLOCK with no reader is ENXIO" "errno=6" \
    '/usr/bin/python3 -c "
import os
os.mkfifo(\"$T/p\")
try:
    os.open(\"$T/p\", os.O_WRONLY | os.O_NONBLOCK); print(\"errno=0\")
except OSError as e:
    print(\"errno=%d\" % e.errno)"'
fifo_case "select: readable after a write, EOF after the writer closes" "poll=True,x,True," \
    '/usr/bin/python3 -c "
import os, select
p = \"$T/q\"; os.mkfifo(p)
r = os.open(p, os.O_RDONLY | os.O_NONBLOCK); w = os.open(p, os.O_WRONLY)
os.write(w, b\"x\")
a = select.select([r], [], [], 2)[0] == [r]; d = os.read(r, 10).decode()
os.close(w)
b = select.select([r], [], [], 2)[0] == [r]; e = os.read(r, 10).decode()
print(\"poll=%s,%s,%s,%s\" % (a, d, b, e))"'

echo
if [ "${fail}" -eq 0 ]; then echo "PASS"; exit 0; fi
echo "FAIL"; exit 1
