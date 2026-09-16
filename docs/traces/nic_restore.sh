#!/bin/bash
# Restores the ConnectX-7 (0000:9c:00.0 / 400gp1) to the state captured before
# directpath testing, and restarts the iokernel as it was.
#
#   legacy eswitch, no VFs, mlx5_core, 400gp1 up with 10.40.1.105/16 mtu 9000
set -x
CALADAN=/home/bcwh/git/forking_junction/junction/lib/caladan

sudo pkill -f iokerneld; sleep 2

# drop tc rules added by setup_vfs.sh
for d in $(ls /sys/class/net/); do sudo tc qdisc del dev $d ingress 2>/dev/null; done

# rebind any VFs back to mlx5_core, then remove them
for v in /sys/bus/pci/devices/0000:9c:00.0/virtfn*; do
  [ -e "$v" ] || continue
  sudo $CALADAN/dpdk/usertools/dpdk-devbind.py -b mlx5_core $(basename $(readlink $v)) 2>/dev/null
done
echo 0 | sudo tee /sys/bus/pci/devices/0000:9c:00.0/sriov_numvfs >/dev/null
sleep 1

# back to legacy eswitch
sudo devlink dev eswitch set pci/0000:9c:00.0 mode legacy

sudo /etc/init.d/openibd restart

# restore addressing
sudo ip link set 400gp1 mtu 9000
sudo ip addr add 10.40.1.105/16 broadcast 10.40.255.255 dev 400gp1 2>/dev/null
sudo ip link set up 400gp1

# restart the iokernel exactly as it was
cd $CALADAN && sudo ./iokerneld ias nobw noht no_hw_qdel numanode -1 -- \
    --allow 00:00.0 --vdev=net_tap0 >/tmp/iokernel_restore.log 2>&1 &
sleep 5
set +x
echo "=== restored state ==="
ip -o addr show 400gp1
sudo devlink dev eswitch show pci/0000:9c:00.0
pgrep -af iokerneld | grep -v sudo | head -2
