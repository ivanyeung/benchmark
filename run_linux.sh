#!/bin/bash
# run_linux.sh — run the 3-phase eviction suite directly on a REAL Linux host
# (no Docker). This is the path that produces the UN-masked latency results,
# because the page cache is charged to the cgroup and evicted reads hit disk.
#
# Requirements:
#   * cgroup v2 mounted at /sys/fs/cgroup (default on modern distros)
#   * fio + sysstat installed; a C compiler + make
#   * ROOT (drop_caches + creating cgroups need it)
#   * test files (test_file_client1_steady_1G, test_file_client2_noisy_8G) on a
#     REAL local block device (ext4/xfs on NVMe/SSD) — NOT tmpfs / NFS / overlay,
#     or the eviction won't force real disk reads and the latency stays masked.
#
# Launch (systemd hosts — gets a writable delegated cgroup, avoids fighting systemd):
#   sudo systemd-run --scope -p Delegate=yes bash run_linux.sh
# Launch (non-systemd / already delegated):
#   sudo bash run_linux.sh
set -e
cd "$(dirname "$(readlink -f "$0")")"

echo "### cgroup v2 init"
mkdir -p /sys/fs/cgroup/init
# Move this shell into a leaf so the root cgroup has no internal processes,
# which is required before delegating controllers to children.
echo $$ > /sys/fs/cgroup/init/cgroup.procs 2>/dev/null || true
echo "+memory +io" > /sys/fs/cgroup/cgroup.subtree_control
echo "subtree_control: $(cat /sys/fs/cgroup/cgroup.subtree_control)"

echo "### build"
make

echo "### run the 3 tests (isolated client1, isolated client2, shared 3-phase)"
bash run_14row_rand_sweep.sh

echo "### generate per-test result .md files"
TESTS=(isolated_client1 isolated_client2 shared_dual)
INDEX=results/SUITE_RESULTS.md
{
  echo "# Page-Cache Fairness — Suite Results (real Linux host)"
  echo
  echo "Generated $(date -u +'%Y-%m-%dT%H:%M:%SZ')."
  echo
  echo "| Test | Report |"
  echo "|---|---|"
} > "$INDEX"
for d in "${TESTS[@]}"; do
  if [ -d "results/$d" ]; then
    echo "  -> results/$d/result.md"
    python3 benchmark_analysis.py "results/$d" > "results/$d/result.md"
    echo "| $d | [$d/result.md]($d/result.md) |" >> "$INDEX"
  fi
done

echo "### done — index: $INDEX"
echo "SUITE_DONE"
