#!/bin/bash
#
# memtrace_report.sh - summarise a LibOS memory trace.
#
#   build/junction/junction_run <config> --trace_libos_mem /tmp/t.txt -- <prog>
#   scripts/memtrace_report.sh /tmp/t.txt
#
# The question the trace answers is not "how many mappings" but "who made them
# and where did they land". A LibOS mapping made after a guest address space
# exists is invisible to that address space, so those are the events that
# matter. See docs/libos-memory-plan.md.

set -u
T=${1:?usage: memtrace_report.sh <trace file>}
[ -r "$T" ] || { echo "cannot read $T" >&2; exit 1; }

n() { awk "!/^#/ && ($1)" "$T" | wc -l; }

echo "=== $T"
echo "events: $(grep -vc '^#' "$T")"
echo

echo "--- who, where, what ---"
awk '!/^#/ {printf "%-14s %-12s %s\n", $3, $4, $2}' "$T" |
    sort | uniq -c | sort -rn
echo

echo "--- the events that matter ---"
printf '%-52s %s\n' "LibOS-range mappings (must reach every address space)" "$(n '$4=="libos-range"')"
printf '%-52s %s\n' "  ...from Junction's own glibc (trapped by seccomp)" "$(n '$3=="junction-libc"')"
printf '%-52s %s\n' "  ...from the Caladan runtime (pool overflow)" "$(n '$3=="caladan"')"
printf '%-52s %s\n' "executable file mappings (would need a window VMA)" "$(n '$2 ~ /file/ && and(strtonum($7),4)')"
printf '%-52s %s\n' "unmaps / hole punches" "$(n '$2 ~ /munmap/')"
printf '%-52s %s\n' "protection changes" "$(n '$2=="mprotect"')"
echo

# Junction decides "is this my own libc?" from preemption state, and the code
# says "probably". A LibOS-internal event seen with preemption enabled is a
# case where that heuristic would have misfiled the syscall as a guest's.
MIS=$(n '$3=="junction-libc" && $16==1')
printf '%-52s %s\n' "junction-libc events with preemption ENABLED" "$MIS"
if [ "$MIS" -gt 0 ]; then
    echo "  ^ these would be misclassified as guest syscalls by the"
    echo "    !preempt_enabled() heuristic in syscall_trap_handler()"
fi
echo

if [ "$(n '$4=="libos-range"')" -gt 0 ]; then
    echo "--- LibOS-range detail ---"
    awk '!/^#/ && $4=="libos-range" {printf "  %-16s %-14s %-14s len=%-10s prot=%s flags=%s\n", $2, $3, $5, $6, $7, $8}' "$T" |
        head -40
fi
