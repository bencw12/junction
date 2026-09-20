#!/bin/bash
#
# app_test.sh - real programs that fork, natively and under Junction.
#
# Everything before this tested the mechanism: memfs, the arena, the regions,
# GC, each with a program written to reach one path. This runs ordinary
# software that forks the way software actually does -- shells spawning
# pipelines and subshells, Python's multiprocessing (fork start method) and
# subprocess, make -j driving the compiler, git -- and holds Junction to the
# same bar as everywhere else: the output must match native Linux.
#
# Each case prints one deterministic summary line ("<name>: total=N") from
# both runs; a mismatch, a crash, or the fault-handler's own alarms are a
# failure. A subset also runs under --debug_as_audit, which diffs every live
# address space after every fork, so a LibOS mapping that reached only one of
# them is reported rather than left to be found by a later crash.
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ROOT}/build/junction
CFG=${BUILD}/caladan_test.config
APP=${BUILD}/apptest
NATIVE_TMP=$(mktemp -d)
trap 'rm -rf "${NATIVE_TMP}"' EXIT
fail=0

# ---------------------------------------------------------------------------
# inputs. Host paths, readable from inside Junction through linuxfs; anything a
# guest *writes* goes to Junction's /tmp, which is memfs.
# ---------------------------------------------------------------------------
mkdir -p "${APP}/mk"

cat > "${APP}/shell_loop.sh" <<'EOF'
#!/bin/sh
# 240 iterations of: a backgrounded subshell running a pipeline, plus a command
# substitution -- fork, pipe, wait -- with 16 subshells in flight at a time.
total=0; i=0
while [ $i -lt 240 ]; do
  ( printf '%s\n' "$i" | tr 0-9 a-j | wc -c > /dev/null ) &
  x=$(printf '%s' "$i" | wc -c); total=$((total + x))
  i=$((i + 1)); [ $((i % 16)) -eq 0 ] && wait
done
wait
echo "shell: total=$total"
EOF

cat > "${APP}/mp_pool.py" <<'EOF'
import multiprocessing as mp, hashlib
def work(i):
    return int(hashlib.sha256(str(i).encode()).hexdigest()[:8], 16) % 1000
if __name__ == "__main__":
    mp.set_start_method("fork")
    total = 0
    for _ in range(5):                       # five pools: create, use, tear down
        with mp.Pool(8) as p:
            total += sum(p.map(work, range(400)))
    print("mp: total=%d" % total)
EOF

cat > "${APP}/subproc.py" <<'EOF'
import subprocess
total = 0
for i in range(150):                          # fork + exec, capture, wait
    r = subprocess.run(["/bin/sh", "-c", "echo %d | wc -c" % i],
                       capture_output=True, text=True)
    total += int(r.stdout.strip())
print("subproc: total=%d" % total)
EOF

cat > "${APP}/churn.py" <<'EOF'
# Short-lived workers for a fixed time: many address spaces created and
# destroyed in sequence, so the arena's GC has something to retire.
import multiprocessing as mp, time, os
def work(i):
    return os.getpid() != 0
if __name__ == "__main__":
    mp.set_start_method("fork")
    end = time.time() + float(os.environ.get("CHURN_SECS", "20")); rounds = 0
    while time.time() < end:
        with mp.Pool(4) as p:
            assert all(p.map(work, range(8)))
        rounds += 1
    print("churn: rounds>=%d" % (1 if rounds >= 1 else 0))
EOF

for i in $(seq 1 24); do
    printf 'int f%d(void) { return %d; }\n' "$i" "$i" > "${APP}/mk/f$i.c"
done
{
    echo '#include <stdio.h>'
    for i in $(seq 1 24); do echo "int f$i(void);"; done
    echo 'int main(void) { int s = 0;'
    for i in $(seq 1 24); do echo "  s += f$i();"; done
    echo '  printf("make: total=%d\n", s); return 0; }'
} > "${APP}/mk/main.c"
# make -j drives shell commands rather than cc: gcc's cc1 is a non-relocatable
# binary and Junction's loader allows one of those process-wide ("Cannot load
# multiple non-relocatable binaries"), which make -j8 exceeds. That limit
# predates address spaces -- and is exactly the kind of thing per-process
# address spaces could lift -- but it is not this test's subject. The single
# cc case below pins the current behaviour so a change is noticed.
cat > "${APP}/mk/Makefile" <<'EOF'
SRCS := $(wildcard *.c)
OUTS := $(patsubst %.c,%.out,$(SRCS))
all: $(OUTS)
	@cat $(OUTS) | awk '{s += $$1} END {print "make: total=" s}'
%.out: %.c
	@sh -c 'grep -o "return [0-9]*" $< | tr -dc 0-9; echo' > $@
EOF

# ---------------------------------------------------------------------------
# harness
# ---------------------------------------------------------------------------
ALARMS='Aborting on signal|ASSERTION|double ready|unhandled segfault|segfault in syscall|fault with preemption disabled|Panic'

native() { (cd "${NATIVE_TMP}" && timeout 300 "$@" 2>&1); }
junction() { (cd "${BUILD}" && timeout 300 ./junction_run "${CFG}" "$@" 2>&1); }

# compare NAME KEY native-cmd... -- junction-args...
compare() {
    local name=$1 key=$2; shift 2
    local ncmd=() ; while [ "$1" != "--" ]; do ncmd+=("$1"); shift; done; shift
    local nout jout nline jline
    nout=$(native "${ncmd[@]}")
    jout=$(junction "$@")
    nline=$(echo "${nout}" | grep -E "^${key}: " | head -1)
    jline=$(echo "${jout}" | grep -E "^${key}: " | head -1)
    if [ -z "${nline}" ]; then
        printf "  %-52s SKIP (does not run natively here)\n" "${name}"
        echo "${nout}" | tail -2 | sed 's/^/        /'
        return
    fi
    if echo "${jout}" | grep -qE "${ALARMS}"; then
        printf "  %-52s FAIL (junction alarm)\n" "${name}"
        echo "${jout}" | grep -E "${ALARMS}" | head -2 | sed 's/^/        /'
        fail=1
    elif [ "${nline}" = "${jline}" ]; then
        printf "  %-52s PASS  %s\n" "${name}" "${jline}"
    else
        printf "  %-52s FAIL\n" "${name}"
        echo "        native:   ${nline:-<none>}"
        echo "        junction: ${jline:-<none>}"
        echo "${jout}" | grep -vE "^\[|^CPU" | tail -3 | sed 's/^/        /'
        fail=1
    fi
}

# audited NAME KEY junction-args... : the same program under --debug_as_audit,
# which must print no divergence.
audited() {
    local name=$1 key=$2; shift 2
    local jout
    jout=$(junction --debug_as_audit "$@")
    if echo "${jout}" | grep -qE "${ALARMS}|LibOS mapping\(s\) absent|differently protected"; then
        printf "  %-52s FAIL\n" "${name}"
        echo "${jout}" | grep -E "${ALARMS}|absent|differently" | head -3 | sed 's/^/        /'
        fail=1
    elif echo "${jout}" | grep -qE "^${key}: "; then
        printf "  %-52s PASS\n" "${name}"
    else
        printf "  %-52s FAIL (no result line)\n" "${name}"
        fail=1
    fi
}

echo "------------------------------------------------------------"
echo "shells: pipelines, subshells, command substitution, jobs"
echo "------------------------------------------------------------"
compare "sh loop: 240 x (subshell | pipeline) & + \$(..)" shell \
        /bin/sh "${APP}/shell_loop.sh" -- -- /bin/sh "${APP}/shell_loop.sh"
compare "bash: same loop" shell \
        /bin/bash "${APP}/shell_loop.sh" -- -- /bin/bash "${APP}/shell_loop.sh"

echo
echo "------------------------------------------------------------"
echo "python: multiprocessing (fork) and subprocess (fork+exec)"
echo "------------------------------------------------------------"
compare "multiprocessing.Pool(8) x5, 2000 tasks" mp \
        /usr/bin/python3 "${APP}/mp_pool.py" -- -- /usr/bin/python3 "${APP}/mp_pool.py"
compare "subprocess.run x150" subproc \
        /usr/bin/python3 "${APP}/subproc.py" -- -- /usr/bin/python3 "${APP}/subproc.py"

echo
echo "------------------------------------------------------------"
echo "toolchains: make -j8 driving cc, and git"
echo "------------------------------------------------------------"
rm -rf "${NATIVE_TMP}/mk" && cp -r "${APP}/mk" "${NATIVE_TMP}/mk"
# Inputs are copied file by file so this case is about make alone; cp -r has
# its own case next.
compare "make -j8: 25 targets, each a forked shell" make \
        /bin/sh -c "make -s -C ${NATIVE_TMP}/mk -j8" -- -- \
        /bin/sh -c "rm -rf /tmp/mk && mkdir /tmp/mk && cp ${APP}/mk/* /tmp/mk/ && make -s -C /tmp/mk -j8"
# coreutils 9.4's cp -r calls fchmodat2 (Linux 6.6, syscall 452) once per
# directory. Until the entry trampolines moved to the top of the table (slots
# 507-511, see junction/syscall/systbl.h) 452 was a trampoline slot and the call
# landed in it instead of a handler. Compare the copied tree with native's.
compare "cp -r (calls fchmodat2, syscall 452)" cpr \
        /bin/sh -c "rm -rf ${NATIVE_TMP}/cpr && cp -r ${APP}/mk ${NATIVE_TMP}/cpr && cd ${NATIVE_TMP}/cpr && echo cpr: \$(ls | wc -l) files \$(cat * | md5sum | cut -c1-12)" -- -- \
        /bin/sh -c "rm -rf /tmp/cpr && cp -r ${APP}/mk /tmp/cpr && cd /tmp/cpr && echo cpr: \$(ls | wc -l) files \$(cat * | md5sum | cut -c1-12)"
# Known limit, pinned: one non-relocatable binary at a time.
kout=$(junction -- /bin/sh -c "rm -rf /tmp/cc1 && mkdir /tmp/cc1 && cd /tmp/cc1 && cp ${APP}/mk/f1.c . && cc -c f1.c && echo CC_OK")
if echo "${kout}" | grep -q "CC_OK"; then
    printf "  %-52s PASS  (limit lifted: note it)\n" "cc: compile one file (gcc's cc1 is non-PIE)"
elif echo "${kout}" | grep -q "Cannot load multiple non-relocatable"; then
    printf "  %-52s KNOWN LIMIT (one non-relocatable binary)\n" "cc: compile one file (gcc's cc1 is non-PIE)"
else
    printf "  %-52s FAIL (neither compiled nor the known message)\n" "cc: compile one file (gcc's cc1 is non-PIE)"
    echo "${kout}" | grep -vE "^\[|^CPU" | tail -2 | sed 's/^/        /'; fail=1
fi
compare "git log over the repo (read-only, forks a pager-less walk)" git \
        /bin/sh -c "echo git: total=\$(git -C ${ROOT} --no-pager log --oneline -n 200 | wc -l)" -- -- \
        /bin/sh -c "echo git: total=\$(git -C ${ROOT} --no-pager log --oneline -n 200 | wc -l)"

echo
echo "------------------------------------------------------------"
echo "the same, with every address space audited after every fork"
echo "------------------------------------------------------------"
audited "sh loop, audited" shell -- /bin/sh "${APP}/shell_loop.sh"
audited "multiprocessing, audited" mp -- /usr/bin/python3 "${APP}/mp_pool.py"
audited "subprocess, audited" subproc -- /usr/bin/python3 "${APP}/subproc.py"

echo
echo "------------------------------------------------------------"
echo "churn: ${CHURN_SECS:-20} s of short-lived worker pools"
echo "------------------------------------------------------------"
compare "churn.py: pools created and torn down for ${CHURN_SECS:-20} s" churn \
        /usr/bin/env CHURN_SECS="${CHURN_SECS:-20}" /usr/bin/python3 "${APP}/churn.py" -- -- \
        /usr/bin/env CHURN_SECS="${CHURN_SECS:-20}" /usr/bin/python3 "${APP}/churn.py"

echo
if [ "${fail}" -eq 0 ]; then echo "PASS"; else echo "FAIL"; fi
exit "${fail}"
