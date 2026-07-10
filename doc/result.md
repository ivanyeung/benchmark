# Result — 14-Row Docker Sweep (2026-07-08)

Pulled directly from the fio JSON (`clat_ns.percentile`) saved under each
`results/rowN_*` directory by the previous Docker run. See
[doc/docker-benchmark-how-to-run.md](docker-benchmark-how-to-run.md) for
how the sweep was run.

## Tenant A (`client1_steady`) p99 read latency — the primary metric

| Row | Condition | p99 (µs) | p99.9 (µs) |
|---|---|---|---|
| 1 | Client1 alone (baseline) | 0.88 | 9.15 |
| 5 | Isolated cgroups, dual | 3.12 | 47.87 |
| 6 | Isolated cgroups, dual | 3.63 | 48.90 |
| 7 | Isolated cgroups, dual | 3.34 | 46.85 |
| 8 | Shared cgroup, no cap | 12.10 | 54.53 |
| 9 | Shared cgroup, no cap | 12.74 | 56.06 |
| 10 | Shared cgroup, no cap | 3.89 | 77.31 |
| 11 | Shared, client1 capped 1G | 4.45 | 47.87 |
| 12 | Shared, client1 capped 1G | 4.90 | 48.38 |
| 13 | Shared, client2 capped 1G | 13.76 | 61.18 |
| 14 | Shared, client2 capped 1G | 6.82 | 49.92 |

(Rows 2, 3, 4 are `client2_noisy`-alone baselines, not `client1_steady` runs —
see full table below.)

## Full per-row table (both clients, as recorded in fio's own `job options`)

| Row | Client | rw (actual, from fio) | p99 (µs) | p99.9 (µs) |
|---|---|---|---|---|
| row1_client1_seq | client1_steady | randread | 0.88 | 9.15 |
| row2_client2_randread | client2_noisy | read | 1.16 | 20.61 |
| row3_client2_seq | client2_noisy | read | 1.46 | 27.52 |
| row4_client2_randwrite | client2_noisy | read | 1.50 | 35.58 |
| row5_isolated_seq | client1_steady | randread | 3.12 | 47.87 |
| row5_isolated_seq | client2_noisy | read | 3.63 | 34.56 |
| row6_isolated_randread | client1_steady | randread | 3.63 | 48.90 |
| row6_isolated_randread | client2_noisy | read | 3.22 | 26.50 |
| row7_isolated_randwrite | client1_steady | randread | 3.34 | 46.85 |
| row7_isolated_randwrite | client2_noisy | read | 3.12 | 34.56 |
| row8_shared_seq | client1_steady | randread | 12.10 | 54.53 |
| row8_shared_seq | client2_noisy | read | 2.58 | 29.06 |
| row9_shared_randread | client1_steady | randread | 12.74 | 56.06 |
| row9_shared_randread | client2_noisy | read | 2.51 | 28.29 |
| row10_shared_randwrite | client1_steady | randread | 3.89 | 77.31 |
| row10_shared_randwrite | client2_noisy | read | 2.22 | 17.28 |
| row11_c1cap_randread | client1_steady | randread | 4.45 | 47.87 |
| row11_c1cap_randread | client2_noisy | read | 1.80 | 20.10 |
| row12_c1cap_randwrite | client1_steady | randread | 4.90 | 48.38 |
| row12_c1cap_randwrite | client2_noisy | read | 1.88 | 21.38 |
| row13_c2limited_randread | client1_steady | randread | 13.76 | 61.18 |
| row13_c2limited_randread | client2_noisy | read | 2.10 | 21.89 |
| row14_c2limited_randwrite | client1_steady | randread | 6.82 | 49.92 |
| row14_c2limited_randwrite | client2_noisy | read | 2.38 | 21.89 |

## Caveats — read before trusting this data

1. **`client2_noisy`'s pattern never actually varied.** fio's own `job
   options` show `rw: read` (sequential) for every single client2_noisy run
   in rows 2–14, regardless of whether the directory name says `_randread`
   or `_randwrite`. Only `client1_steady` correctly stayed `randread`
   throughout. This means the intended sweep through Mechanism 1
   (read/randread) vs Mechanism 2 (randwrite, dirty writeback) never
   actually happened — every row ran client2 as a plain sequential scan. The
   `_randwrite` rows (4, 7, 10, 12, 14) are not distinguishable by pattern
   from their `_seq`/`_randread` siblings in the underlying data.

2. **These numbers don't match `Project Cache Fairness Notes.txt`.** That
   file claims row 1 (client1 alone) has p99 = 3.6 µs; the fio JSON says
   0.88 µs — off by roughly 4x, and the discrepancy isn't limited to that
   row. The table above is read directly from the saved fio output, so
   treat it as the authoritative source when reconciling with the notes
   file.

**Recommendation:** rerun the sweep, verifying (e.g. `grep phase_0_pattern
fairness_configs.ini`) that the pattern edit actually took effect
immediately before each `./benchmark` invocation, before drawing conclusions
about Mechanism 1 vs Mechanism 2.