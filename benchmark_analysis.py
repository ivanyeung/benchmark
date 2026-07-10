#!/usr/bin/env python3
"""benchmark_analysis.py — summarize page-cache fairness results.

Reads a results directory produced by ./benchmark and reports:
  * Tenant A p99 / p999 read latency (from fio clat_ns.percentile)
  * IOPS / bandwidth (secondary context)
  * workingset_refault_file_delta per phase per cgroup (from memstat/)
  * dirty-page pressure (from dirty/ — [TODO-3] vmstat + file_dirty samples)
  * per-cgroup memory.current per phase (from memstat/)
  * approximate cache hit/miss per phase (fio logical reads vs. iostat device
    reads observed during the phase window)

Usage: ./benchmark_analysis.py <results_dir>
"""

import csv
import json
import os
import sys
from glob import glob


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"  ! could not read {path}: {e}")
        return None


def us(ns):
    return ns / 1000.0


def report_latency(results_dir):
    print("\n## \U0001f4c8 READ LATENCY (Tenant A objective)")
    print("=" * 50)
    files = sorted(glob(os.path.join(results_dir, "*.json")))
    if not files:
        print("  (no fio json found)")
        return
    for path in files:
        data = load_json(path)
        if not data or "jobs" not in data:
            continue
        name = os.path.basename(path)
        for job in data["jobs"]:
            clat = job.get("read", {}).get("clat_ns", {})
            pct = clat.get("percentile", {})
            p99 = pct.get("99.000000")
            p999 = pct.get("99.900000")
            iops = job.get("read", {}).get("iops", 0.0)
            bw = job.get("read", {}).get("bw", 0.0)  # KiB/s
            line = f"  {name} [{job.get('jobname','?')}]:"
            if p99 is not None:
                line += f" p99={us(p99):.1f}us"
            if p999 is not None:
                line += f" p999={us(p999):.1f}us"
            line += f" iops={iops:.0f} bw={bw/1024:.1f}MiB/s"
            print(line)


def report_refaults(results_dir):
    print("\n## \U0001f501 WORKING SET REFAULTS (page-cache eviction churn)")
    print("=" * 50)
    memstat_dir = os.path.join(results_dir, "memstat")
    files = sorted(glob(os.path.join(memstat_dir, "*.csv")))
    if not files:
        print("  (no memstat csv found — Linux/cgroup v2 only)")
        return
    for path in files:
        name = os.path.basename(path).replace(".csv", "")
        print(f"\n**{name}:**")
        with open(path) as f:
            for row in csv.DictReader(f):
                if row.get("when") == "after":
                    delta = row.get("workingset_refault_file_delta", "-1")
                    try:
                        d = int(delta)
                    except ValueError:
                        d = -1
                    if d >= 0:
                        print(f"  phase {row['phase']}: {d:>12,} pages "
                              f"({d * 4096 / (1024*1024):8.1f} MiB)")


def report_dirty(results_dir):
    """[TODO-3] /proc/vmstat + per-cgroup file_dirty time series."""
    print("\n## \U0001f4a7 DIRTY-PAGE PRESSURE (vmstat + per-cgroup file_dirty)")
    print("=" * 50)
    dirty_dir = os.path.join(results_dir, "dirty")
    files = sorted(glob(os.path.join(dirty_dir, "*.csv")))
    if not files:
        print("  (no dirty csv found — Linux only)")
        return
    for path in files:
        name = os.path.basename(path).replace(".csv", "")
        peaks = {}
        with open(path) as f:
            reader = csv.DictReader(f)
            fields = [c for c in (reader.fieldnames or []) if c != "ts"]
            for row in reader:
                for c in fields:
                    try:
                        v = int(row[c])
                    except (ValueError, TypeError):
                        continue
                    peaks[c] = max(peaks.get(c, 0), v)
        summary = "  ".join(f"{c} peak={v:,}" for c, v in peaks.items())
        print(f"  {name}: {summary}")


def report_psi(results_dir):
    """PSI 'some' stall accrued over the run = last total - first total (us)."""
    print("\n## \U0001f6a6 PSI PRESSURE (memory + io 'some' stall over run)")
    print("=" * 50)
    psi_dir = os.path.join(results_dir, "psi")
    files = sorted(glob(os.path.join(psi_dir, "*.csv")))
    if not files:
        print("  (no psi csv found — Linux only)")
        return
    for path in files:
        name = os.path.basename(path).replace(".csv", "")
        rows = []
        with open(path) as f:
            for row in csv.DictReader(f):
                rows.append(row)
        if not rows:
            print(f"  {name}: (empty)")
            continue

        def accrued(col):
            vals = [int(r[col]) for r in rows
                    if r.get(col, "-1").lstrip("-").isdigit() and int(r[col]) >= 0]
            return (vals[-1] - vals[0]) if len(vals) >= 2 else -1

        mem = accrued("mem_some_total_us")
        io = accrued("io_some_total_us")
        parts = []
        if mem >= 0:
            parts.append(f"memory={mem/1000:.1f}ms")
        if io >= 0:
            parts.append(f"io={io/1000:.1f}ms")
        print(f"  {name}: {'  '.join(parts) if parts else '(no data)'}")


def report_memory(results_dir):
    """Per-cgroup memory.current before/after each phase (from memstat/)."""
    print("\n## \U0001f4be MEMORY CONSUMPTION (cgroup memory.current)")
    print("=" * 50)
    memstat_dir = os.path.join(results_dir, "memstat")
    files = sorted(glob(os.path.join(memstat_dir, "*.csv")))
    if not files:
        print("  (no memstat csv found — Linux/cgroup v2 only)")
        return
    for path in files:
        name = os.path.basename(path).replace(".csv", "")
        print(f"\n**{name}:**")
        with open(path) as f:
            for row in csv.DictReader(f):
                cur = row.get("memory_current_bytes")
                try:
                    cur_v = int(cur)
                except (TypeError, ValueError):
                    continue
                if cur_v < 0:
                    continue
                mib = cur_v / (1024 * 1024)
                print(f"  phase {row['phase']} ({row.get('when')}): {mib:9.1f} MiB")


def _phase_windows(memstat_path):
    """Return {phase: (before_wall_time, after_wall_time)} from a memstat csv."""
    tmp = {}
    with open(memstat_path) as f:
        for row in csv.DictReader(f):
            try:
                ph = int(row["phase"])
                wt = int(row["wall_time"])
            except (KeyError, ValueError, TypeError):
                continue
            tmp.setdefault(ph, {})[row.get("when")] = wt
    return {ph: (d["before"], d["after"]) for ph, d in tmp.items()
            if "before" in d and "after" in d}


def _parse_iostat_samples(iostat_path, start_ts):
    """Return [(wall_time, total_r_per_s)] for each periodic 1s report block.

    `iostat -dx 1` (no -t) prints a "since boot" report first, then one
    report per second after that with no per-sample timestamp. We attribute
    report index i (i>=1, i==0 is the since-boot report) to
    wall_time = start_ts + i — an approximation, but good enough to bucket
    ~1s samples into a ~60s phase window.
    """
    if not os.path.exists(iostat_path):
        return []
    with open(iostat_path) as f:
        content = f.read()
    samples = []
    for block_idx, block in enumerate(content.split("\n\n")):
        lines = [l for l in block.splitlines() if l.strip()]
        header_idx = next((i for i, l in enumerate(lines)
                            if l.strip().startswith("Device")), None)
        if header_idx is None:
            continue
        cols = lines[header_idx].split()
        if "r/s" not in cols:
            continue
        r_idx = cols.index("r/s")
        total_r = 0.0
        for line in lines[header_idx + 1:]:
            fields = line.split()
            if len(fields) <= r_idx:
                continue
            dev = fields[0]
            if dev.startswith(("loop", "ram", "dm-")):
                continue
            try:
                total_r += float(fields[r_idx])
            except ValueError:
                continue
        if block_idx > 0:
            samples.append((start_ts + block_idx, total_r))
    return samples


def report_cache_hit_miss(results_dir):
    """Approximate cache hit/miss: fio logical reads vs. iostat device reads
    observed in the phase's wall-clock window (see [[_parse_iostat_samples]])."""
    print("\n## \U0001f3af CACHE HIT / MISS (fio logical reads vs. device reads)")
    print("=" * 50)
    print("  Approximate: miss ≈ device reads seen in iostat during the phase")
    print("  window; hit = fio's logical reads minus that. iostat samples are")
    print("  bucketed by report index, not exact timestamp — treat as an")
    print("  estimate, not an exact count.")

    memstat_dir = os.path.join(results_dir, "memstat")
    iostat_dir = os.path.join(results_dir, "iostat")
    memstat_files = sorted(glob(os.path.join(memstat_dir, "*.csv")))
    if not memstat_files:
        print("  (no memstat csv found — needs --cgroup-config)")
        return

    for mpath in memstat_files:
        client_mode = os.path.basename(mpath).replace(".csv", "")
        client, _, mode = client_mode.rpartition("_")
        if not client:
            continue
        windows = _phase_windows(mpath)
        if not windows:
            continue

        iostat_path = os.path.join(iostat_dir, f"run_{mode}.iostat")
        ts_path = os.path.join(iostat_dir, f"run_{mode}.start_ts")
        if not (os.path.exists(iostat_path) and os.path.exists(ts_path)):
            print(f"\n**{client_mode}:** (no iostat data)")
            continue
        with open(ts_path) as f:
            start_ts = int(f.read().strip())
        samples = _parse_iostat_samples(iostat_path, start_ts)

        print(f"\n**{client_mode}:**")
        for ph in sorted(windows):
            before_ts, after_ts = windows[ph]
            device_reads = sum(r for (wt, r) in samples
                                if before_ts <= wt <= after_ts + 1)

            data = load_json(os.path.join(results_dir, f"{client}_{mode}_p{ph}.json"))
            total_ios = 0
            if data and "jobs" in data:
                for job in data["jobs"]:
                    total_ios += job.get("read", {}).get("total_ios", 0)

            if total_ios <= 0:
                print(f"  phase {ph}: (no fio read ios recorded)")
                continue
            misses = min(device_reads, total_ios)
            hits = max(0, total_ios - misses)
            hit_rate = 100.0 * hits / total_ios
            print(f"  phase {ph}: logical_reads={total_ios:,}  "
                  f"hit={hits:,.0f} ({hit_rate:.2f}%)  "
                  f"miss≈{misses:,.0f} ({100 - hit_rate:.2f}%)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    results_dir = sys.argv[1]
    if not os.path.isdir(results_dir):
        print(f"error: {results_dir} is not a directory")
        sys.exit(1)

    print(f"# Page-Cache Fairness Analysis — {results_dir}")
    report_latency(results_dir)
    report_refaults(results_dir)
    report_memory(results_dir)
    report_cache_hit_miss(results_dir)
    report_dirty(results_dir)
    report_psi(results_dir)
    print()


if __name__ == "__main__":
    main()
