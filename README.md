## Abstract

The emerging interlaced magnetic recording (IMR) technology achieves a higher areal density for hard disk drive (HDD) over the conventional magnetic recording (CMR) technology. IMR-based HDD interlaces top tracks and bottom tracks, where each bottom track is overlapped with two neighboring top tracks. Thus, top tracks can be updated without restraint, whereas bottom tracks can be updated by the time-consuming read-modify-write (RMW) update strategy. Therefore, the layout of the tracks between the IMR-based HDD and the CMR-based HDD is much different. Unfortunately, there has been no related disk simulator and product available to the public, which motivates us to develop an open-source IMR disk simulator to provide a platform for further research.

We implement the first public IMR disk simulator, called IMRSim, as a block device driver in the Linux kernel, simulating the interlaced tracks and implementing many state-of-the-art data placement strategies. IMRSim is built on the actual CMR-based HDD to precisely simulate the I/O performance of IMR drives. While I/O operations in CMR-based HDD are easy to visualize, RMW strategy and multi-stage allocation strategy in IMR are inherently dynamic. Therefore, we further graphically demonstrate how IMRSim processes I/O requests in the visualization mode. We release IMRSim as an open-source IMR disk simulation tool and hope to attract more scholars into related research on IMR technology.



## Design

IMRSim is the first open-source simulator for IMR drives. We design and implement it in the Linux kernel using the principle of block device driver development and Device Mapper (DM) framework.

On the whole, we design IMRSim into two parts: the kernel module and the user interface program:

- **The kernel module** is designed based on the Device Mapper framework to build the structure of the disk simulator and implement the required functions (read, write, stage allocation strategy, etc.).
- **The user interface program** is a extensible tool to build the channel between the user and the simulator.  IMRSim uses the ioctl interface provided by Devcie Mapper to build a set of configuration tools that can be used by users. The user interface program is developed in the user space using the C standard library.

More details can be found in the paper ([IMRSim: A Disk Simulator for Interlaced Magnetic Recording Technology]([IMRSim: A Disk Simulator for Interlaced Magnetic Recording Technology | SpringerLink](https://link.springer.com/chapter/10.1007/978-3-031-21395-3_25))).



## Source (version 1.1.0)

You can get IMRSim V1.1.0 at the following link:

1. github: [AlieZ22/IMRSim: A Disk Simulator for Interlaced Magnetic Recording Technology (github.com)](https://github.com/AlieZ22/IMRSim)
3. get in touch with us.



## Build

operating system: Linux（Recommended version: Ubuntu14.10）

1. Enter the directory where `IMRSim` is located and build the kernel module:

   ```bash
   $ make
   ```

2. Load the driver into the kernel:

   ```bash
   $ sudo make install
   $ sudo depmod --quick
   $ sudo modprobe dm-imrsim
   ```

4. prepare a block device：

   （a）Use block devices (eg, /dev/sdb) or partitions (eg, /dev/sdb1) directly, and the capacity requirement is greater than 256MB.

   （b）Use a loop device. A zone is 256 MiB, and the persistence tail grows
   with the zone count. In the following example, an 80-zone device is
   constructed without assuming that a fixed 2 MiB tail is sufficient:

   ```bash
   $ zones=80
   $ pstore_bytes="$(imrsim_util/imr_format.sh -p "${zones}")"
   $ truncate -s "$((zones * 256 * 1024 * 1024 + pstore_bytes))" /tmp/imrsim1
   $ loopdev="$(sudo losetup --find --show /tmp/imrsim1)"
   ```

5. Inspect the zone count and usable sector count of the block device. Both
   forms are read-only; they do not format or initialize the device:

   （a）zones：

   ```bash
   $ sudo imrsim_util/imr_format.sh -z -d "${loopdev}"
   ```

   （b）sectors：

   ```bash
   $ sudo imrsim_util/imr_format.sh -d "${loopdev}"
   ```

6. Initialize the dynamically sized persistence range, then create the `IMRSim`
   device. Initialization writes to the selected block device, so it requires
   both root privileges and an explicit destructive-operation opt-in:

   ```bash
   $ sectors="$(sudo env IMR_LSM_TEST_DESTRUCTIVE=1 imrsim_util/imr_format.sh -i -d "${loopdev}")"
   $ printf '0 %s imrsim %s 0\n' "${sectors}" "${loopdev}" | sudo dmsetup create imrsim
   ```

   The generic mapper form is:

   ```bash
   $ printf '0 %s imrsim /dev/<your device> 0\n' "<sectors>" | sudo dmsetup create imrsim
   ```

   If the build is successful, the IMRSim device will be created and stored in `/dev/mapper/imrsim`.

   `imr_format.sh -p ZONES` uses the same 64-bit state layout as the module to
   size this tail. The target constructor rejects a backing device whose tail
   is too small. Because the persisted header stores its length as 32 bits,
   the current format accepts at most 7,759 zones. Run `-p` on the Linux host
   that will load the module: the reserve is rounded to that kernel's page
   size, so an image prepared on a host with a different page size must be
   recalculated. Serialized layout version 1.2.0 adds the persistent physical
   append log and intentionally resets older state rather than attempting an
   unsafe in-place migration. A corrupt
   current-version snapshot fails target creation without being overwritten;
   rerun `-i` only when explicitly choosing to discard that metadata.
   The runtime persistence worker copies a consistent state snapshot and dirty
   zone list while holding `imrsim_zone_lock`, then performs CRC calculation
   and synchronous backing-device writes after releasing the lock. Data-path
   `KERN_INFO` messages are disabled by default and use the existing debug-log
   switch when detailed I/O tracing is explicitly needed.

   The data path reserves one physical zone as GC overprovisioning, so the
   mapper requires at least two zones. Logical writes are appended to one
   active physical zone regardless of their logical LBA; a full zone is sealed
   and later compacted into an empty zone selected from the free-zone pool.

7. Use `imrsim_util.c` for interface function testing, or use tools such as `fio` for performance testing, or perform other tests in the `file system`.



## Destroy

After building IMRSim, to destroy it, you can do the following:

1. ```bash
   $ sudo dmsetup remove imrsim
   ```

2. ```bash
   $ sudo losetup -d /dev/loop1
   ```

3. ```bash
   $ sudo rmmod dm_imrsim
   ```

4. ```bash
   $ sudo make clean
   ```



## How to use

### I/O Alignment Contract

IMR-LSM stores metadata at a 4 KiB key granularity. The target asks Device
Mapper to keep normal data I/O bounded to 4 KiB where possible, so aligned
multi-block requests are remapped and recorded one block at a time. Sub-4 KiB
or unaligned data I/O is handled by a worker-based read-modify-write path: the
target reads the covered 4 KiB logical block, merges the requested sectors,
and publishes a normal 4 KiB metadata mapping. Discard ranges must still have
a 4 KiB-aligned start and length. An unaligned discard that reaches the target
is rejected; the block layer may instead complete a range smaller than the
advertised 4 KiB discard granularity as a no-op. In either case it must not
publish a tombstone or delete an adjacent full block. The current singleton
layout also requires both the Device Mapper target and its backing-device
range to begin at sector 0.

### IMR-LSM Debug Validation

`seed_full_zone` is exposed only as a VM/debug validation helper through
debugfs. It seeds zone metadata so a test VM can exercise full-zone compaction
quickly without first writing an entire zone. It is not a formal IMR-LSM data
path and should not be treated as production behavior.

Formal zone compaction is still handled by `compact_zone`.

When a zone reaches full metadata capacity, it is sealed.  A sealed zone is a
normal zone-GC candidate only after its invalid ratio reaches 250 permille
(25%).  If the number of free physical zones falls below the default low
watermark of 3, pressure mode permits a lower-ratio victim so forward progress
can use the permanent GC spare.  Candidate selection ranks invalid ratio
first, reclaimable block count second, and oldest generation last.  This keeps
nearly-all-live zones out of GC while free capacity is plentiful.

The current policy and selection decision are exported in `stats` as
`zone_gc_*` and `zone_compaction_candidate_*`.  The defaults can be adjusted
for a controlled experiment through debugfs; values are reset when the mapper
is recreated:

```bash
# 250 permille = 25%; valid range is 0..1000.
echo 250 | sudo tee \
  /sys/kernel/debug/imrsim_lsm/zone_gc_min_invalid_ratio_permille

# Pressure mode is active while free-zone count is strictly below this value.
echo 3 | sudo tee \
  /sys/kernel/debug/imrsim_lsm/zone_gc_free_low_watermark

sudo grep -E \
  '^(zone_gc_.*|zone_compaction_candidate_(zone|dest_zone|reclaimable|ratio_permille|pressure|ready)):' \
  /sys/kernel/debug/imrsim_lsm/stats
```

Automatic zone compaction is enabled by default; write `0` to
`zone_compaction_auto_run` to disable it for controlled manual validation.
Manual `compact_zone` requests intentionally bypass the automatic eligibility
threshold.

On a fresh mapper whose source, active-append, and free-pool zones are
disposable, the small marker/readback test can exercise that automatic path
directly:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_ZONE_COMPACTION_MODE=auto \
  IMR_LSM_ZONE_COMPACTION_ZONE=0 \
  IMR_LSM_ZONE_COMPACTION_AUTO_TIMEOUT=120 \
  bash tests/imr_lsm_zone_compaction_test.sh /dev/mapper/imrsim
```

The test disables auto compaction while it seeds metadata and writes four
marker blocks, temporarily sets the invalid-ratio threshold to zero, enables
auto compaction to trigger the pending candidate, waits for the worker to
become idle, and then restores the policy while verifying payloads, counters,
and bottom-track placement. The default mode remains `manual` for the original
debugfs test.

The IMR-LSM read path also exposes a bounded read acceleration tree through
`read_tree` and related `read_tree_*` stats. It caches the latest key to PBA
mapping with LRU eviction while preserving tombstone visibility.
`clear_read_tree` is a VM/debug validation helper for forcing tree misses while
leaving LSM metadata intact.
`read_tree_limit` is a VM/debug validation helper for temporarily lowering the
tree capacity; writing `0` restores the default capacity.
`compaction_threshold`, `max_bytes_for_level_base`,
`max_bytes_for_level_multiplier`, and `bloom_bits_per_key` are VM/debug
validation helpers for parameter sweeps, not stable production tuning
interfaces. Writing `0` restores their defaults. `compaction_threshold` is
the L0 record trigger. New records always enter L0; an L0 compaction writes
directly to the dynamically selected base level. L1-L6 capacity and
compaction scores use logical metadata bytes instead of entry counts. One
mapping record has a stable 32-byte encoded cost; transient pointers and
compiler padding are excluded. `max_bytes_for_level_base` defaults to 512
bytes and `max_bytes_for_level_multiplier` defaults to 10. The base-level
target is selected in `(base / multiplier, base]`, while the last-level target
is anchored to the largest current level, following RocksDB dynamic-level
semantics. If the configured fanout cannot fit the current metadata into the
fixed seven-level tree, an integer effective multiplier is raised and exposed
as `effective_max_bytes_for_level_multiplier`. Parameter changes affect
subsequent compaction decisions but do not rebuild existing segments.
`bloom_bits_per_key` affects only Bloom filters in segments built after the
change; existing segments retain their filters. The stats file exposes
`metadata_compaction_input_bytes` and `metadata_compaction_output_bytes` for
metadata write-amplification comparisons.

Automatic level compaction runs on the dedicated single-thread
`imrsim_lsm_level` workqueue rather than in the foreground device-mapper write
path. A foreground write publishes its L0 mapping and schedules work when a
level reaches its trigger. Each worker invocation executes at most one
compaction round and requeues itself while more work is eligible. A round now
uses three phases: prepare captures an exact source run under the zone/LSM
locks, build allocates and constructs the sorted delta, segment table, and
Bloom filter with both global locks released, and publish validates and
splices the plan under the locks. The one-pass publish merge is O(source plus
destination); newly prepended foreground records are left in place. The
compaction mutex remains held across all three phases so zone compaction,
metadata reset, and validation-policy changes cannot invalidate borrowed
nodes. L0 work freezes one `compaction_threshold`-sized batch so worker
scheduling cannot silently enlarge one compaction. Mapper teardown calls
`cancel_work_sync()` before releasing LSM metadata. `stats` exposes
`level_compaction_pending`, `level_compaction_running`, the
`level_compaction_work_*` counters, and the last worker error. Destructive
tests wait for pending background level work to drain before asserting final
metadata state.

The same `stats` file also exposes mapper-lifetime diagnostic counters without
changing compaction policy. `level_compaction_{queue_wait,work}*_ns` separates
workqueue delay from worker runtime, while `level_compaction_{total,max}_ns`
measures individual background compaction rounds. Per-level
`levelN_compaction_{time_count,total_ns,max_ns}` counters identify whether L0
or a lower-level cascade owns the cost. Separate zone/LSM lock-wait counters
distinguish worker contention from foreground data-path contention. The
short aliases `compaction_{total,max}_ns` expose the same level-compaction
timers, `compaction_queue_depth{,_max}` reports the dedicated worker queue,
and `imr_lsm_lock_wait_{count,total_ns,max_ns}` aggregates foreground and
background waits for quick workload comparisons.
`flush_bio_count`, `incoming_fua_write_count`, `fua_write_count`, and the
internal flush/RMW FUA breakdown identify durable-write traffic. The incoming
counter records requests presented to the target; `fua_write_count` records
FUA data writes actually remapped or submitted by IMRSim. Internal
`WRITE_FLUSH_FUA` submissions also expose total/max/last completion time.
`partial_{io,rmw}_*` and `legacy_rmw_*` report queued partial-I/O and older RMW
service times. Count and total fields can be differenced across a workload;
max and last fields are mapper-lifetime observations and must not be
subtracted. These counters reset when a new singleton mapper is created and
are not serialized to persistence.

Worker lock-hold diagnostics further split runtime into
`level_compaction_{zone,lsm}_lock_hold_*`, the post-compaction phase into
`level_compaction_post_round_*`, and the three phase timers into
`level_compaction_{prepare,build,publish}_*`. `level_compaction_phase` reports
0=idle, 1=prepare, 2=unlocked build, and 3=publish. The bounded VM-only
`level_compaction_build_delay_ms` control injects up to 5000 ms into phase 2
for concurrency validation and resets with the mapper. Successful compaction
paths update
invalid-metadata statistics while committing their segment changes, so the
worker no longer performs a redundant tail scan. The exported
`level_compaction_post_recalc_*` compatibility counters therefore remain zero;
a nonzero value indicates a legacy or unexpected worker-tail scan. The global
`invalid_recalc_*` timer now covers only explicit validation/repair scans and
the allocation-failure fallback; ordinary level and zone compaction publish
does not increment it.

Invalid accounting is incremental. Each newest-key index node links the active
segment entries tied at that key's latest timestamp. A new write invalidates
only those linked entries. Compaction publish scans only its new output table,
and source retirement subtracts only the retired segment's aggregate counters;
candidate selection still scans segment headers but never all historical
entries. All mutations remain under the same zone/LSM locks. The
`invalid_incremental_{supersede,entries_updated,segment_publish_*,segment_retire_*}`
counters expose this work, while
`invalid_incremental_fallback_recalc_count` must remain zero in a healthy run.
In an isolated dynamic-level workload, one successful level-compaction round
therefore corresponds to one `invalid_incremental_segment_publish_count`
increment and zero `invalid_recalc_count` increments.
As useful approximations, one round consists of prepare, unlocked build,
publish, and private-object cleanup. Each prepare/publish/worker-bookkeeping
lock acquisition contributes a separate wait and hold sample; lock-hold totals
therefore intentionally exclude build time and must not be treated as another
copy of the complete round timer.

On a fresh disposable mapper, this deterministic regression injects a
five-second build delay, observes phase 2, and proves that a foreground
write/read completes before that delay expires. It also checks the phase
counters, publish conflicts, and that neither global lock-hold maximum includes
the injected delay:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  bash tests/imr_lsm_level_compaction_unlocked_build_test.sh \
  /dev/mapper/imrsim
```

`invalid_recalc_segments_scanned_*` counts all segment-list nodes visited,
including retired nodes, while `invalid_recalc_entries_scanned_*` counts
entries examined in active segments during an explicit full recalculation.
They should remain zero for normal metadata-compaction-only workloads. A full
recalculation also rebuilds the runtime newest-entry links and includes
segment-compaction candidate selection. Aggregate and per-level
`level_compaction_{input,output}_entries_*` counters describe successful
batches and attribute them to the source level; output entries are newly
emitted 32-byte mapping records, not the destination level's cumulative size.
`level_compaction_coalesced_schedule_count` counts an eligible trigger seen
while work is already pending, not queue depth, and
`level_compaction_no_work_run_count` counts worker invocations that find no
eligible round after acquiring the locks. Compaction scores use 1000 as the
eligibility boundary; the bottom-level unsorted flush reports a synthetic
score of 1000. `level_compaction_score_max` is the mapper-lifetime maximum
evaluated score, while the three `last_*_score` fields retain the most recent
evaluation, foreground schedule, and worker requeue scores. The YCSB runner
reports count/total fields as phase deltas but prints every `last_*` and
`*_max` field as a mapper-lifetime snapshot.

`tests/imr_lsm_parameter_sweep_test.sh` validates parameter combinations for
read tree capacity, 4KB/8KB/16KB/64KB writes, compaction thresholds, and Bloom
filter sizing. Tests that mutate an existing mapper require an explicit
destructive-operation opt-in:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  tests/imr_lsm_parameter_sweep_test.sh /dev/mapper/imrsim
```

The I/O boundary regression test checks interleaved multi-block remapping,
partial-block data read-modify-write, and safe rejection or no-op handling of
partial-block discard requests:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  tests/imr_lsm_io_boundary_test.sh /dev/mapper/imrsim
```

Workload-proximity smoke tests are also available for less controlled I/O
patterns. These tests are destructive. The ext4 and RocksDB tests create a new
bounded ext4 filesystem on the mapper before running their workloads:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  bash tests/imr_lsm_fio_mixed_workload_test.sh /dev/mapper/imrsim

sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  bash tests/imr_lsm_ext4_fstrim_workload_test.sh /dev/mapper/imrsim

sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  bash tests/imr_lsm_rocksdb_small_workload_test.sh /dev/mapper/imrsim
```

`imr_lsm_fio_mixed_workload_test.sh` requires `fio` and runs bounded
randwrite, randread/randwrite, randread, and randtrim phases over a small
aligned range, with trim last because deleted blocks may reject later reads.
`imr_lsm_ext4_fstrim_workload_test.sh` requires `mkfs.ext4`,
`mount`, and `fstrim`, then validates create/update/delete/readback through
ext4 before FITRIM and after a post-FITRIM remount. By default it formats a
64 MiB filesystem (`IMR_LSM_FS_MKFS_BLOCKS=16384`) and trims a 4 MiB window
(`IMR_LSM_FS_FSTRIM_LENGTH_BYTES=4194304`) so the smoke test does not spend
minutes initializing the entire mapper.
`imr_lsm_rocksdb_small_workload_test.sh` requires the RocksDB `ldb` tool,
performs put/update/delete/readback through RocksDB on ext4, and runs a
bounded FITRIM plus remount by default. It uses 8 keys by default
(`IMR_LSM_ROCKSDB_KEYS=8`) because the older `ldb` CLI opens the database per
command and can otherwise amplify WAL/log file creation enough to fill the
small smoke-test filesystem. It trims a 16 MiB window by default
(`IMR_LSM_ROCKSDB_FSTRIM_LENGTH_BYTES=16777216`) because a 4 MiB window can sit
entirely inside ext4 metadata or live RocksDB blocks and produce no discard.
Its filesystem and trim windows are controlled by `IMR_LSM_ROCKSDB_MKFS_BLOCKS`
and `IMR_LSM_ROCKSDB_FSTRIM_LENGTH_BYTES`. Set
`IMR_LSM_ROCKSDB_LDB=/path/to/ldb` if the tool is not named `ldb`.

For KVIMR-style experiments, use YCSB to generate the RocksDB workload shape
and place the RocksDB database directory on an ext4 filesystem mounted from the
IMRSim mapper. This does not reimplement KVIMR; it lets IMR-LSM run a comparable
YCSB-over-RocksDB load/run workload while debugfs records IMR-LSM metadata
counters:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  bash tests/imr_lsm_ycsb_rocksdb_workload_test.sh /dev/mapper/imrsim
```

The default run is a small smoke workload using `workloada`, 10,000 records,
10,000 operations, 4 threads, Zipfian requests, and 1 KiB records
(`fieldcount=10`, `fieldlength=100`). The script formats the mapper, mounts
ext4, runs `ycsb load rocksdb`, runs `ycsb run rocksdb`, and prints deltas for
`lsm_record_insert_count`, `read_lookup_count`, `compaction_count`,
`segment_hit_count`, `fallback_count`, tombstone/discard counters, and related
read-path counters. Override the workload shape with environment variables.
Set `IMR_LSM_YCSB_DROP_CACHES=1` to sync and clear Linux page, dentry, and inode
caches after `ycsb load` closes RocksDB and before `ycsb run` starts. The script
then captures a fresh run-phase counter baseline, allowing reads that miss
RocksDB's new-process cache to reach the IMRSim mapper:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_YCSB_DROP_CACHES=1 \
  IMR_LSM_YCSB_COMPACTION_THRESHOLD=128 \
  IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_BASE=4096 \
  IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_MULTIPLIER=10 \
  bash tests/imr_lsm_ycsb_rocksdb_workload_test.sh /dev/mapper/imrsim
```

Set `IMR_LSM_YCSB_CLEAR_READ_TREE=1` in a separate cold-read experiment to
clear the kernel IMR-LSM read tree after load as well. This forces the first
device reads during RocksDB open/run through unsorted and segment metadata
instead of the read-tree cache. Keep the default value `0` when measuring the
complete IMR-LSM design with its read acceleration tree enabled.

`IMR_LSM_YCSB_COMPACTION_THRESHOLD` accepts the same range as the debugfs
`compaction_threshold` control. The runner applies it before `mkfs.ext4` and
restores the prior value during cleanup. When a threshold is supplied without
`IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_BASE`, the runner sets base bytes to
`threshold * 32`, so one full L0 batch initially matches the base-level byte
target. `IMR_LSM_YCSB_MAX_BYTES_FOR_LEVEL_MULTIPLIER` controls the dynamic
fanout independently. Recreate the mapper between parameter runs so metadata
produced by one configuration cannot affect the next run. A typical threshold
sweep uses `128`, `256`, `512`, and `1024` with otherwise identical YCSB
parameters.

Use the formal threshold-sweep runner to execute that matrix with three fresh
mapper instances per threshold. The backing device is destructive test input:
the runner removes the named mapper, resets the backing device's IMR-LSM
persistence area, and recreates the mapper before every individual run.

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_SWEEP_RESULT_DIR=/var/tmp/imr-lsm-threshold-formal-1 \
  bash tests/imr_lsm_ycsb_threshold_sweep_test.sh /dev/sdb
```

The default matrix fixes `recordcount=10000`, `operationcount=10000`,
`threadcount=1`, warm Linux and IMR-LSM caches between the YCSB load and run
phases, `max_bytes_for_level_base=4096`, and
`max_bytes_for_level_multiplier=10`. It runs thresholds in repetition rounds
(`128`, `256`, `512`, `1024`, then the same order twice more) to reduce
time-order bias. Override the workload size with
`IMR_LSM_SWEEP_RECORD_COUNT` and `IMR_LSM_SWEEP_OPERATION_COUNT`; the defaults
remain 10,000. `IMR_LSM_SWEEP_THREAD_COUNT` defaults to one. The result path
must not already exist.

`runs.csv` contains every run's throughput, durable throughput including final
sync and both level/zone background-compaction drain, YCSB P99/max latency,
zone-compaction counts and failures, invalid recalculation time, foreground
zone-lock wait, and kernel-error count.
`medians.csv` contains the per-threshold medians and `decision.txt` selects the
best complete threshold by median run-phase durable throughput. Each run also
retains raw YCSB output, before/after debugfs stats, mapper information, and
the dmesg interval delimited by kernel-log markers. A new hung-task,
`jbd2`/sync-blocked, or I/O-error signature stops the sweep by default. An
average invalid recalculation over one second or a mapper-lifetime maximum at
or above 1.8 seconds marks the result for third-phase review without discarding
the remaining measurements. The captured stats also require the mapper-lifetime
newest-key index to remain valid; an allocation failure or correctness fallback
marks the run for review. Any nonzero
`invalid_incremental_fallback_recalc_count` also marks the run because it means
the fast path had to invoke a full validation scan.

Set `IMR_LSM_SWEEP_MIN_ZONE_COMPACTIONS` only when the workload is known to
produce a full-zone candidate. The 500K/500K threshold pilot is a level-
compaction gate and therefore permits zero zone candidates; run the small
auto-zone test above as the independent zone-correctness gate:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_YCSB_MKFS_BLOCKS=1048576 \
  IMR_LSM_SWEEP_RESULT_DIR=/var/tmp/imr-lsm-zone-threshold-500k-pilot \
  IMR_LSM_SWEEP_THRESHOLDS=4096 \
  IMR_LSM_SWEEP_REPETITIONS=1 \
  IMR_LSM_SWEEP_RECORD_COUNT=500000 \
  IMR_LSM_SWEEP_OPERATION_COUNT=500000 \
  IMR_LSM_SWEEP_THREAD_COUNT=1 \
  IMR_LSM_SWEEP_MIN_L0_COMPACTIONS=20 \
  IMR_LSM_SWEEP_MIN_ZONE_COMPACTIONS=0 \
  bash tests/imr_lsm_ycsb_threshold_sweep_test.sh /dev/sdb
```

When the best threshold is the upper edge of the tested range, first verify
that a larger threshold is not merely deferring compaction beyond the workload
horizon. The runner records total and L0-only compaction counts plus the final
L0 actual bytes. A 100K/100K threshold-4096 pilot that requires at least 15 L0
compactions is:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_SWEEP_RESULT_DIR=/var/tmp/imr-lsm-threshold-4096-pilot \
  IMR_LSM_SWEEP_THRESHOLDS=4096 \
  IMR_LSM_SWEEP_REPETITIONS=1 \
  IMR_LSM_SWEEP_RECORD_COUNT=100000 \
  IMR_LSM_SWEEP_OPERATION_COUNT=100000 \
  IMR_LSM_SWEEP_THREAD_COUNT=1 \
  IMR_LSM_SWEEP_MIN_L0_COMPACTIONS=15 \
  bash tests/imr_lsm_ycsb_threshold_sweep_test.sh /dev/sdb
```

If the pilot passes, run the formal upper-bound comparison with thresholds
1024, 2048, and 4096, three fresh mapper instances each, and the same 100K
workload. If it reports insufficient L0 coverage, increase both workload counts
and use a new result directory before comparing thresholds.

To compare the standard YCSB Workload A through F at the selected parameters,
use the dedicated workload-comparison runner. Its defaults are
`recordcount=100000`, `operationcount=100000`, `threadcount=1`,
`compaction_threshold=4096`, `max_bytes_for_level_base=4096`, and
`max_bytes_for_level_multiplier=10`. The backing device is destructive input;
the runner resets it and creates a fresh mapper before every workload run. It
runs three repetitions by default. The run mixes remain fixed to A=50%
read/50% update, B=95% read/5% update, C=100% read, D=95% read/5% insert with
the latest distribution, E=95% scan/5% insert, and F=50% read/50%
read-modify-write.

The completed three-repetition result summary, variability analysis,
compaction coverage, write-amplification interpretation, and remaining work
are documented in [`YCSB_A_F_PROGRESS.md`](YCSB_A_F_PROGRESS.md).

```bash
make -C imrsim_util
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_YCSB_COMPARE_RESULT_DIR=/var/tmp/imr-lsm-ycsb-a-f-100k \
  bash tests/imr_lsm_ycsb_workloads_comparison_test.sh /dev/sdb
```

The comparison-level workload, LSM, cache, Bloom-filter, and zone-GC controls
can be overridden without editing the runner. The following command selects
300,000 records, 1,000,000 operations, eight threads, a 2 GiB ext4 test
filesystem, cold Linux and IMR-LSM read caches, and the stated IMR-LSM policy:

```bash
make -C imrsim_util
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_YCSB_COMPARE_RESULT_DIR=/var/tmp/imr-lsm-ycsb-a-f-300k-1m \
  IMR_LSM_YCSB_COMPARE_REPETITIONS=3 \
  IMR_LSM_YCSB_COMPARE_RECORD_COUNT=300000 \
  IMR_LSM_YCSB_COMPARE_OPERATION_COUNT=1000000 \
  IMR_LSM_YCSB_COMPARE_THREAD_COUNT=8 \
  IMR_LSM_YCSB_COMPARE_COMPACTION_THRESHOLD=4096 \
  IMR_LSM_YCSB_COMPARE_MAX_BYTES_FOR_LEVEL_BASE=131072 \
  IMR_LSM_YCSB_COMPARE_MAX_BYTES_FOR_LEVEL_MULTIPLIER=10 \
  IMR_LSM_YCSB_COMPARE_DROP_CACHES=1 \
  IMR_LSM_YCSB_COMPARE_CLEAR_READ_TREE=1 \
  IMR_LSM_YCSB_COMPARE_READ_TREE_LIMIT=4096 \
  IMR_LSM_YCSB_COMPARE_BLOOM_BITS_PER_KEY=10 \
  IMR_LSM_YCSB_COMPARE_ZONE_GC_MIN_INVALID_RATIO_PERMILLE=250 \
  IMR_LSM_YCSB_COMPARE_ZONE_GC_FREE_LOW_WATERMARK=3 \
  IMR_LSM_YCSB_COMPARE_ZONE_COMPACTION_AUTO_RUN=1 \
  IMR_LSM_YCSB_MKFS_BLOCK_SIZE=4096 \
  IMR_LSM_YCSB_MKFS_BLOCKS=524288 \
  IMR_LSM_YCSB_MOUNT_OPTIONS=noatime,nodiratime \
  IMR_LSM_YCSB_FIELD_COUNT=10 \
  IMR_LSM_YCSB_FIELD_LENGTH=100 \
  IMR_LSM_YCSB_FSTRIM=0 \
  bash tests/imr_lsm_ycsb_workloads_comparison_test.sh /dev/sdb
```

The mapper policy controls are applied and read back after every fresh mapper
creation. Their configured values, along with the filesystem and cache
settings, are recorded in the top-level `manifest.txt`. Use a new result path
for every invocation.

Set `IMR_LSM_YCSB_COMPARE_REPETITIONS=1` for a single pilot round. The result
directory contains raw artifacts for every run, `runs.csv`, per-workload
`medians.csv`, and a compact `comparison.md`. Latency includes average, P95,
P99, and maximum YCSB latency; throughput is reported both as YCSB run
throughput and as durable throughput including final sync and IMR-LSM
compaction drain. The read metric is READ for A-D/F and SCAN for E. The write
metric is UPDATE for A/B, INSERT for D/E, and READ-MODIFY-WRITE for F. Workload
C has no write operation, so its write-latency fields are `NA`.

Three write-amplification categories are kept separate. `imr_wa` uses the
device simulator counters and is calculated as
`write_total_delta / (write_total_delta - extra_write_total_delta)`.
`metadata_wa` measures IMR-LSM metadata rewriting and is calculated as
`(ingested_metadata_bytes + metadata_compaction_output_bytes) /
ingested_metadata_bytes`, where every mapping record is 32 bytes. A phase with
no host or metadata writes reports `NA`, rather than zero.

Zone compaction copies payload blocks directly to the backing device, so those
writes are not included in the simulator's `write_total` counter. The
comparison runner therefore also reports `zone_gc_copy_overhead` as
`zone_compaction_copied_entries_delta / host_write_delta` and `zone_gc_wa` as
`(host_write_delta + zone_compaction_copied_entries_delta) /
host_write_delta`. `zone_cleaning_wa` is the victim-cleaning view,
`zone_compaction_input_entries_delta /
zone_compaction_skipped_entries_delta`; it reports `NA` when no invalid entry
was reclaimed. Physical copy cost uses copied rather than committed entries
because a foreground update can win after the copy was already written.

The single-workload runner can save the same IMR device snapshots when
`IMR_LSM_YCSB_CAPTURE_DEVICE_STATS=1` is set. These phase counters include the
YCSB process's RocksDB open/cleanup, final sync, and the following IMR-LSM
compaction drain.

Reset the persistence area as well as recreating the mapper before every formal
threshold run. For a 79-zone mapper backed by `/dev/sdb`, use:

```bash
sudo dmsetup remove imrsim
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  bash imrsim_util/imr_format.sh -i -d /dev/sdb
echo "0 $((79*524288)) imrsim /dev/sdb 0" | sudo dmsetup create imrsim
sudo grep -E \
  '^(logical_write_count|lsm_record_insert_count|compaction_count|read_tree_size|newest_index_size|newest_index_valid|newest_index_update_fail_count|newest_index_fallback_count|invalid_recalc_count):' \
  /sys/kernel/debug/imrsim_lsm/stats
```

Before YCSB starts, the counters in that check must be zero except
`newest_index_valid`, which must be one. A newly initialized
mapper can report `initialized: 1`; this only means its empty in-memory metadata
has been constructed and is not evidence of persisted workload state. The
runner rejects persisted workload metadata by default so an old zone state
cannot make `mkfs.ext4` fail with an out-of-policy short write.
`IMR_LSM_YCSB_ALLOW_DIRTY=1` bypasses this guard for diagnostics only; do not
use it for comparable benchmark runs.

Each load and run phase reports the complete process wall time, DB startup/open
time (up to the YCSB `DBWrapper` ready message), post-open operations plus
cleanup time, post-open effective throughput (including cleanup), YCSB overall
and last-progress-interval throughput, the final `sync` time, and the following
background level-compaction drain time. The post-mount setup drain keeps
filesystem-format compactions out of the load-phase counter baseline. Keep
these values separate: cold-open, final writeback, and background compaction
can dominate small workloads even when steady operations are fast.

For example, the KVIMR paper-style mixed phases can be approximated with:

```bash
sudo env IMR_LSM_TEST_DESTRUCTIVE=1 \
  IMR_LSM_YCSB_HOME=/path/to/YCSB \
  IMR_LSM_YCSB_WORKLOAD=workloada \
  IMR_LSM_YCSB_RECORD_COUNT=75000000 \
  IMR_LSM_YCSB_OPERATION_COUNT=7500000 \
  IMR_LSM_YCSB_READ_PROPORTION=0.9 \
  IMR_LSM_YCSB_UPDATE_PROPORTION=0.1 \
  IMR_LSM_YCSB_INSERT_PROPORTION=0 \
  IMR_LSM_YCSB_SCAN_PROPORTION=0 \
  bash tests/imr_lsm_ycsb_rocksdb_workload_test.sh /dev/mapper/imrsim
```

Repeat with `READ_PROPORTION/UPDATE_PROPORTION` set to `0.5/0.5` and `0.1/0.9`
for the other mixed workload points. Increase `IMR_LSM_YCSB_MKFS_BLOCKS` and
the mapper size before paper-scale runs.

To also sweep different temporary device sizes / zone counts, first remove any
active `imrsim` target because the module supports a single mapped target, then
run for example. This mode creates, owns, and cleans up its temporary backing
device, so it does not require the destructive opt-in used for an existing
mapper:

```bash
sudo env IMR_LSM_SWEEP_DEVICE_ZONES="3 5 8" \
  tests/imr_lsm_parameter_sweep_test.sh
```

Delete follows LSM-style tombstone semantics rather than in-place invalid
marking. A delete appends a metadata entry with `valid=0`, updates the read
tree to expose that tombstone, and leaves old physical payloads untouched.
During reads, the newest tombstone masks older live records and prevents
fallback to stale disk data. Later compaction treats obsolete live entries and
unneeded tombstones as invalid metadata and drops them when they no longer
protect an older version.

### Function Testing

The user interface program `imrsim_util.c` provide a tool, `imrsim_util`, can be used to test the interface function. The command format accepted by this program is as follows:

```bash
$ ./imrsim_util /dev/mapper/<dm_device_name> <code> <seq> <arg>
```

- <dm_device_name> is the built logical device name and should be filled in `imrsim`.
- \<code> identifies the functional category.
- \<seq> identifies the specific type of a certain type of function.
- \<arg> is an optional parameter.

Different functional categories can be divided according to *code*, which are represented by a single character, including `e` (identifying error report), `z` (identifying zone status), `s` (identifying zone statistics), and `l` (identifying configuration related). 

*seq* represents the specific type of a certain type of function, and uses numbers to indicate the number of function options of a certain type; and *arg* is some parameters (0 or 1) required by the function.

| function name            | code | seq  | function description                                      |
| ------------------------ | ---- | ---- | --------------------------------------------------------- |
| show last read error     | e    | 1    | Display the last read error                               |
| show last write error    | e    | 2    | Display the last write error                              |
| enable logging           | e    | 3    | Enable error log function                                 |
| disable logging          | e    | 4    | Turn off error logging                                    |
| get number of zones      | z    | 1    | Get how many zones are in the device                      |
| get default zone size    | z    | 2    | Get the default size of a zone                            |
| set default zone size    | z    | 3    | Set the default size of a zone                            |
| reset zone status        | z    | 4-5  | Reset the status of a zone                                |
| query zone status        | z    | 6-8  | Query the status of a zone                                |
| get all zone stats       | s    | 1    | Get statistics for all zones                              |
| get zone stats           | s    | 2    | Get statistics for multiple zones                         |
| get zone stats by idx    | s    | 3    | Get statistics for a certain zone                         |
| reset all zone stats     | s    | 4    | Reset all zone statistics                                 |
| reset zone stats by lba  | s    | 5    | Reset the statistics of the zone where the lba is located |
| reset zone stats by idx  | s    | 6    | Reset statistics for a zone                               |
| set all default config   | l    | 1    | Reset all default configurations                          |
| set zone default config  | l    | 2    | Reset the configuration of all zones                      |
| reset dev default config | l    | 3    | Reset device configuration                                |
| get dev config           | l    | 4    | Get device configuration                                  |
| set read penalty delay   | l    | 5    | Set read delay                                            |
| set write penalty delay  | l    | 6    | Set write delay                                           |

### Performance Testing

Taking the performance test of the stress test tool fio as an example, the test indicators are: IOPS, bandwidth and delay.

（1）4K sequential write test on 20GB IMRSim simulator:

```bash
$ fio --filename=/dev/mapper/imrsim --iodepth=128 --ioengine=libaio --direct=1 --rw=write --bs=4k --size=20g --numjobs=1 --runtime=3000 --group_reporting --name=test-write
```

（2）4K random write test on 20GB IMRSim simulator:

```bash
$ fio --filename=/dev/mapper/imrsim --iodepth=128 --ioengine=libaio --direct=1 --rw=randwrite --bs=4k --size=20g --numjobs=1 --runtime=3000 --group_reporting --name=test-rand-write
```

*iodepth* can be adjusted. By expanding the value of *iodepth*, the hard disk utilization can be increased to obtain the peak performance.



## Security

The project passed the security inspection of CNNVD and was included by the Open Source Security Community (OSCS).

[![Security Status](./SECURITY.svg)](https://www.murphysec.com/accept?code=8701786a3068b7a168c90d796e96a43c&type=1&from=2&t=2)



## Citation

If you found this work useful for you, please consider citing it.

```txt
@InProceedings{10.1007/978-3-031-21395-3_25,
author="Zeng, Zhimin
and Chen, Xinyu
and Yang, Laurence T.
and Cui, Jinhua",
title="IMRSim: A Disk Simulator for Interlaced Magnetic Recording Technology",
booktitle="Network and Parallel Computing",
year="2022",
publisher="Springer Nature Switzerland",
address="Cham",
pages="267--273",
isbn="978-3-031-21395-3"
}
```



## Contact us

For any issues/questions regarding the paper or simulator, please contact any of the following.

Zeng zhimin  (Email: im_zzm@126.com)
