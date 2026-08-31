# IMR-LSM YCSB Workload A-F Benchmark Progress

Last updated: 2026-08-31

This document summarizes the current implementation and validation status of
the IMR-LSM YCSB Workload A-F benchmark. The benchmark has completed
successfully on the Ubuntu VM; no test is executed on the Windows development
host.

## Current status

- Implemented a dedicated Workload A-F comparison runner:
  [`tests/imr_lsm_ycsb_workloads_comparison_test.sh`](tests/imr_lsm_ycsb_workloads_comparison_test.sh).
- Extended the single-workload runner to accept Workload F's
  `readmodifywriteproportion` and to capture IMRSim device counters.
- Fixed the userspace statistics-buffer allocation in
  [`imrsim_util/imrsim_util.c`](imrsim_util/imrsim_util.c). The allocation now
  uses `offsetof(struct imrsim_stats, zone_stats)` and the actual variable-tail
  size, preventing the stats ioctl from overrunning the heap buffer and
  producing the earlier `double free or corruption` failure.
- Completed Workloads A-F with three fresh-mapper repetitions each: 18/18 runs
  passed.
- All rows report `kernel_issue_count=0` and `review=none`.
- Backed up the result set under
  [`results/imr-lsm-ycsb-a-f-100k-rerun1`](results/imr-lsm-ycsb-a-f-100k-rerun1).

## Benchmark configuration

| Item | Value |
| --- | --- |
| YCSB records | 100,000 |
| YCSB operations | 100,000 |
| Repetitions | 3 per workload |
| Threads | 1 |
| Cache mode | Warm |
| `compaction_threshold` | 4096 records |
| `max_bytes_for_level_base` | 4096 bytes |
| `max_bytes_for_level_multiplier` | 10 |
| Filesystem | ext4, 4096-byte blocks |
| Filesystem test size | 262,144 blocks (1 GiB) |
| Backing device | `/dev/sdb`, 20 GiB |
| IMRSim mapper | 79 zones, 256 MiB per zone |
| Kernel | Linux 3.16.0-23-generic |
| Recorded repository commit | `fb13619e86fbd989d4fd2f98b5875426b2bef9d6` |

Every repetition resets the persistence area, creates a fresh IMRSim mapper,
creates a fresh ext4 filesystem, executes the 100K-record load phase, and then
executes the 100K-operation run phase. The exact captured configuration is in
the [result manifest](results/imr-lsm-ycsb-a-f-100k-rerun1/manifest.txt).

The run mixes are the standard YCSB mixes:

| Workload | Run mix |
| --- | --- |
| A | 50% READ, 50% UPDATE |
| B | 95% READ, 5% UPDATE |
| C | 100% READ |
| D | 95% READ, 5% INSERT; latest distribution |
| E | 95% SCAN, 5% INSERT |
| F | 50% READ, 50% READ-MODIFY-WRITE |

## Median results

All values below are medians of three successful runs. Latency is reported in
microseconds. Durable throughput includes the YCSB process, final `sync`, and
the IMR-LSM background-compaction drain.

| Workload | YCSB throughput (ops/s) | Durable throughput (ops/s) | Read/scan avg (us) | Read/scan P99 (us) | Write operation | Write avg (us) | Write P99 (us) | IMR WA | Metadata WA |
| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| A | 11,344.30 | 8,291.874 | 5.056 READ | 12 | UPDATE | 10.316 | 26 | 1.000000 | 4.687435 |
| B | 11,851.15 | 11,684.973 | 4.942 READ | 11 | UPDATE | 12.609 | 44 | 1.000000 | 4.778971 |
| C | 13,285.51 | 13,095.862 | 4.624 READ | 11 | NONE | NA | NA | 1.000000 | 4.829230 |
| D | 13,182.18 | 12,958.404 | 2.862 READ | 9 | INSERT | 8.122 | 26 | 1.000000 | 4.777353 |
| E | 6,652.03 | 6,600.660 | 77.807 SCAN | 170 | INSERT | 12.069 | 36 | 1.000000 | 4.778473 |
| F | 11,532.70 | 7,618.467 | 4.880 READ | 12 | READ-MODIFY-WRITE | 11.637 | 32 | 1.000000 | 4.769546 |

Key observations:

- Workload C has the highest YCSB throughput, closely followed by D.
- Workload E is the slowest because its primary operation is SCAN; its average
  scan latency is about 77.8 us and P99 is 170 us.
- Workload D has the lowest point-read average latency, about 2.86 us.
- A and F show a larger gap between YCSB and durable throughput, indicating
  more time outside the reported YCSB operation interval, including final
  synchronization and compaction draining.
- Workload C has no application-level write operation, so its write-latency
  fields are `NA`.

The generated comparison and complete median columns are available in
[`comparison.md`](results/imr-lsm-ycsb-a-f-100k-rerun1/comparison.md) and
[`medians.csv`](results/imr-lsm-ycsb-a-f-100k-rerun1/medians.csv).

## Repetition variability

The following table uses the three raw YCSB run-throughput measurements for
each workload. SD is the sample standard deviation, and CV is `SD / mean`.
The 95% confidence intervals use Student's t distribution with two degrees of
freedom; with only three samples they are necessarily wide and should be
treated as preliminary error bars.

| Workload | Min-max (ops/s) | Sample SD | CV | Mean 95% CI (ops/s) |
| --- | ---: | ---: | ---: | ---: |
| A | 11,205.7-13,570.4 | 1,327.0 | 11.02% | 8,743.6-15,336.7 |
| B | 10,949.3-12,062.7 | 591.3 | 5.09% | 10,152.2-13,089.9 |
| C | 11,724.7-14,289.8 | 1,292.6 | 9.87% | 9,889.1-16,310.9 |
| D | 12,473.5-13,386.9 | 479.3 | 3.68% | 11,823.5-14,204.8 |
| E | 6,387.7-6,739.0 | 182.9 | 2.77% | 6,138.5-7,047.4 |
| F | 10,560.8-13,413.8 | 1,450.5 | 12.25% | 8,232.6-15,438.9 |

- D and E are the most stable throughput results in this three-run sample.
- A, C, and F show about 10-12% throughput CV and would benefit most from more
  repetitions.
- The slowest individual cases relative to their same-workload peers are C
  repetition 3 and F repetition 2. A repetition 2 is unusually fast rather
  than unusually slow.
- No run failed, so these are performance variations rather than correctness
  failures. Three samples are insufficient for a defensible formal outlier
  classification.

The 18 unaggregated observations are preserved in
[`runs.csv`](results/imr-lsm-ycsb-a-f-100k-rerun1/runs.csv).

## Compaction coverage

`compaction_threshold=4096` is the L0 mapping-record trigger. Every run
recorded nonzero L0 compaction activity.

| Workload | Run total compactions (repetitions 1/2/3) | Run L0 compactions | Load L0 compactions | Load + run L0 |
| --- | ---: | ---: | ---: | ---: |
| A | 35 / 33 / 29 | 10 / 10 / 10 | 7 / 7 / 7 | 17 / 17 / 17 |
| B | 25 / 20 / 20 | 7 / 7 / 7 | 7 / 7 / 7 | 14 / 14 / 14 |
| C | 23 / 23 / 22 | 7 / 7 / 7 | 7 / 7 / 7 | 14 / 14 / 14 |
| D | 23 / 21 / 24 | 7 / 7 / 7 | 7 / 7 / 7 | 14 / 14 / 14 |
| E | 24 / 23 / 24 | 7 / 7 / 7 | 7 / 7 / 7 | 14 / 14 / 14 |
| F | 32 / 28 / 33 | 10 / 10 / 10 | 7 / 7 / 7 | 17 / 17 / 17 |

For all 18 repetitions, the observed count satisfies:

```text
total L0 compactions
  = floor((load LSM ingest records + run LSM ingest records) / 4096)
```

The remaining L0 records after each run are below 4096, so there is no complete
threshold-sized batch left unprocessed after the drain. This confirms that the
threshold was triggered rather than silently deferred.

The earlier threshold-sweep pilot proposed a conservative minimum of 15 total
L0 compactions. A and F meet that optional gate with 17; B, C, D, and E reach
14. The latter still trigger L0 compaction repeatedly, but a study that adopts
the strict 15-compaction gate must increase its workload size and rerun them.

## Write-amplification interpretation

Two different write-amplification metrics are intentionally reported:

```text
IMR WA = write_total / (write_total - extra_write_total)

Metadata WA =
  (ingested mapping bytes + metadata compaction output bytes)
  / ingested mapping bytes
```

### IMR WA

All load and run medians are 1.0 because every observed
`extra_write_total` delta is zero. This means the tested runs did not trigger
an overlapping-track protection write; it does not mean that the workload
issued no lower-layer writes.

The current allocation code does not make this RMW path unreachable:

- `IMR_ALLOCATION_PHASE=2` fills the bottom region of each zone first.
- Top allocation starts only after a single zone reaches 36,352 unique mapped
  4 KiB blocks, about 142 MiB.
- An update to an existing LBA writes back to its mapped PBA in place.
- Once an adjacent top block is marked used, a subsequent overlapping bottom
  update increments `extra_write_total` and performs the protection RMW.

Therefore the current result should be stated as "no overlapping-track extra
writes were observed under this workload," not "the allocation policy always
avoids IMR extra writes."

The completed artifacts do not contain a per-zone `z_map_size` snapshot, and
the comparison runner removes the mapper after every repetition. It is
therefore not possible to determine retrospectively whether all zones stayed
below the top-allocation boundary or whether top blocks existed but were not
followed by overlapping bottom updates.

### Metadata WA

- Every workload's median load metadata WA is 4.8 because each load phase
  inserts the same 100K records into a fresh mapper. The 18 raw load values are
  not identical; they range from approximately 4.659 to 4.800.
- Median run metadata WA ranges from 4.687435 for A to 4.829230 for C, a spread
  of approximately 3.03% relative to A.
- Metadata WA measures mapping-record rewriting during IMR-LSM compaction. It
  is separate from the payload-level overlapping-track IMR WA.
- Workload C has no YCSB UPDATE/INSERT operation, but its measurement interval
  includes RocksDB open/cleanup, filesystem activity, final `sync`, and the
  IMR-LSM compaction drain. Its three runs each recorded 28,881 lower-layer
  writes with zero overlapping-track extra writes, so its metadata-WA
  denominator is nonzero rather than `NA`.

## Reproduction

The comparison must run on the Ubuntu test host and destroys the selected
backing device contents:

```bash
cd ~/IMRSim
make -C imrsim_util

sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME="$HOME/YCSB" \
  IMR_LSM_YCSB_COMPARE_RESULT_DIR=/var/tmp/imr-lsm-ycsb-a-f-100k \
  bash tests/imr_lsm_ycsb_workloads_comparison_test.sh /dev/sdb
```

Basic result-integrity check:

```bash
RESULT=/var/tmp/imr-lsm-ycsb-a-f-100k

awk -F, '
NR > 1 && ($3 != "PASS" || $36 != 0 || $37 != "none") {
    print
}' "$RESULT/runs.csv"
```

No output means every run passed with no recorded kernel issue and no review
flag.

## Remaining work

1. Capture per-zone occupancy before mapper teardown. Save the output of
   `imrsim_util /dev/mapper/imrsim z 6 4` after load and after run so top-track
   allocation can be distinguished from a bottom-only layout.
2. Add explicit top-write, bottom-update, and overlapping-bottom-update
   counters. `extra_write_total` alone cannot distinguish "top never used"
   from "top used but no overlapping bottom update."
3. Run an update-heavy stress configuration, preferably 100K records with a
   larger operation count or a 100% UPDATE workload, to exercise physical
   reuse and the bottom-track RMW path.
4. Increase repetitions for A, C, and F. At least five runs would improve
   variance estimates; more are preferable for confidence intervals.
5. Resolve the Bash `args_ref: circular name reference` warnings in the
   single-workload runner. The completed tests still used the standard A-F
   workload files, but the optional property-override path should be warning
   free before the next formal run.
6. Preserve the complete nested `artifacts/` directories in the next backup.
   The current repository backup contains the aggregate CSV files, runner
   logs, final stats, mapper tables, and kernel logs, but not all temporary
   before/after snapshots produced on Ubuntu.
7. Commit the runner, utility fix, documentation, and result set together so
   the exact benchmark implementation is traceable. The manifest records the
   base Git commit, while the current benchmark-related files are still local
   working-tree changes.

## Result files

- [Manifest](results/imr-lsm-ycsb-a-f-100k-rerun1/manifest.txt)
- [All 18 raw rows](results/imr-lsm-ycsb-a-f-100k-rerun1/runs.csv)
- [Per-workload medians](results/imr-lsm-ycsb-a-f-100k-rerun1/medians.csv)
- [Generated comparison table](results/imr-lsm-ycsb-a-f-100k-rerun1/comparison.md)
- [Existing runtime correctness report](TEST_REPORT.md)
