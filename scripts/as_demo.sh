#!/bin/bash
#
# as_demo.sh - shows that Junction's scheduling threads really are running in
# different address spaces at the same time.
#
# Starts bench/fork/as_demo inside a Junction container. It forks a few guest
# processes, each of which fills one private region with a distinctive byte and
# then sits in a loop re-checking it.
#
# The interesting part is that all of those guests hold that region at the *same
# virtual address* -- that is what fork means, and it is exactly what a single
# address space cannot provide. So the mappings look identical from outside;
# what differs is the memory behind them. This script reads that one address
# through /proc/<junction pid>/task/<tid>/mem for every scheduling thread.
# procfs reads through the *task's* mm, so each thread sees whatever guest its
# core is currently bound to.
#
# Different bytes at one address, in one Linux process, at one instant.

set -u

ROOT=$(dirname "$(readlink -f "$0")")/..
BUILD=${ROOT}/build/junction
CONFIG=${1:-${BUILD}/caladan_test.config}
DEMO=${ROOT}/bench/fork/as_demo
SECONDS_TO_RUN=${2:-20}
LOG=/tmp/as_demo.log

if [ ! -x "${DEMO}" ]; then
    echo "build the benchmarks first: make -C ${ROOT}/bench/fork" >&2
    exit 1
fi

cd "${BUILD}" || exit 1
./junction_run "${CONFIG}" -- "${DEMO}" 3 "${SECONDS_TO_RUN}" > "${LOG}" 2>&1 &

# Wait for every guest to have claimed its token.
for _ in $(seq 1 60); do
    sleep 0.5
    grep -q "child 2" "${LOG}" 2>/dev/null && break
done

PID=$(pgrep -x junction_run | head -1)
REGION=$(awk '/^region /{print $2; exit}' "${LOG}")
if [ -z "${PID:-}" ] || [ -z "${REGION:-}" ]; then
    echo "junction_run did not get going; see ${LOG}" >&2
    exit 1
fi

echo "junction host pid ${PID}, guest region at ${REGION}"
grep -E "^(parent|child) " "${LOG}"
echo
echo "reading that one address through each scheduling thread:"
echo
printf '%-10s  %s\n' "thread" "byte at ${REGION}"
printf '%-10s  %s\n' "----------" "-----------------"

OFF=$((REGION))
TOKENS=""
for t in /proc/"${PID}"/task/*; do
    tid=$(basename "$t")
    byte=$(sudo dd if="$t/mem" bs=1 count=1 skip="${OFF}" status=none 2>/dev/null |
           od -An -tu1 | tr -d ' \n')
    [ -z "${byte}" ] && byte="(unreadable)"
    printf '%-10s  %s\n' "${tid}" "${byte}"
    TOKENS="${TOKENS} ${byte}"
done

DISTINCT=$(echo ${TOKENS} | tr ' ' '\n' | grep -v '^$' | sort -u | wc -l)
echo
echo "distinct values seen at one address, at one instant: ${DISTINCT}"
if [ "${DISTINCT}" -gt 1 ]; then
    echo "=> the scheduling threads of this single Linux process are bound to"
    echo "   different address spaces"
else
    echo "=> all cores happen to be bound to the same guest right now; re-run,"
    echo "   or raise the guest count in as_demo"
fi

wait
echo
tail -5 "${LOG}"
