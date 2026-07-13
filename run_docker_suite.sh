#!/bin/bash
# run_docker_suite.sh — driver executed INSIDE the pcf-bench container.
# Does the one-time cgroup v2 init, builds the Linux binary, runs all 6
# benchmark tests, and writes one result .md per test (+ a combined index).
set -e
cd /app

echo "### cgroup v2 init"
mkdir -p /sys/fs/cgroup/init
echo $$ > /sys/fs/cgroup/init/cgroup.procs
echo "+memory +io" > /sys/fs/cgroup/cgroup.subtree_control
echo "subtree_control: $(cat /sys/fs/cgroup/cgroup.subtree_control)"

echo "### build"
make

echo "### run all 6 tests"
bash run_14row_rand_sweep.sh

echo "### generate per-test result .md files"
TESTS=(isolated_client1 isolated_client2 shared_dual)
INDEX=results/SUITE_RESULTS.md
{
  echo "# Page-Cache Fairness — Suite Results"
  echo
  echo "Generated $(date -u +'%Y-%m-%dT%H:%M:%SZ') inside the pcf-bench container."
  echo "One detailed report per test is in each test's directory as \`result.md\`."
  echo
  echo "| Test | Report |"
  echo "|---|---|"
} > "$INDEX"
for d in "${TESTS[@]}"; do
  dir="results/$d"
  if [ -d "$dir" ]; then
    echo "  -> $dir/result.md"
    python3 benchmark_analysis.py "$dir" > "$dir/result.md"
    echo "| $d | [$dir/result.md]($d/result.md) |" >> "$INDEX"
  else
    echo "  !! missing $dir"
    echo "| $d | (missing) |" >> "$INDEX"
  fi
done

echo "### done — index: $INDEX"
echo "SUITE_DONE"
