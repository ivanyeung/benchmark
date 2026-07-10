# How to Run the Benchmark in Docker (and Save Results)

Step-by-step walkthrough for building the image, running an experiment inside
the container, and getting the results saved somewhere durable. See
[docker-setup.md](docker-setup.md) for the deeper explanation of *why* each
cgroup step is needed; this doc is the condensed run-book.

## 0. Prerequisites

- Docker Desktop installed and running: `docker info` should succeed.
- On macOS, Docker Desktop's Linux VM gives genuine cgroup v2, so no special
  workarounds are needed beyond what's below.

## 1. Build the image

From the repo root (where `Dockerfile` lives):

```bash
docker build -t pcf-bench .
```

## 2. Start the container with cgroup access and the repo mounted

```bash
docker run -it --rm \
  --privileged \
  --cgroupns=private \
  -v "$(pwd)":/app \
  -w /app \
  pcf-bench bash
```

- `-v "$(pwd)":/app` bind-mounts the repo into the container. This is the key
  step for saving results: anything the benchmark writes under `/app` inside
  the container (e.g. `benchmark_results/`, `results/`) lands directly on
  your Mac's filesystem at the same path — no copy-out step needed, and
  results survive after `--rm` deletes the container.
- `--privileged` + `--cgroupns=private` give the container its own writable
  cgroup v2 tree (needed for `memory.max` / `memory.low` / `io.weight`).

## 3. One-time cgroup setup (per container session)

Cgroup v2's "no internal processes" rule means the shell (PID 1's child)
sitting in the root cgroup blocks delegating controllers to children. Move
the shell into its own leaf cgroup first:

```bash
mkdir -p /sys/fs/cgroup/init
echo $$ > /sys/fs/cgroup/init/cgroup.procs
echo "+memory +io" > /sys/fs/cgroup/cgroup.subtree_control
```

Verify it took:

```bash
cat /sys/fs/cgroup/cgroup.subtree_control
# expect: io memory
```

Do this once per new container shell, before running `./benchmark`.

## 4. Build the benchmark binary

```bash
make
```

(Rebuild here even if a `benchmark` binary already exists on the host — it
must be compiled for Linux inside the container, not macOS.)

## 5. Run the experiment

Pick the mode/workload you want. Some common examples:

```bash
# Primary dual-client interference experiment (cached mode, isolated cgroups)
./benchmark -v --cgroup-config cgroup_isolated.ini -m cached dual

# Shared page-cache pool, no per-tenant isolation
./benchmark --no-cgroup -m cached dual

# A single client baseline (Case 1)
./benchmark --cgroup-config cgroup_isolated.ini -m cached client1_steady

# Every workload in the config
./benchmark all
```

By default results go to `benchmark_results/` in the current directory
(`/app`, which is the mounted repo). Use `-o <dir>` to send a run to its own
named directory instead — do this any time you want to keep multiple runs
side by side rather than overwriting the default one:

```bash
./benchmark -v --cgroup-config cgroup_isolated.ini -m cached -o results/run_$(date +%Y%m%d_%H%M%S) dual
```

## 6. Save / archive the results

Because step 2's bind mount makes `/app` inside the container the same
directory as the repo on your Mac, **the raw results are already saved on
the host as soon as the run finishes** — you'll see them appear under
`benchmark_results/` (or whatever `-o` directory you chose) in Finder/`ls` on
the Mac, even after the container exits.

To keep runs from clobbering each other and to produce a readable summary:

1. **Always use `-o` with a descriptive, timestamped name** for anything you
   want to keep (see command above) — the default `benchmark_results/` is
   convenient for one-off tests but gets overwritten by the next default run.

2. **Generate and save the analysis report** — `benchmark_analysis.py` prints
   to stdout, so redirect it into the results directory:

   ```bash
   python3 benchmark_analysis.py results/run_20260709_143000 \
     > results/run_20260709_143000/analysis_report.md
   ```

3. **(Optional) tar up a run for sharing/backup**, from inside or outside the
   container (both see the same files via the bind mount):

   ```bash
   tar -czf results/run_20260709_143000.tar.gz -C results run_20260709_143000
   ```

4. **Verify what a completed run directory contains** before trusting it:

   ```bash
   ls results/run_20260709_143000
   # *_p<N>.json   fio raw output per client per phase (has clat_ns.percentile.99)
   # summary.txt    one-line run summary
   # iostat/        device read/write latency & queue depth over time
   # psi/           per-cgroup memory/io pressure time series (CSV)
   # memstat/       per-cgroup memory.stat before/after each phase (refault deltas)
   # dirty/         vmstat + per-cgroup dirty-page time series
   ```

## 7. Sanity-check cgroups actually applied (optional, mid-run)

In a second terminal, exec into the running container:

```bash
docker exec -it <container_id_or_name> bash
cat /sys/fs/cgroup/client1_steady/cgroup.procs   # should list fio PIDs, not empty
cat /sys/fs/cgroup/client1_steady/memory.low     # should match cgroup_isolated.ini
```

## 8. Worked example: the 14-row experiment sweep

This is the exact sequence that produced the 14 `results/rowN_*` directories
and the numbers in `Project Cache Fairness Notes.txt`. It's a concrete
progression through baselines → isolated cgroups → shared cgroup → shared
cgroup with one tenant capped, sweeping `client2_noisy`'s pattern
(`read` sequential / `randread` / `randwrite`) at each stage. Run steps 1–4
above first (image built, container up, cgroup init done, `make` run).

Two files get hand-edited between rows — there's no separate config per row,
the same `fairness_configs.ini` / `cgroup_shared.ini` are reused and mutated
in place, then the run is immediately saved to its own `-o` directory before
the next edit:

- `[client2_noisy] phase_0_pattern` in `fairness_configs.ini` — cycles through
  `read` (sequential) → `randread` → `randwrite`.
- `memory.max` under `[client1_steady]` / `[client2_noisy]` in
  `cgroup_shared.ini` — added for rows 11–14, removed again afterward (the
  file in the repo today has neither cap — that's the post-sweep state).

| Row | Directory | Condition | `client2_noisy` pattern | cgroup config |
|---|---|---|---|---|
| 1 | `row1_client1_seq` | Client1 alone (baseline) | n/a | `cgroup_isolated.ini` |
| 2 | `row2_client2_randread` | Client2 alone (baseline) | randread | `cgroup_isolated.ini` |
| 3 | `row3_client2_seq` | Client2 alone (baseline) | read | `cgroup_isolated.ini` |
| 4 | `row4_client2_randwrite` | Client2 alone (baseline) | randwrite | `cgroup_isolated.ini` |
| 5 | `row5_isolated_seq` | Dual, isolated cgroups | read | `cgroup_isolated.ini` |
| 6 | `row6_isolated_randread` | Dual, isolated cgroups | randread | `cgroup_isolated.ini` |
| 7 | `row7_isolated_randwrite` | Dual, isolated cgroups | randwrite | `cgroup_isolated.ini` |
| 8 | `row8_shared_seq` | Dual, shared cgroup, no extra cap | read | `cgroup_shared.ini` |
| 9 | `row9_shared_randread` | Dual, shared cgroup, no extra cap | randread | `cgroup_shared.ini` |
| 10 | `row10_shared_randwrite` | Dual, shared cgroup, no extra cap | randwrite | `cgroup_shared.ini` |
| 11 | `row11_c1cap_randread` | Dual, shared cgroup, **client1** `memory.max=1G` | randread | `cgroup_shared.ini` (edited) |
| 12 | `row12_c1cap_randwrite` | Dual, shared cgroup, **client1** `memory.max=1G` | randwrite | `cgroup_shared.ini` (edited) |
| 13 | `row13_c2limited_randread` | Dual, shared cgroup, **client2** `memory.max=1G` | randread | `cgroup_shared.ini` (edited) |
| 14 | `row14_c2limited_randwrite` | Dual, shared cgroup, **client2** `memory.max=1G` | randwrite | `cgroup_shared.ini` (edited) |

Commands, in order:

```bash
# Rows 1–4: baselines, single client, isolated cgroup, no interference
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row1_client1_seq client1_steady

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randread
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row2_client2_randread client2_noisy

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = read
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row3_client2_seq client2_noisy

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randwrite
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row4_client2_randwrite client2_noisy

# Rows 5–7: dual mode, isolated cgroups (client1_steady gets memory.low=1G floor)
# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = read
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row5_isolated_seq dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randread
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row6_isolated_randread dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randwrite
./benchmark --cgroup-config cgroup_isolated.ini -m cached -o results/row7_isolated_randwrite dual

# Rows 8–10: dual mode, shared 2G cgroup pool, no per-tenant cap
# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = read
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row8_shared_seq dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randread
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row9_shared_randread dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randwrite
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row10_shared_randwrite dual

# Rows 11–12: shared cgroup, cap the VICTIM
# edit cgroup_shared.ini: add "memory.max = 1G" under [client1_steady]
# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randread
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row11_c1cap_randread dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randwrite
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row12_c1cap_randwrite dual

# Rows 13–14: shared cgroup, cap the AGGRESSOR instead
# edit cgroup_shared.ini: remove client1_steady's memory.max, add
# "memory.max = 1G" under [client2_noisy]
# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randread
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row13_c2limited_randread dual

# edit fairness_configs.ini: [client2_noisy] phase_0_pattern = randwrite
./benchmark --cgroup-config cgroup_shared.ini -m cached -o results/row14_c2limited_randwrite dual

# afterward: remove the memory.max cap from cgroup_shared.ini again,
# restore fairness_configs.ini pattern, to return to the repo's default state
```

After the sweep, roll the 14 directories into one comparison table (this is
what `Project Cache Fairness Notes.txt` captures — p99, refault delta,
writeback peak, memory PSI, io PSI per row):

```bash
for d in results/row*; do
  echo "== $d =="
  python3 benchmark_analysis.py "$d"
done > results/sweep_summary.md
```

## Known limitations in Docker Desktop

- `io.weight` files don't appear under `/sys/fs/cgroup/<tenant>/` — Docker
  Desktop's virtualized disk (overlay2 over virtiofs) doesn't use an
  `io`-weight-capable scheduler (e.g. `bfq`). `memory.max`/`memory.low`
  experiments work fine; `io.weight`-based ones don't. Use a real Linux
  host/VM with a real block device for those.
- Docker Desktop's storage path adds I/O latency/variance a bare-metal Linux
  box wouldn't have — fine for validating the harness, not for numbers you'd
  trust as final fairness measurements.