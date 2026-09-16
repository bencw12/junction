#!/bin/bash
#
# directpath_trace.sh - run the LibOS memory trace set with directpath enabled.
#
# Answers the one question the static analysis cannot: does the mlx5 path map
# anything Junction has to care about, and does the UAR/BAR MMIO mapping
# (VM_IO|VM_PFNMAP) survive the clone() that makes a new address space?
#
# This RECONFIGURES THE ConnectX-7: SR-IOV VFs, switchdev eswitch mode, an
# openibd restart, and tc rules. 400gp1 loses its 10.40.1.105/16 identity for
# the duration. Restores at the end, or run scripts/../nic_restore.sh by hand.
#
#   sudo -v && scripts/directpath_trace.sh
#
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CALADAN=$ROOT/lib/caladan
BUILD=$ROOT/build/junction
IF=400gp1
PF=0000:9c:00.0
OUT=${1:-/tmp/directpath_traces}
mkdir -p "$OUT"

say() { echo; echo "=== $* ==="; }

restore() {
    say "restoring"
    sudo pkill -f iokerneld; sleep 2
    for d in $(ls /sys/class/net/); do sudo tc qdisc del dev $d ingress 2>/dev/null; done
    for v in /sys/bus/pci/devices/$PF/virtfn*; do
        [ -e "$v" ] || continue
        sudo "$CALADAN/dpdk/usertools/dpdk-devbind.py" -b mlx5_core \
            "$(basename "$(readlink "$v")")" 2>/dev/null
    done
    echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs >/dev/null
    sleep 1
    sudo devlink dev eswitch set pci/$PF mode legacy 2>/dev/null
    sudo /etc/init.d/openibd restart
    sudo ip link set $IF mtu 9000
    sudo ip addr add 10.40.1.105/16 broadcast 10.40.255.255 dev $IF 2>/dev/null
    sudo ip link set up $IF
    (cd "$CALADAN" && sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 -- \
        --allow 00:00.0 --vdev=net_tap0 >/tmp/iokernel_restore.log 2>&1 &)
    sleep 5
    ip -o addr show $IF
    sudo devlink dev eswitch show pci/$PF
}
trap restore EXIT

say "stopping the current iokernel"
sudo pkill -f iokerneld; sleep 2

say "setting up SR-IOV VFs on $IF"
sudo "$CALADAN/scripts/setup_vfs.sh" $IF || { echo "setup_vfs failed"; exit 1; }
VFPCI=$(basename "$(readlink /sys/class/net/$IF/device/virtfn0)")
echo "vfio VF is $VFPCI"

say "starting the iokernel with vfio directpath on $VFPCI"
(cd "$CALADAN" && sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 \
    vfio nicpci "$VFPCI" -- --allow "$VFPCI" >/tmp/iokernel_dp.log 2>&1 &)
sleep 8
grep -iE "directpath|error|panic" /tmp/iokernel_dp.log | head -20

say "workloads"
cd "$BUILD"
run() { # name, args...
    local n=$1; shift
    echo "--- $n"
    timeout 180 ./junction_run caladan_directpath.config \
        --trace_libos_mem "$OUT/dp_$n.txt" -- "$@" 2>&1 |
        grep -iE "DIRECTPATH|directpath|error|segfault|Unexpected" | head -6
    if [ -r "$OUT/dp_$n.txt" ]; then
        printf '    events=%s  libos-range=%s  caladan=%s  junction-libc=%s\n' \
            "$(grep -vc '^#' "$OUT/dp_$n.txt")" \
            "$(awk '!/^#/&&$4=="libos-range"' "$OUT/dp_$n.txt" | wc -l)" \
            "$(grep -c ' caladan ' "$OUT/dp_$n.txt")" \
            "$(grep -c junction-libc "$OUT/dp_$n.txt")"
        echo "    directpath events:"
        awk '!/^#/ && $2 ~ /directpath/ {print "      " $2, $3, $4, $5, $6, $7, $8}' \
            "$OUT/dp_$n.txt"
    fi
}
S=$(dirname "$OUT")/scratch; mkdir -p "$S"
run sleeper   "$ROOT/../$IF" 2>/dev/null || true
run forkstress "$ROOT/bench/fork/fork_stress" -t 4 -s 2 -f 32
run memfsrace  "$ROOT/docs/traces/memfs_race_bin"

say "the question that needed measuring: UAR MMIO across a clone"
echo "look for 'directpath-uar-mmio' above, then check whether fork_stress"
echo "still passes and whether the doorbell page is present in a child's"
echo "/proc/thread-self/maps (VM_IO|VM_PFNMAP inheritance)."
