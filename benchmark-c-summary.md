# `benchmark.c` — Method-by-Method Summary

`benchmark.c` is the C harness for a **page-cache fairness benchmark**. It pairs a
latency-sensitive *victim* (Tenant A / `client1_steady`) against a *noisy neighbor*
(Tenant B / `client2_noisy`) under **cgroup v2**, drives both with `fio`, and measures
Tenant A's read-latency spike while decomposing it into two mechanisms:

- **Mechanism 1** — LRU eviction / refaults from a clean scan (B reads a large file and
  evicts A's cached hot set).
- **Mechanism 2** — eviction *plus* dirty-writeback contention (B is a buffered writer,
  adding writeback pressure on top of eviction).

Tenant B is **configurable** — the mechanism under test is chosen entirely by B's fio
parameters in `fairness_configs.ini`, not by the C code. The workload shipped on this
branch runs a **3-phase eviction choreography** with `--drop-once` (cache dropped only
before phase 0, then persists across phases):

- **Phase 0 (cold):** A loads its whole 1G file into cache (`io_size=1G` @ 100 MiB/s ≈ 10s)
  then idles; B reads only ~480 MB. Pool holds ~1.5 GB < 2 GB → no eviction.
- **Phase 1 (crank):** A rereads its still-cached 1G (fast); B ramps to 100 MiB/s and streams
  ~6 GB, **filling the 2 GB pool and evicting A** by the end.
- **Phase 2 (evicted):** B bursts unlimited; A's pages are gone, so A's reread refaults/reads
  from disk. A's p99/p999 and refault are recorded each phase.

The knobs that make this possible are `io_size` (read a fixed amount once then stop),
`rate_bw` (bandwidth cap — *bandwidth*, not IOPS, is what streams pages through the pool and
drives eviction), and the `--drop-once` flag.

The harness sets up cgroups, spawns fio per client/phase, samples kernel pressure/dirty-page
telemetry in the background, and snapshots per-cgroup `memory.stat` before/after every phase.
It is Linux-only for the measurement paths (cgroups, PSI, `memory.stat`, `/proc/vmstat`); on
macOS it builds and runs but the Linux-specific paths no-op.

> ⚠️ **Docker Desktop host-cache masking (read before trusting latency numbers).** On Docker
> Desktop (and any setup where the test files sit on a bind mount / virtiofs), the file's page
> cache lives in the **host VM outside the container cgroup**. The cgroup memory cap and
> eviction still work — you can watch B fill the 2 GB pool and A's `memory.current` collapse
> (e.g. 1047 MiB → 21 MiB) and `workingset_refault_file` tick up — **but A's reads are still
> served fast from the host cache**, so A's p99/p999 stays at cache speed (~µs) and only a tiny
> fraction of pages truly refault. In other words: on Docker Desktop the *eviction* is
> observable but the *latency spike is masked*. To see A's p99/p999 actually blow up in the
> evicted phase, run on a **real Linux host with a real block device** (files on the
> cgroup-accounted device, not a virtiofs bind mount). Also note `io.weight` control files
> don't appear under Docker Desktop's virtualized disk — `memory.max` works, `io.weight` doesn't.

---

## How the pieces fit together

```
                         fairness_configs.ini            cgroup_*.ini
                       (workloads: A + B phases)     (cgroup layout + limits)
                                 │                            │
                        parse_config()              parse_cgroup_config()
                                 │                            │
                              Config                    CgroupSet
                                 └──────────┬───────────────┘
                                            ▼
                                    main() dispatch
                              (single / dual / all)
                                            ▼
                                     run_for_modes()      ── cached and/or direct
                                            ▼
                                      run_clients()
                          ┌────────────────┼─────────────────┐
                    start_sampler()   spawn_client_phase()   start_iostat()
                    (PSI, vmstat,     (fio per phase, in     (iostat -dx 1)
                     file_dirty)       its cgroup)
                                            ▼
                                    record_memstat()
                              (before/after refault + memory.current)
                                            ▼
                                  benchmark_results/
                     (JSON per phase, memstat/, psi/, dirty/, iostat/, summary.txt)
```

The two config files play distinct roles:

| File | Parsed by | Produces | Governs |
|------|-----------|----------|---------|
| `fairness_configs.ini` | `parse_config` | `Config` → `ClientConfig[]` → `PhaseConfig[]` | **What** each tenant does: fio pattern, block size, IOPS/bandwidth rate (`rate_iops`/`rate_bw`), `io_size`, iodepth, runtime, number of phases |
| `cgroup_isolated.ini` / `cgroup_shared.ini` | `parse_cgroup_config` | `CgroupSet` → `CgroupConfig[]` | **Where/under-what-limits** each tenant runs: cgroup path, `memory.max`, `memory.low`, `io.weight` |

---

## Configuration model (data structures)

- **`PhaseConfig`** — one fio invocation's parameters: `pattern` (randread/read/randwrite/write/randrw),
  `block_size`, `ioengine`, `rate_iops` (0 = unlimited), `iodepth`, `numjobs`, `runtime`, `rwmixread`
  (for `randrw`; −1 = unset), `io_size` (read exactly this much once then stop — not time-based; "" =
  unset), `rate_bw` (bandwidth cap e.g. `100m` → fio `--rate`; "" = unset), `random_distribution`
  (e.g. `zipf:1.2` to skew the hot set), and flush cadence `fdatasync` / `fsync` (issue a flush every
  N write ops). `present` records whether the phase was actually defined.
- **`ClientConfig`** — one tenant: `name`, `description`, `file_size`, and up to `MAX_PHASES` (16)
  `PhaseConfig`s. `num_phases` tracks how many were defined.
- **`CgroupConfig`** — one cgroup leaf: `section` (ini section name), `cgroup_name` (path relative to
  `/sys/fs/cgroup`, defaults to the section name), and optional `memory_max` / `memory_low` / `io_weight`.
- **`Config` / `CgroupSet`** — arrays of the above plus counts.
- **`opt`** — global run options (config paths, output dir, cache `mode`, verbose, `use_cgroups`, `use_psi`,
  `drop_once`), seeded with defaults and overridden by CLI flags.
- **`CacheMode`** — `MODE_BOTH` / `MODE_CACHED` / `MODE_DIRECT`, selecting buffered vs `direct=1` fio runs.

---

## Configuration parameter reference

This section documents **every parameter** in the two ini files: what it does (the fio /
cgroup knob it maps to), **what it is attempting to measure** (why it exists in a
*fairness* benchmark), and **how to measure / observe its effect** (which output file to
read). The current values shipped for the `dual` experiment are given inline.

### `fairness_configs.ini` — workload parameters

Each `[section]` is one tenant. Keys are either **section-level** (apply to the whole
client) or **phased** (`phase_N_<key>`, apply to phase `N`). A bare key with no `phase_`
prefix is shorthand for `phase_0`. Defaults (`init_phase`): `pattern=randread`, `bs=4k`,
`ioengine=libaio`, `iodepth=1`, `numjobs=1`, `runtime=60`, `rwmixread=-1`.

#### Section-level keys

| Key | fio mapping | What it does | What it measures / why | How to measure its effect |
|-----|-------------|--------------|------------------------|---------------------------|
| `description` | (none) | Free-text label for the tenant. | Documents the section's role (victim vs neighbor). | N/A — appears only in this file. |
| `file_size` | `--size` + backing file `test_file_<name>_<size>` | Size of the file fio reads/writes. | Sets each tenant's **cache footprint**. A's is ~0.5× the 2G pool (fits when alone); B's is 4× (8G) so B alone overflows the pool and forces reclaim. This size ratio is what *creates* the eviction contention being studied. | Watch `memstat/<client>_<mode>.csv` `memory_current_bytes` grow toward the cap, and `workingset_refault_file_delta` rise once the combined footprint exceeds `memory.max`. |

#### Phased keys (`phase_N_<key>`)

| Key | fio flag | What it does | What it measures / why | How to measure its effect |
|-----|----------|--------------|------------------------|---------------------------|
| `pattern` | `--rw` | Access pattern: `randread` / `read` / `randwrite` / `write` / `randrw`. | Selects behaviour. In the 3-phase design **A uses `read`** (sequential, to load/reread its whole 1G file once) and **B uses `read`** (sequential stream of its 8G file). `randread` gives cleaner per-op latency but can't cover a whole file from cold disk in the phase window. `randwrite`/`write` would add dirty-writeback pressure. | The pattern drives which telemetry moves: reads → `pgscan_kswapd`, `workingset_refault_file`; writes → `nr_dirty`/`file_dirty`/`file_writeback` and io PSI. |
| `block_size` | `--bs` | Size of each I/O. | Trades **IOPS vs bandwidth**. In the 3-phase design A uses `64k` (sequential load, decent latency granularity) and B uses `1M` (big blocks = high bandwidth per I/O → streams the most pages through the pool → drives eviction). | Compare `bs` × achieved IOPS = bandwidth in the fio JSON; bigger `bs` on B ⇒ more page churn ⇒ faster eviction of A. |
| `ioengine` | `--ioengine` | I/O submission engine. | `libaio` gives real async queue depth on Linux (needed for `iodepth` to matter). | N/A directly; enables `iodepth` to take effect. |
| `io_size` | `--io_size` (no `--time_based`) | Read exactly this many bytes **once, then stop** (e.g. `1G`). `""` = unset. | Models "**read the file once then suspend**" — A reads its whole 1G each phase then idles for the rest of the 60s while B keeps streaming. When set, `runtime` becomes a *ceiling*, not a fixed window. | A finishes its read early (JSON `total_ios` ≈ file/`bs`); the phase length is then set by B (time-based). |
| `rate_bw` | `--rate` (bandwidth) | Caps **bandwidth** e.g. `100m` = 100 MiB/s, `8m` = 8 MiB/s. `""` = unlimited. | **The primary eviction knob** — bandwidth (not IOPS) is the rate at which distinct pages are streamed into the pool, i.e. the eviction rate. B ramps `8m → 100m → unlimited` across phases; A is paced at `100m` so its 1G load takes ~10s. | Achieved `bw` in the fio JSON; watch `memory_current_bytes` fill toward the 2G cap as B's `rate_bw` rises. |
| `rate_iops` | `--rate_iops` (per **job**) | Caps offered **IOPS** per job (`0`/unset = unlimited). | Alternative to `rate_bw` when you want to pin ops/s rather than bytes/s (e.g. an IOPS-ratio experiment). Not used by the 3-phase design (which is bandwidth-driven). Aggregate = `rate_iops × numjobs`. | Achieved `iops` in the fio JSON. |
| `iodepth` | `--iodepth` | Number of outstanding I/Os per job. | Controls **queueing**. A = `1` → one in-flight I/O, so its clat p99 is pure latency with no self-queueing (contention shows cleanly). B = `32` → deep queue to sustain streaming bandwidth. | A higher `iodepth` on A inflates its own clat (self-queueing) and muddies the SLO signal — keep it `1`. On B it raises device utilization (`%util`/`aqu-sz` in `iostat/run_<mode>.iostat`). |
| `numjobs` | `--numjobs` (+ `--group_reporting`) | Parallel fio jobs (threads) per phase. | Scales concurrency/offered load. Held at `1` in the 3-phase design. | Reported as one group in the fio JSON. |
| `runtime` | `--runtime` (+ `--time_based` when `io_size` unset) | Seconds per phase. With `io_size` set it is only a **ceiling**; otherwise it is a fixed time-based window. | B uses time-based `runtime=60` (streams the whole 60s); A uses it only as a safety ceiling on its `io_size` read. | Phase `before`/`after` timestamps in `memstat/*.csv` bracket this window. |
| `rwmixread` | `--rwmixread` (only when pattern contains `rw`) | Read percentage for `randrw`/`rw` (`-1` = unset). | Tunes the **read/write blend** for a mixed neighbor (both mechanisms at once). Ignored for pure read/write patterns. | Split of read vs write IOPS in the fio JSON; balances `refault` (reads) vs `file_dirty` (writes). |
| `random_distribution` | `--random_distribution` | Skews the random access hot set, e.g. `zipf:1.2`. | Makes a **concentrated hot set** so evicting it actually causes refaults (uniform random over a huge file rarely re-hits a page). Most relevant on A. | With skew, A's `workingset_refault_file_delta` jumps when B evicts the hot pages; without it, refaults stay diffuse. |
| `fdatasync` / `fsync` | `--fdatasync=N` / `--fsync=N` | Force a flush every `N` write ops. | Adds **checkpoint / WAL-style writeback bursts** for a Mechanism-2 writer B. `0` = unset. | Drives spikes in `file_writeback` / `nr_writeback` and io PSI in the `dirty/` and `psi/` CSVs. |

**This branch's values (3 tests, 3-phase eviction).** Every run uses the same cgroup shape —
`client1_steady` and `client2_noisy` are sibling leaves under a 2G-capped parent, with **no**
`memory.low` / `memory.min` / `memory.max` on the children — and **`--drop-once`** so the page
cache is dropped only before phase 0 and then persists across phases (that persistence is what
lets B's reads accumulate and evict A). `numjobs=1` throughout.

- **Shared benchmark — 1 test, 3 phases** (`fairness_configs.ini`, `dual`, `cgroup_shared.ini`).
  A and B run concurrently and compete for the one 2G pool. A (`read` / `64k` / `io_size=1G` /
  `rate_bw=100m`, `iodepth=1`) reads its whole 1G file once at the start of each phase (~10s)
  then idles; B (`read` / `1M` / `iodepth=32`) streams its 8G file for the full 60s at a
  **ramping bandwidth**:

  | Phase | B `rate_bw` | What happens | A's expected result |
  |---|---|---|---|
  | 0 (cold) | `8m` (~480 MB) | pool ~1.5 GB, no eviction | cold-load latency, refault 0 |
  | 1 (crank) | `100m` (~6 GB) | B fills the 2 GB pool, evicts A by end | cache-hit latency, refault ~0 |
  | 2 (burst) | unlimited | A's pages gone → reread from disk | disk latency (high), refault high |

  Observed (Docker Desktop) `memory.current` confirms the eviction really happens: A = 1047 → 21
  → 18 MiB, B = 499 → 2013 → 1990 MiB; A's phase-2 refault = 1946 pages. A's *latency* stayed
  flat here only because of the host-cache masking (see the header caveat).

- **Isolated benchmark — 2 tests** (`fairness_isolated.ini`, `client1_steady` / `client2_noisy`
  alone, `cgroup_isolated.ini`): each client runs the same 3-phase workload **alone**. With no
  neighbour nothing evicts A, so these are the "best case" baselines the shared run is compared
  against.

### The three tests — what each one does

| # | Test (config → command) | Who runs | What it does / why |
|---|---|---|---|
| 1 | isolated client1 (`fairness_isolated.ini`, `client1_steady`) | A alone | **Victim baseline** — A's per-phase p99/p999/refault with no aggressor (never evicted). |
| 2 | isolated client2 (`fairness_isolated.ini`, `client2_noisy`) | B alone | **Aggressor baseline** — B's own streaming throughput with the whole 2G pool to itself. |
| 3 | shared (`fairness_configs.ini`, `dual`) | A + B | **The eviction test** — the 3-phase choreography above; record A's p99/p999/refault as B ramps and evicts across phases. |

**Reading the result:** compare A's p99/p999 and `workingset_refault_file_delta` across the 3
phases of the shared test (and against the isolated baseline). The eviction shows up as A's
`memory_current_bytes` collapsing and its refault rising from phase 1 → 2. On a real Linux host
the latency follows the refault; on Docker Desktop only the eviction/refault is visible (header
caveat).

> **Why the cache must persist across phases (`--drop-once`).** The whole experiment depends on
> B's reads *accumulating* across phases until they overflow the 2G pool and evict A. The default
> behaviour drops the cache before **every** phase, which would reset that accumulation and make
> cross-phase eviction impossible — so this design is only meaningful with `--drop-once`
> (drop before phase 0 only).

> **Why bandwidth, not IOPS, drives the eviction (`rate_bw` + `io_size`).** Eviction rate ≈
> distinct pages streamed per second = **bandwidth ÷ 4 KiB**, so B is paced by `rate_bw`
> (`8m → 100m → unlimited`) at `bs=1M` for maximum bytes/s of churn. A uses `io_size=1G` to
> "read its file once then suspend" each phase, and `rate_bw=100m` so that load takes ~10s.
> A big-block, high-bandwidth B fills the pool; a small-block/high-IOPS B (e.g. 4k) moves few
> bytes and barely evicts — read the *achieved* `bw` from the fio JSON, not the configured cap.

> **`numjobs` vs `iodepth` — two kinds of concurrency, and why A uses `numjobs=1, iodepth=1`.**
> Both knobs add outstanding I/O, but they are *not* interchangeable for the victim, because A
> is a **latency** measurement. `iodepth` is concurrency *within one worker* (an async job keeps
> up to N I/Os pipelined on its own queue); `numjobs` is concurrency *across independent workers*
> (N separate fio processes, each with its own queue). The difference shows up in A's `clat`:
> - **1 job × iodepth=4** — the four requests share one worker's queue, so a slow/refaulted read
>   holds a slot and the next submission waits behind it. Each sample's clat then includes
>   **self-queueing** (time spent behind *A's own* siblings), so you can't separate "B evicted my
>   cache" from "I queued behind myself." Good for *saturating a device* (that's B's job), bad for
>   a clean SLO reading.
> - **1 job × iodepth=1** (A's setting) — exactly one in-flight I/O at any instant, so no request
>   ever waits behind another A request. Every clat sample is a **pure per-request latency**, so
>   A's p99 reflects only the contention under test (LRU eviction / refault, device + PSI pressure
>   from B). This is the cleanest possible SLO probe, which is why A is the constant "ruler."
>
> `numjobs=1` is held constant for both tenants. A keeps `iodepth=1` to protect its latency
> signal; B uses `iodepth=32` so its sequential stream stays deep enough to sustain the target
> bandwidth (`rate_bw`). If you ever need more victim load, scale it with `numjobs` while keeping
> `iodepth=1`; use `iodepth` on the neighbor, whose own latency is irrelevant.

### `cgroup_isolated.ini` / `cgroup_shared.ini` — cgroup layout parameters

Parsed by `parse_cgroup_config`; each `[section]` becomes one cgroup leaf. A section name
must match a workload section (that's how `cgroup_for_client` joins the two files). Passed
to the harness with `--cgroup-config`; omit it (or `--no-cgroup`) to run without limits.

| Key | cgroup v2 file | What it does | What it measures / why | How to measure its effect |
|-----|----------------|--------------|------------------------|---------------------------|
| `[section]` name | (directory match) | Names the cgroup and links it to the same-named workload client. | Establishes which tenant runs under which limits (`[clients]` here is the shared parent, not a client). | The join point — `cgpath_or_null` places each client's fio into this cgroup via `cgroup.procs`. |
| `cgroup_name` | path under `/sys/fs/cgroup` | Directory path for the leaf (defaults to the section name). Slashes create nesting, e.g. `clients/client1_steady`. | Defines the **hierarchy**: both tenants are leaves under a common parent so a single parent cap governs their shared pool. | `mkdir_p_cgroup` creates each level; per-cgroup telemetry filenames flatten the path (`clients_client1_steady`). |
| `memory.max` | `memory.max` | Hard memory ceiling (usage above it triggers reclaim, then OOM). | **The scarce resource being arbitrated.** In `cgroup_isolated.ini` it is set to `2G` on the **parent** `[clients]` only — so A and B share one 2G page-cache pool and either can evict ("steal") the other's cached pages. This is the *no-protection* baseline the experiment measures against. | `memory_current_bytes` in `memstat/*.csv` plateaus at the cap; `workingset_refault_file_delta` climbs once combined footprint hits it. |
| `memory.low` | `memory.low` | Best-effort **protected reserve**: memory up to this size is reclaimed last. | Tests whether standard cgroup v2 protection can **shield the victim's hot set** and recover its p99. **Not set** in the current isolated layout (that's the point — no protection); a protected variant would put `memory.low = <A's working set>` on `client1_steady`. | Compare A's p99 and `workingset_refault_file_delta` with vs without `memory.low` — protection should cut both. |
| `io.weight` | `io.weight` | Proportional **I/O bandwidth share** (1–10000) under contention. | Controls block-layer fairness. Both tenants set `100` (equal share) so the experiment isolates *memory/cache* fairness rather than confounding it with I/O weighting. | Per-device shares/latency in `iostat/run_<mode>.iostat`; skewing the weights would shift `iops`/`await` between tenants. |

> **Note on the file name.** Despite being called `cgroup_isolated.ini`, this layout gives
> each tenant its own *leaf* cgroup but **no per-tenant memory protection** — all
> arbitration happens at the shared 2G parent. It is the "isolated leaves, shared pool"
> baseline. `cgroup_shared.ini` is the comparison layout.

### How the parameters turn into a measurement (quick recipe)

1. Build: `make`. Run the sweep: `./benchmark --cgroup-config cgroup_isolated.ini -m cached dual`
   (cached mode is required for any writeback/Mechanism-2 variant; `drop_caches` runs each
   phase so refaults start from cold).
2. **x-axis (cause):** B's `rate_iops` per phase (from this ini) and its *achieved* iops/bw
   (`benchmark_results/client2_noisy_<mode>_p<phase>.json`).
3. **y-axis (effect):** A's clat p99/p999 per phase
   (`benchmark_results/client1_steady_<mode>_p<phase>.json` → `read.clat_ns.percentile`).
4. **Attribution:** correlate the spike with eviction (`memstat/*.csv`
   `workingset_refault_file_delta`, `dirty/vmstat_<mode>.csv` `pgscan_kswapd`, memory PSI in
   `psi/*.csv`) for Mechanism 1, or with writeback (`nr_dirty`/`file_dirty`/`file_writeback`,
   io PSI) for Mechanism 2.

---

## Method-by-method breakdown

### Small helpers

- **`trim(s)`** — Strips leading/trailing whitespace in place; returns pointer into `s`. Used throughout
  ini parsing so keys/values/section names are clean.
- **`is_linux()`** — Compile-time `__linux__` check. Gate for every Linux-only path (cgroups, PSI, vmstat,
  drop_caches, sampler, iostat). On non-Linux it makes those methods no-op so the tool still builds/runs.
- **`ensure_dir(path)`** — `mkdir -p` equivalent: walks the path creating each component (`0755`), ignoring
  `EEXIST`. Guards against overly long paths. Used to create the results dir and its subdirs.
- **`ensure_subdir(base, sub, out, n)`** — Builds `base/sub`, creates it, and returns the path in `out`.
  Convenience wrapper around `ensure_dir` for the `memstat/`, `psi/`, `dirty/`, `iostat/` subtrees.
- **`cgroup_file_label(cg, out, n)`** — Flattens a cgroup path for use in filenames
  (`clients/client1_steady` → `clients_client1_steady`) so telemetry CSVs get valid, unique names.

### INI parsing — workload config

- **`apply_phase_key(p, key, val)`** — Routes a single key/value onto the correct `PhaseConfig` field
  (string copy for text fields, `atoi` for numeric ones). Unknown keys are logged and ignored. Marks the
  phase `present`. This is the single point that maps ini keys → fio parameters.
- **`init_phase(p)`** — Zeroes a phase and applies defaults: `randread`, `4k`, `libaio`, `iodepth=1`,
  `numjobs=1`, `runtime=60`, `rwmixread=-1`. Ensures every phase is valid even if the ini specifies only
  a few keys.
- **`find_or_add_client(cfg, name)`** — Returns the existing `ClientConfig` for a section name or appends a
  new one (initializing all its phases). Enforces the `MAX_CLIENTS` cap. Lets one section be built up across
  many lines.
- **`parse_config(path, cfg)`** — The main workload-ini reader. Skips blanks/comments (`#`, `;`), tracks the
  current `[section]`, and for each `key = value`:
  - `description` / `file_size` → client-level fields.
  - `phase_N_key` → parses index `N` and the trailing key, routes to `phases[N]` via `apply_phase_key`, and
    grows `num_phases`.
  - a bare key → treated as shorthand for `phase_0`.
  It also strips inline `;` comments from values. Result: a fully populated `Config`.
- **`find_client(cfg, name)`** — Linear lookup of a client by name; `NULL` if absent. Used by dispatch and
  the run loop.

### INI parsing — cgroup config

- **`parse_cgroup_config(path, set)`** — Reads the separate cgroup-layout ini. Each `[section]` becomes a
  `CgroupConfig` whose `cgroup_name` defaults to the section name; keys `cgroup_name`, `memory.max`,
  `memory.low`, `io.weight` override the defaults. Produces the `CgroupSet` that drives cgroup creation and
  process placement.
- **`cgroup_for_client(set, client)`** — Matches a client name to its `CgroupConfig` by section name; `NULL`
  if none. This is the join between the two ini files — it links `[client1_steady]` in the workload ini to
  `[client1_steady]` in the cgroup ini.

### cgroup setup (Linux)

- **`write_cgroup_file(cg_path, file, val)`** — Writes `val` into a control file (e.g. `memory.max`) unless
  `val` is empty. Best-effort with warnings on failure.
- **`enable_controllers(cg_path)`** — Writes `+memory +io` to `cgroup.subtree_control` so children expose
  `memory.max` / `io.weight`. Best-effort (systemd often has already delegated these).
- **`mkdir_p_cgroup(rel)`** — Creates a cgroup path component-by-component under `/sys/fs/cgroup`, enabling
  controllers on the root and every non-leaf directory top-down (a controller can only be delegated by a
  parent that already has it). This is why nested paths like `clients/client1_steady` work.
- **`setup_cgroups(set)`** — For each `CgroupConfig`: makes the cgroup tree, then applies `memory.max`,
  `memory.low`, `io.weight`. No-ops off Linux. This turns the cgroup ini into live kernel limits — the
  arbitration layer the whole experiment measures.

### `memory.stat` snapshots + refault accounting

- **`read_memstat_field(cgroup_name, field)`** — Opens `<cgroup>/memory.stat` and returns the value of a
  named field (e.g. `workingset_refault_file`, `file_dirty`, `file_writeback`). −1 on failure/non-Linux.
- **`read_cgroup_scalar(cgroup_name, filename)`** — Reads a single-integer control file such as
  `memory.current`.
- **`record_memstat(...)`** — The core measurement hook. For a given phase and a `"before"`/`"after"` marker,
  it reads `workingset_refault_file` and `memory.current`, appends a row to
  `memstat/<client>_<mode>.csv` (writing a header on first create), and on `"after"` computes the deltas
  (`workingset_refault_file_delta`, `memory_current_bytes_delta`) against the before-values passed in. The
  refault delta is the direct signal that A's cached pages were evicted and had to be re-read — the heart of
  Mechanism 1.

### Background telemetry sampler

- **`sampler_sig` / `sampler_stop`** — Signal handler + flag so the sampler child exits cleanly on
  `SIGTERM`/`SIGINT`.
- **`read_vmstat_field(field)`** — Reads a system-wide counter from `/proc/vmstat` (`nr_dirty`,
  `nr_writeback`, `pgpgin`, `pgscan_kswapd`) — dirty/writeback pages, pages read from disk, and LRU scan
  activity (eviction pressure).
- **`read_psi_total(cgroup_name, resource)`** — Parses the `some ... total=N` line from a cgroup's
  `memory.pressure` or `io.pressure` PSI file, returning cumulative stall microseconds. This quantifies how
  long tasks were stalled on memory/IO — the pressure evidence for both mechanisms.
- **`run_sampler(mode, cgroup_names, n_cg)`** — Runs in a forked child. Every `SAMPLE_INTERVAL_S` (1s) it
  appends:
  - `dirty/vmstat_<mode>.csv` — `ts, nr_dirty, nr_writeback, pgpgin, pgscan_kswapd`
  - `dirty/<cg>_<mode>_dirty.csv` (per cgroup) — `ts, file_dirty, file_writeback`
  - `psi/<cg>_<mode>.csv` (per cgroup, if PSI enabled) — `ts, mem_some_total_us, io_some_total_us`
  It flushes each write so partial data survives a crash, and exits on signal. This is the time-series
  backbone that lets analysis correlate latency spikes with eviction/writeback activity.
- **`start_sampler(mode, ...)`** — Forks the sampler child (Linux only) and returns its pid.
- **`stop_sampler(pid)`** — Sends `SIGTERM` and reaps the child at phase-set end.

### `iostat` logger

- **`start_iostat(mode)`** — Forks `iostat -dx 1` redirected to `iostat/run_<mode>.iostat` for extended
  per-device latency/queue-depth stats each second. Also writes `iostat/run_<mode>.start_ts` (wall-clock
  start) so analysis can map iostat's 1-second reports back to real time and line them up with the phase
  before/after wall times in the memstat CSVs. Linux only.
- **`stop_iostat(pid)`** — Terminates and reaps the iostat process.

### fio command builder

- **`append(cmd, cap, fmt, ...)`** — Overflow-guarded `printf`-append onto the command buffer.
- **`build_fio_cmd(cmd, cap, c, p, test_file, json_out, cached)`** — Translates one `PhaseConfig` into a full
  `fio` command line: `--name`, `--filename`, `--size`, `--rw` (pattern), `--bs`, `--ioengine`, `--iodepth`,
  `--numjobs`, `--runtime --time_based`, and `--direct=0` (cached/buffered) or `--direct=1` (bypass cache).
  Conditionally adds `--rate_iops`, `--rwmixread` (only for `rw` patterns), `--random_distribution` (hot-set
  skew), and `--fdatasync` / `--fsync` (flush cadence for writer/WAL variants). Emits
  `--output-format=json+ --output=<json_out>` so fio reports `clat_ns` percentiles including p99/p999 — the
  victim's SLO signal. This method is where the workload ini becomes an actual benchmark run.

### Test-file management

- **`test_file_for(c, out, n)`** — Deterministic per-client filename `test_file_<name>_<size>` so runs reuse
  the same backing file.
- **`ensure_test_file(c)`** — Pre-creates the file with `fio --create_only` if missing, so timed phases don't
  pay file-layout cost and refault accounting stays clean.
- **`drop_caches()`** — Writes `3` to `/proc/sys/vm/drop_caches` so refaults start from a cold cache. Warns if
  it lacks permission (needs sudo). Linux only. By default it runs before **every** cached-mode phase; with
  the **`--drop-once`** flag it runs only before **phase 0**, so the cache built up in earlier phases persists
  and the tenants compete for it (required by the 3-phase eviction design).

### Running a client phase

- **`wrap_with_cgroup(cmd, cap, cg_path)`** — Rewrites the fio command so the spawned shell first echoes its
  pid into `<cg_path>/cgroup.procs` (joining the cgroup) and then `exec`s fio. This is how fio inherits the
  memory/io limits from the cgroup ini.
- **`spawn_client_phase(c, p, mode_str, cached, cg_path)`** — Builds the fio command, wraps it in the cgroup
  join, forks, and `exec`s it via `/bin/sh -c`. Writes JSON output to
  `benchmark_results/<client>_<mode>_p<phase>.json`. Returns the child pid so the caller can run clients
  concurrently and wait on them.

### Run orchestration

- **`cgpath_or_null(set, client, buf, n)`** — Returns the full cgroup path for a client, or `NULL` if cgroups
  are disabled or the client has no cgroup mapping.
- **`run_clients(cfg, cgset, client_names, n_clients, cached)`** — The heart of one experiment run for a
  single cache mode:
  1. Collects the participating cgroup names for the sampler.
  2. Computes `max_phases` across clients and pre-creates every test file.
  3. Starts the sampler and iostat loggers.
  4. For each phase index: drops caches (every phase, or only phase 0 under `--drop-once`), takes **before**
     memstat snapshots, spawns every client's phase concurrently, waits for all to finish, then takes
     **after** snapshots (which compute the refault/memory deltas).
  5. Stops iostat and the sampler.
  6. Appends a one-line run summary to `summary.txt`.
  Running the phases concurrently is what creates the contention being measured — A and B hit the shared (or
  isolated) cache at the same time.
- **`run_for_modes(cfg, cgset, names, n)`** — Runs `run_clients` for the selected cache mode(s): cached
  and/or direct, per `opt.mode`. `both` runs cached then direct so the same workload can be compared with and
  without the page cache in play.

### CLI / entry point

- **`usage(argv0)`** — Prints help: the three run modes (`<workload>` single, `dual`, `all`), all options
  (including `--drop-once`), and the important caveats (`-m cached` is required for cache/writeback effects;
  Linux required for the kernel telemetry).
- **`main(argc, argv)`** — Parses flags (`-c/--config`, `--cgroup-config`, `--no-cgroup`, `--no-psi`,
  `--drop-once`, `-m/--mode`, `-o/--output`, `-v/--verbose`, `-h`), loads the workload config, ensures the
  output dir, and —
  if a cgroup config was given — parses it and calls `setup_cgroups`. Then dispatches on the positional arg:
  - **`dual`** — runs `client1_steady` + `client2_noisy` concurrently (the primary A-vs-B experiment);
    errors if either section is missing.
  - **`all`** — runs every client section, each on its own (baselines/characterization).
  - **`<workload>`** — runs one named section alone (Case-1 baseline).

---

## How a run produces results (end-to-end)

1. **Configs are parsed.** `fairness_configs.ini` defines what A and B do (patterns, rates, phases);
   `cgroup_*.ini` defines the memory/io limits and cgroup layout they run under.
2. **cgroups are created and capped** (`setup_cgroups`) so A and B share a 2G parent pool (isolated vs shared
   variants differ in per-leaf protection).
3. **For each cache mode**, `run_clients` starts the background sampler (`vmstat`, per-cgroup `file_dirty`,
   PSI) and `iostat`, then for each phase drops caches (every phase, or only phase 0 under `--drop-once`),
   snapshots `memory.stat`, launches A and B's fio jobs concurrently inside their cgroups, waits, and
   snapshots again to compute refault/memory deltas.
4. **fio writes per-phase JSON** with `clat` percentiles — Tenant A's p99 latency is the SLO metric; the
   spike is the thing being explained.
5. **The telemetry** (`memstat/`, `psi/`, `dirty/`, `iostat/`) lets the analysis (e.g. `benchmark_analysis.py`)
   correlate A's latency spike with B's eviction activity (`workingset_refault_file_delta`,
   `pgscan_kswapd`, memory PSI → Mechanism 1) and with dirty writeback contention
   (`nr_dirty`/`file_dirty`/`file_writeback`, io PSI → Mechanism 2).

## Output layout (`benchmark_results/`)

| Path | Written by | Contents |
|------|-----------|----------|
| `<client>_<mode>_p<phase>.json` | `spawn_client_phase` (fio) | Per-phase fio results incl. `clat` p99/p999 |
| `memstat/<client>_<mode>.csv` | `record_memstat` | Before/after refault + `memory.current` and deltas |
| `dirty/vmstat_<mode>.csv` | `run_sampler` | System `nr_dirty`, `nr_writeback`, `pgpgin`, `pgscan_kswapd` |
| `dirty/<cg>_<mode>_dirty.csv` | `run_sampler` | Per-cgroup `file_dirty`, `file_writeback` time series |
| `psi/<cg>_<mode>.csv` | `run_sampler` | Per-cgroup memory/io PSI stall totals |
| `iostat/run_<mode>.iostat` + `.start_ts` | `start_iostat` | Device latency/queue depth + start wall-clock |
| `summary.txt` | `run_clients` | One line per run (clients, phases, cgroups/psi on/off) |