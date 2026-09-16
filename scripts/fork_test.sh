#!/bin/bash
#
# fork_test.sh - verify that fork() works inside Junction.
#
# Runs the same binaries natively and inside a Junction container and prints
# both results, because the comparison is the evidence: these are plain Linux
# programs that know nothing about Junction.
#
#   scripts/fork_test.sh              semantics + a short stress run
#   scripts/fork_test.sh --quick      semantics only
#   scripts/fork_test.sh --latency    also measure fork latency
#
# Prerequisites (see docs/multi-address-space.md):
#   sudo lib/caladan/scripts/setup_machine.sh nouintr
#   sudo lib/caladan/iokerneld ias nobw noht no_hw_qdel numanode -1 -- \
#        --allow 00:00.0 --vdev=net_tap0 &
#   sudo insmod kern/junction_as.ko

set -u

ROOT=$(dirname "$(readlink -f "$0")")/..
BUILD=${ROOT}/build/junction
BENCH=${ROOT}/bench/fork
CONFIG=${BUILD}/caladan_test.config
JRUN=${BUILD}/junction_run

QUICK=0
LATENCY=0
for a in "$@"; do
    case "$a" in
        --quick)   QUICK=1 ;;
        --latency) LATENCY=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

hr() { printf '%s\n' "------------------------------------------------------------"; }

# ---- prerequisites -------------------------------------------------------

if [ ! -x "${JRUN}" ]; then
    echo "Junction is not built. Run scripts/build.sh first." >&2
    exit 1
fi
if [ ! -x "${BENCH}/fork_stress" ]; then
    echo "Building the benchmarks..."
    make -s -C "${BENCH}" || exit 1
fi
if [ ! -e /dev/junction_as ]; then
    echo "WARNING: /dev/junction_as is missing, so Junction has no address-space"
    echo "support and fork() will report ENOSYS. Load it with:"
    echo "    sudo insmod ${ROOT}/kern/junction_as.ko"
    echo
fi
if ! pgrep -x iokerneld > /dev/null; then
    echo "ERROR: the Caladan iokernel is not running; Junction cannot start." >&2
    echo "See the header of this script for the command." >&2
    exit 1
fi

STRESS_ARGS="-t 4 -s 2 -f 32"
[ "${QUICK}" = 1 ] && STRESS_ARGS="-S"

rc=0

# ---- the module on its own ----------------------------------------------

if [ -x "${ROOT}/kern/as_test" ] && [ -e /dev/junction_as ]; then
    hr; echo "kernel module: address space isolation"; hr
    "${ROOT}/kern/as_test" | tail -4 || rc=1
    echo
fi

# ---- fork semantics, native then Junction -------------------------------

# Full output to a file, summary to the terminal.
#
# These sections used to be piped straight through `tail`, which is fine while
# everything passes and useless the moment something does not: a run that
# reported "46 checks, 1 failure(s)" had every line explaining why -- Junction's
# own fault and abort messages included -- already discarded by the time the
# failure was printed. Keep the evidence, show the summary.
NATIVE_LOG=$(mktemp); JUNCTION_LOG=$(mktemp)

hr; echo "fork semantics and stress: NATIVE LINUX"; hr
"${BENCH}/fork_stress" ${STRESS_ARGS} > "${NATIVE_LOG}" 2>&1 || rc=1
tail -5 "${NATIVE_LOG}"
echo

hr; echo "fork semantics and stress: JUNCTION"; hr
(cd "${BUILD}" && ./junction_run "${CONFIG}" -- "${BENCH}/fork_stress" ${STRESS_ARGS} 2>&1) \
    > "${JUNCTION_LOG}" || rc=1
grep -vE "CPU [0-9]+\| <5>" "${JUNCTION_LOG}" | grep -vE '^\[.*\*' | tail -8
echo

# Anything the tail cannot have shown, and that a reader would need.
for log in "${NATIVE_LOG}" "${JUNCTION_LOG}"; do
    # Deliberately NOT "exiting with code [^0]": several tests verify exit-status
    # propagation and expect codes like 42, 143 and 3, and the stress children
    # exit with their own token. Those are passes, not failures.
    if grep -qE "FAIL|fault with|segfault|Panic|Abort|ASSERTION" "${log}"; then
        [ "${log}" = "${NATIVE_LOG}" ] && echo "--- native, full detail:" \
                                      || echo "--- junction, full detail:"
        grep -nE "FAIL|fault with|segfault|Panic|Abort|ASSERTION" "${log}" |
            head -20 | sed 's/^/    /'
        echo "    (full log: ${log})"
        rc=1
        echo
    fi
done

# ---- latency -------------------------------------------------------------

if [ "${LATENCY}" = 1 ]; then
    hr; echo "fork() to the child's first instruction"; hr
    printf '%-10s %-12s %s\n' "footprint" "native" "junction"
    for m in 0 16 64; do
        n=$("${BENCH}/fork_latency" -n 200 -w 20 -m "$m" -c | tail -1 | cut -d, -f6)
        j=$(cd "${BUILD}" && ./junction_run "${CONFIG}" -- \
              "${BENCH}/fork_latency" -n 200 -w 20 -m "$m" -c 2>/dev/null \
              | tail -1 | cut -d, -f6)
        printf '%-10s %-12s %s\n' "${m} MB" "${n} ns" "${j} ns"
    done
    echo
fi

hr
if [ "${rc}" = 0 ]; then
    echo "done — both runs should report the same number of checks passing"
else
    echo "done — something failed above"
fi
exit ${rc}
