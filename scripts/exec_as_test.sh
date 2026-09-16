#!/bin/bash
#
# exec_as_test.sh - exec must keep a process in its own address space.
#
# Regression test for docs/bug-exec-timer-address-space.md: execve() built its
# new MemoryMap with MemoryMap::Create(), which never assigns an address-space
# handle, so the map defaulted to kRootAddressSpace while the new image had just
# been loaded into the caller's address space -- and the old map's destructor
# then released that address space out from under it.
#
# The failure needs the process to be descheduled and resumed, so every case
# here pairs an exec'd program that outlives the call with a second runnable
# process. `sleep 1 &` is enough. Neither scripts/fork_test.sh nor
# scripts/test.sh covered that combination, which is why this shipped.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_stacktest.config
BIN=${BUILD}/fork_exec_sleep
fail=0

[ -r "${CFG}" ] || sed 's/^runtime_kthreads .*/runtime_kthreads 2/' \
    "${BUILD}/caladan_test.config" > "${CFG}"
cc -O1 -w -o "${BIN}" "${ROOT}/docs/traces/fork_exec_sleep.c" || exit 2

echo "------------------------------------------------------------"
echo "exec address-space handover"
echo "------------------------------------------------------------"

check() { # name, expected-substring, command...
    local name=$1 want=$2; shift 2
    local out
    out=$(cd "${BUILD}" && timeout 120 "$@" 2>&1)
    if echo "${out}" | grep -q -- "${want}"; then
        printf "  %-46s PASS\n" "${name}"
    else
        printf "  %-46s FAIL\n" "${name}"
        echo "${out}" | grep -E "fault with|segfault in|Panic|exiting with code|failed to switch" |
            head -2 | sed 's/^/        /'
        fail=1
    fi
}

check "fork + exec sleep, parent concurrent" "child exited 0" \
      ./junction_run "${CFG}" -- "${BIN}" 0
check "fork + exec sleep, parent spinning" "child exited 0" \
      ./junction_run "${CFG}" -- "${BIN}" 1
check "sh: sleep 1 & wait" "survived" \
      ./junction_run "${CFG}" -- /bin/sh -c 'sleep 1 & wait; echo survived'
check "sh: two sequential execs" "OK" \
      ./junction_run "${CFG}" -- /bin/sh -c '/bin/true; /bin/true; echo OK'
check "sh: write then read a file, sequential" "hello" \
      ./junction_run "${CFG}" -- /bin/sh -c 'echo hello > /tmp/eat_a; sleep 0.2; cat /tmp/eat_a'
check "sh: file created before fork, child reads" "hello" \
      ./junction_run "${CFG}" -- /bin/sh -c 'echo hello > /tmp/eat_b; ( cat /tmp/eat_b ) & wait'

echo
if [ "${fail}" -eq 0 ]; then echo "PASS"; exit 0; fi
echo "FAIL"; exit 1
