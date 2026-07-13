#!/bin/bash
# run_14row_rand_sweep.sh — page-cache fairness benchmark (3-phase eviction test).
#
# 3 tests, all with --drop-once (cache dropped only before phase 0, then persists
# across phases so client2's reads accumulate and evict client1):
#
#   ISOLATED client1  — victim alone (baseline; nothing evicts it).
#   ISOLATED client2  — aggressor alone (its own streaming throughput).
#   SHARED dual        — the 3-phase eviction choreography:
#       phase 0 cold-load, phase 1 cache-hit, phase 2 evicted-to-disk.
#
# See fairness_configs.ini for the full per-phase story. cgroup: sibling leaves
# under a 2G-capped parent (cgroup_shared.ini / cgroup_isolated.ini).
set -e
cd "$(dirname "$(readlink -f "$0")")"   # repo dir (works in Docker /app and on a host)

SHARED_CFG=fairness_configs.ini
ISO_CFG=fairness_isolated.ini
SHARED_CGROUP=cgroup_shared.ini
ISO_CGROUP=cgroup_isolated.ini

run() {
  local desc="$1"; local cfg="$2"; local cgroup="$3"; local outdir="$4"; local target="$5"
  echo "=== $desc -> $outdir ==="
  rm -rf "$outdir"
  ./benchmark -v --drop-once -c "$cfg" --cgroup-config "$cgroup" -m cached -o "$outdir" $target
}

# ---- ISOLATED baselines (each client alone) --------------------------------
run "isolated client1 (victim baseline)"    "$ISO_CFG" "$ISO_CGROUP" results/isolated_client1 client1_steady
run "isolated client2 (aggressor baseline)" "$ISO_CFG" "$ISO_CGROUP" results/isolated_client2 client2_noisy

# ---- SHARED 3-phase eviction test ------------------------------------------
run "shared dual (3-phase eviction)"        "$SHARED_CFG" "$SHARED_CGROUP" results/shared_dual dual

echo "ALL TESTS COMPLETE (3: 2 isolated + 1 shared 3-phase)"
