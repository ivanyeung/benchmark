# RUN_BOOK — Page-Cache Fairness Benchmark (3-phase eviction)

How to run the benchmark on **Docker Desktop** and on a **real Linux host**, plus the
diffs that turned the harness into the 3-phase eviction test.

The suite is **3 tests**: `isolated_client1`, `isolated_client2`, and a `shared` dual test
that runs the 3-phase eviction choreography (cold-load → crank/evict → evicted-to-disk).
See `benchmark-c-summary.md` for the full design and parameter reference.

> ⚠️ **Docker Desktop host-cache masking.** On Docker Desktop (or any bind-mount / virtiofs
> path) the file cache lives in the **host VM outside the container cgroup**. The cgroup
> eviction is real and observable — you can watch client2 fill the 2 GB pool and client1's
> `memory.current` collapse (e.g. 1047 MiB → 21 MiB) with `workingset_refault_file` ticking
> up — **but client1's reads are still served from the host cache, so its p99/p999 stays at
> cache speed (~µs).** The *eviction* is visible; the *latency spike is masked*. To see
> client1's latency actually blow up in the evicted phase, run on a **real Linux host with a
> real local block device** (files on ext4/xfs over NVMe/SSD, not tmpfs/NFS/overlay).

---

## 1. Diffs — what changed

### `benchmark.c` (surgical additions)

Two new per-phase fio knobs (`io_size`, `rate_bw`) and a `--drop-once` flag:

```diff
 typedef struct {
     ...
-    int   runtime;                    /* seconds                             */
+    int   runtime;                    /* seconds (time-based, or ceiling)    */
     int   rwmixread;
+    /* read a fixed amount then stop (not time_based); "" = unset */
+    char  io_size[MAX_STR];           /* e.g. 1G -> fio --io_size            */
+    char  rate_bw[MAX_STR];           /* bandwidth cap e.g. 100m -> --rate   */
 } PhaseConfig;

 static struct {
     ...
+    bool        drop_once;   /* drop caches only before phase 0, not each phase */
 } opt = { ...
+    .drop_once          = false,
 };

 // apply_phase_key():
+    else if (!strcmp(key, "io_size"))  snprintf(p->io_size, MAX_STR, "%s", val);
+    else if (!strcmp(key, "rate_bw"))  snprintf(p->rate_bw, MAX_STR, "%s", val);

 // build_fio_cmd():
-    append(cmd, cap, " --runtime=%d --time_based", p->runtime);
+    if (p->io_size[0]) {
+        append(cmd, cap, " --io_size=%s", p->io_size);          /* read once then stop */
+        if (p->runtime > 0) append(cmd, cap, " --runtime=%d", p->runtime);  /* ceiling */
+    } else {
+        append(cmd, cap, " --runtime=%d --time_based", p->runtime);
+    }
+    if (p->rate_bw[0]) append(cmd, cap, " --rate=%s", p->rate_bw);

 // run_clients() phase loop:
-        if (cached) drop_caches();
+        /* --drop-once: only phase 0 starts cold; later phases keep the cache */
+        if (cached && (!opt.drop_once || ph == 0)) drop_caches();

 // main() flag parsing:
+        else if (!strcmp(a, "--drop-once")) opt.drop_once = true;
```

### Config / script changes

| File | Change |
|---|---|
| `fairness_configs.ini` | Rewritten as the **shared 3-phase eviction** test: client1 reads its 1G once/phase (`io_size=1G`, `rate_bw=100m`, `read`/`64k`); client2 streams `1M` reads with `rate_bw` ramping `8m → 100m → unlimited`. |
| `fairness_isolated.ini` | Same client1 / client2 workloads run **alone** (baselines), 3 phases each. |
| `fairness_shared_combo{1,2,3}.ini` | **Deleted** (were the old bandwidth-ratio sweep). |
| `cgroup_shared.ini` / `cgroup_isolated.ini` | Unchanged layout (2 GB parent, sibling leaves, no child memory caps); commented. |
| `run_14row_rand_sweep.sh` | Now runs the **3 tests** with `--drop-once`; `cd` made portable (Docker `/app` **and** host). |
| `run_docker_suite.sh` | Docker driver: cgroup-init → `make` → run 3 tests → generate per-test `.md`. |
| `run_linux.sh` | **New** — the same driver for a bare Linux host (no Docker). |
| `benchmark-c-summary.md` | Updated for the 3-phase design + the Docker-masking caveat in its header. |

The harness itself needs **no** change to run on Linux — it is already Linux-native (cgroup v2,
PSI, `/proc/vmstat`, `drop_caches`); macOS is only the build-only fallback.

---

## 2. How to run on Docker Desktop

Good for validating the harness and watching the **eviction** (memory.current / refault).
Latency will be masked (see caveat).

```bash
# 1. Build the image (once)
docker build -t pcf-bench .

# 2. Run the whole suite (cgroup-init + build + 3 tests + per-test .md)
docker run --rm --privileged --cgroupns=private \
  -v "$(pwd)":/app -w /app pcf-bench \
  bash run_docker_suite.sh
```

- `--privileged --cgroupns=private` give the container a writable cgroup v2 tree.
- `-v "$(pwd)":/app` bind-mounts the repo, so results land on the host under `results/`.
- Runtime ≈ 7 min (client1 finishes each phase in ~10 s; only client2 runs the full 60 s).

Results appear in `results/{isolated_client1,isolated_client2,shared_dual}/result.md` and
`results/SUITE_RESULTS.md`.

---

## 3. How to run on a real Linux host (un-masked latency)

This is the path that shows client1's p99/p999 actually spike in the evicted phase.

**Prerequisites**
- cgroup v2 mounted at `/sys/fs/cgroup` (default on modern distros).
- `fio`, `sysstat` (`iostat`), a C compiler + `make` installed.
- **root** (for `drop_caches` and creating cgroups).
- Test files on a **real local block device** (ext4/xfs on NVMe/SSD) — **not** tmpfs, NFS,
  or an overlay/virtiofs mount. The harness auto-creates them on first run if missing.

**Run (systemd host — recommended):**
```bash
sudo systemd-run --scope -p Delegate=yes bash run_linux.sh
```
`systemd-run --scope -p Delegate=yes` hands the run a writable, delegated cgroup subtree so it
doesn't fight systemd's management of the root cgroup.

**Run (non-systemd, or already in a delegated cgroup):**
```bash
sudo bash run_linux.sh
```

`run_linux.sh` does: cgroup-init (move shell to a leaf, enable `+memory +io`) → `make` → run the
3 tests with `--drop-once` → generate per-test `.md`.

**Manual equivalent** (if you prefer to run steps yourself):
```bash
# one-time cgroup init (per shell)
sudo mkdir -p /sys/fs/cgroup/init
echo $$ | sudo tee /sys/fs/cgroup/init/cgroup.procs
echo "+memory +io" | sudo tee /sys/fs/cgroup/cgroup.subtree_control

make
sudo ./benchmark -v --drop-once -c fairness_isolated.ini --cgroup-config cgroup_isolated.ini -m cached -o results/isolated_client1 client1_steady
sudo ./benchmark -v --drop-once -c fairness_isolated.ini --cgroup-config cgroup_isolated.ini -m cached -o results/isolated_client2 client2_noisy
sudo ./benchmark -v --drop-once -c fairness_configs.ini   --cgroup-config cgroup_shared.ini   -m cached -o results/shared_dual dual
python3 benchmark_analysis.py results/shared_dual > results/shared_dual/result.md
```

*(Optional)* for `io.weight` to arbitrate I/O, use a `bfq`/io-cost scheduler on the device:
`echo bfq | sudo tee /sys/block/<dev>/queue/scheduler`.

---

## 4. Reading the results (shared 3-phase test)

Per phase, in `results/shared_dual/result.md`:

| Signal | Where | Expected across phases 0 → 1 → 2 |
|---|---|---|
| client1 p99 / p999 | `READ LATENCY` section | low → low → **high** *(real Linux; flat on Docker)* |
| client1 refault | `WORKING SET REFAULTS` | 0 → ~0 → **high** |
| client1 `memory.current` | `MEMORY CONSUMPTION` | ~1 GB → **collapses** → low |
| client2 `memory.current` | `MEMORY CONSUMPTION` | ~0.5 GB → **~2 GB (cap)** → ~2 GB |

The eviction is proven by client1's `memory.current` collapsing while client2 fills the 2 GB
pool, and client1's refault rising in the final phase. On a real Linux host the p99/p999
follows the refault; on Docker Desktop only the eviction/refault is visible.
