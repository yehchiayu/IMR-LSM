# IMR-LSM Runtime Correctness Test Report

Draft status: environment captured; all controlled shell-script evidence
captured on the default 79-zone VM mapper. Real workload and persistence
evidence remain future work.

This report tracks runtime validation for the IMR-LSM metadata layer added to
IMRSim. The scope is functional correctness and debug validation, not a
production-readiness claim for every workload or crash/reload corner case.

## Scope

The validation target is the IMR-LSM runtime metadata path:

- Multi-block write metadata recording.
- Append-only insert into unsorted metadata.
- Read path ordering through read tree, unsorted metadata, segment Bloom filter,
  block table, sorted metadata, and disk fallback.
- Delete through LSM-style tombstones.
- Discard/TRIM translation into range delete.
- RocksDB-style dynamic level bytes adapted as dynamic level entries.
- Level and segment compaction behavior.
- Zone compaction behavior for full top+bottom zone metadata.
- Debugfs tunables used for validation sweeps.

## Test Environment

Environment captured from the Ubuntu VM:

```text
Host: Teresa
Kernel: Linux Teresa 3.16.0-23-generic #31-Ubuntu SMP Tue Oct 21 17:56:17 UTC 2014 x86_64 x86_64 x86_64 GNU/Linux
Distribution: Ubuntu 14.10 (utopic)
GCC: gcc (Ubuntu 4.9.1-16ubuntu6) 4.9.1
Make: GNU Make 4.0
```

Known host limitation:

- The local macOS development machine cannot build or load the Linux kernel
  module. Build, load, mapper creation, debugfs validation, and block-device
  tests must be performed inside the Ubuntu VM or another compatible Linux
  environment.

## Device Configuration

The validation VM uses a direct block device rather than a loop device:

```text
Backing device: /dev/sdb
Mapper name: imrsim
Mapper target: imrsim
Logical sectors: 41,418,752
Zone count: 79
Zone size: 524,288 sectors = 256 MiB
Mapped capacity: 79 * 256 MiB = 19.75 GiB
Debugfs path: /sys/kernel/debug/imrsim_lsm
```

Observed mapper evidence:

```text
sudo dmsetup ls:
imrsim (252:0)

sudo dmsetup table imrsim:
0 41418752 imrsim 8:16 0

sudo blockdev --getsz /dev/mapper/imrsim:
41418752
```

Initial debugfs stats before the validation tests:

```text
initialized: 0
active_write_level: 6
dynamic_base_level: 6
lowest_unnecessary_level: -1
insert_target: L6 unsorted
compact_target: next dynamic level
logical_write_count: 0
lsm_record_insert_count: 0
lsm_write_count: 0
delete_count: 0
read_lookup_count: 2
read_miss_count: 2
segment_lookup_count: 0
segment_skip_count: 0
segment_candidate_count: 0
bloom_lookup_count: 0
bloom_negative_count: 0
bloom_maybe_count: 0
block_table_lookup_count: 0
block_table_hit_count: 0
block_table_miss_count: 0
read_tree_capacity: 4096
read_tree_size: 0
read_tree_lookup_count: 0
read_tree_hit_count: 0
read_tree_miss_count: 0
read_tree_update_count: 0
read_tree_update_fail_count: 0
read_tree_remove_count: 0
read_tree_evict_count: 0
```

Device setup procedure used in the VM:

```bash
sudo mount -t debugfs none /sys/kernel/debug
sudo dmsetup remove imrsim
sudo dmesg -C
make clean
make
sudo make install
sudo depmod --quick
sudo modprobe -r dm-imrsim
sudo modprobe dm-imrsim
echo "0 $((79*524288)) imrsim /dev/sdb 0" | sudo dmsetup create imrsim
```

Follow-up evidence to collect from the VM:

```bash
sudo dmsetup ls
sudo dmsetup table imrsim
sudo blockdev --getsz /dev/mapper/imrsim
sudo head -40 /sys/kernel/debug/imrsim_lsm/stats
```

Observed collection issue:

```text
Running dmsetup/debugfs reads without sudo failed with permission denied.
Running blockdev before creating /dev/mapper/imrsim failed with:
No such file or directory.
```

## Test Matrix

| Test | Script | Status | Evidence |
| --- | --- | --- | --- |
| Automatic threshold compaction and readback | `tests/imr_lsm_auto_compaction_readback_test.sh` | PASS | VM verified threshold compaction, segment/block-table metadata, no fallback readback |
| Read path ordering and tombstone masking | `tests/imr_lsm_read_path_order_test.sh` | PASS | VM verified tree -> unsorted -> segment/Bloom/block-table -> tombstone -> disk fallback order |
| Delete, rewrite, overwrite-delete, and segment compaction | `tests/imr_lsm_delete_tombstone_compaction_test.sh` | PASS | VM verified delete masking, rewrite-after-delete, overwrite-delete, and segment compaction safety |
| Discard/TRIM range delete | `tests/imr_lsm_discard_range_delete_test.sh` | PASS | VM verified `blkdiscard` reaches the dm target, creates tombstones, and preserves the out-of-range neighbor |
| RocksDB-style dynamic level entries | `tests/imr_lsm_dynamic_level_entries_test.sh` | PASS | VM output captured below |
| Zone-level compaction | `tests/imr_lsm_zone_compaction_test.sh` | PASS | VM verified full-zone metadata compaction into two bottom-track regions |
| Zone compaction with tombstone skipping | `tests/imr_lsm_zone_tombstone_compaction_test.sh` | PASS | VM verified tombstone skip, live-key preservation, and bottom-track expansion |
| Parameter sweep for read tree, write size, compaction threshold, and Bloom sizing | `tests/imr_lsm_parameter_sweep_test.sh` | PASS | VM verified default 79-zone mapper sweep; optional zone-count sweep not run |

Suggested run order:

```bash
sudo tests/imr_lsm_dynamic_level_entries_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_auto_compaction_readback_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_read_path_order_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_delete_tombstone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_discard_range_delete_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_zone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_zone_tombstone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_parameter_sweep_test.sh /dev/mapper/imrsim
```

`tests/imr_lsm_dynamic_level_entries_test.sh` should be run first after mapper
creation because it asserts an exact fresh-metadata distribution. Recreate the
mapper before running it if prior writes have already changed IMR-LSM metadata.
An initial run passed the level, compaction, count, and target checks, then
failed only because the script expected two L6 logical segments while the
observed run-history-preserving metadata reported three. The script was updated
to expect three logical segments, matching the three flush/compaction events.

`tests/imr_lsm_discard_range_delete_test.sh` requires the dm target to advertise
discard support. The target was updated to set `ti->discards_supported = 1` so
discard bios are delivered to IMR-LSM even when the backing `/dev/sdb` does not
advertise native discard support. A follow-up update also exposes virtual
discard queue limits through the target `io_hints` callback and adds
`discard_bio_count`, `discard_delete_count`, `discard_delete_failed_count`,
`last_discard_lba`, `last_discard_sectors`, and `last_discard_error` to
debugfs stats for direct discard-path observability.

Optional zone-count sweep:

```bash
sudo IMR_LSM_SWEEP_DEVICE_ZONES="3 5 8" tests/imr_lsm_parameter_sweep_test.sh
```

## Manual Progress Classification

Earlier manual validation notes were reviewed and classified as follows:

```text
Covered by existing automated scripts:
- Logical segment metadata and segment/block-table readback.
- Automatic threshold compaction and readback.
- Read path order: tree, unsorted, segment/Bloom/block_table, tombstone, fallback.
- Delete/tombstone correctness before and after segment compaction.
- Zone-level compaction and zone compaction with tombstone skipping.
- Read-tree capacity/LRU behavior and parameter sweep tunables.

Newly automated in this report update:
- Discard/TRIM range delete.
- RocksDB-style dynamic level entries.

Still best treated as manual or future tests:
- commit_output mode 1/2/3 end-to-end physical-copy workflow.
- Detailed compaction-policy scoring and selection weights.
- Filesystem workload: mkfs.ext4, mount, file create/update/delete, fstrim.
- RocksDB real put/update/delete/readback workload.
- Persistence/reload readback after mapper recreation or module reload.
- Concurrent fio mixed read/write/delete workload.
```

## Validation Summary

### 1. Multi-block bio write metadata

Expected coverage:

- 4KB, 8KB, 16KB, and 64KB writes are recorded as per-block IMR-LSM metadata
  entries.
- Each logical block maps to the expected latest physical block.
- Readback after multi-block writes returns the expected payload.

Evidence source:

- `tests/imr_lsm_parameter_sweep_test.sh`

Status:

- PASS on Ubuntu VM. The parameter sweep validated 4KB, 8KB, 16KB, and 64KB
  writes, with per-block insert accounting and no stale fallback during
  readback.

Write-size sweep VM evidence:

```text
device=/dev/mapper/imrsim zones=79 test_zone=2 key_offset=32768
PASS: 4096-byte write records 1 keys: lsm_record_insert_count +1
PASS: 4096-byte write readback: no stale fallback: fallback_count +0
PASS: 8192-byte write records 2 keys: lsm_record_insert_count +2
PASS: 8192-byte write readback: no stale fallback: fallback_count +0
PASS: 16384-byte write records 4 keys: lsm_record_insert_count +4
PASS: 16384-byte write readback: no stale fallback: fallback_count +0
PASS: 65536-byte write records 16 keys: lsm_record_insert_count +16
PASS: 65536-byte write readback: no stale fallback: fallback_count +0
```

### 2. Delete API and tombstone semantics

Expected coverage:

- `delete_key` appends a `valid=0` tombstone instead of erasing payloads in
  place.
- A newer tombstone hides older live records.
- A later write after delete becomes visible again.
- Segment compaction drops obsolete live entries and keeps tombstone semantics
  correct.

Evidence source:

- `tests/imr_lsm_delete_tombstone_compaction_test.sh`
- `tests/imr_lsm_read_path_order_test.sh`

Status:

- PASS on Ubuntu VM. The automated script validated delete masking before and
  after segment compaction, rewrite-after-delete visibility, overwrite-then-delete
  masking, unrelated live-key preservation, and deleted-key metadata safety after
  compaction.

Delete/tombstone VM evidence:

```text
base_key=1024
keys: delete=1024 delete_rewrite=1025 overwrite_delete=1026 keep=1027

Before segment compaction:
PASS: write A -> delete hides old A before segment compaction
PASS: write A -> delete -> write B reads latest B before segment compaction
PASS: write A -> write B -> delete hides old A/B before segment compaction
PASS: unrelated live key survives before segment compaction

Segment compaction:
compact_segment round=1 candidates=2 score=4250
compact_segment round=2 candidates=1 score=5000
compact_segment round=3 candidates=1 score=1000
segment compaction idle after 3 round(s)

After segment compaction:
PASS: write A -> delete still hides old A after segment compaction
PASS: write A -> delete -> write B still reads latest B after segment compaction
PASS: write A -> write B -> delete still hides old A/B after segment compaction
PASS: unrelated live key survives after segment compaction
PASS: deleted key has no copied stale metadata after segment compaction
PASS: overwrite-then-delete key has no copied stale metadata after segment compaction
PASS: compact executions delta=3, physical copy entries delta=4
```

### 3. Discard/TRIM as range delete

Expected coverage:

- `REQ_OP_DISCARD` on the mapped device is translated into IMR-LSM range delete.
- Deleted blocks do not fall back to stale physical payloads.
- The discard path does not pass the discard bio to the backing device after
  metadata deletion succeeds.

Evidence source:

- `tests/imr_lsm_discard_range_delete_test.sh`
- Earlier manual validation also used `blkdiscard -o $((KEY * 4096)) -l 4096`
  and observed `delete_count` increasing by one.

Status:

- PASS on Ubuntu VM. The automated script validates the range discard path,
  tombstone masking, neighbor preservation, rewrite-after-discard, and a second
  single-block discard.

VM discard capability observation:

```text
/sys/block/dm-0/queue/discard_max_bytes: 2147450880
/sys/block/dm-0/queue/discard_granularity: 512
```

Before the final fix, the VM could advertise discard capability while still
missing the IMR-LSM discard path:

```text
range discard records tombstones: delete_count delta expected 3, got 0
```

That failure occurred before the VM was rebuilt with the implementation that
exposes discard counters and handles both possible old-kernel paths:

- discard bio path through `imrsim_map()`
- `BLKDISCARD` ioctl path through `imrsim_ioctl()`

Final VM evidence:

```text
Initial discard stats:
discard_bio_count: 0
discard_delete_count: 0
discard_delete_failed_count: 0
last_discard_lba: 0
last_discard_sectors: 0
last_discard_error: 0

Range discard, first_key=200704, blocks=3:
PASS: blkdiscard reaches dm target: discard_bio_count +1
PASS: blkdiscard completes metadata range delete: discard_delete_count +1
PASS: range discard records tombstones: delete_count +3
PASS: discarded key=200704 hides old payload
PASS: discarded key=200705 hides old payload
PASS: discarded key=200706 hides old payload
PASS: discarded reads observe tombstone path: tombstone_hit_count +3 >= 1
PASS: neighbor key=200707 survives discard range

Rewrite key=200705, then single-block discard:
PASS: rewrite after discard is readable
PASS: single-block discard after rewrite: delete_count +1
PASS: single-block discard hides rewritten payload
PASS: discard/TRIM range delete hides stale payloads and preserves out-of-range data

Final discard stats:
discard_bio_count: 2
discard_delete_count: 2
discard_delete_failed_count: 0
last_discard_lba: 1605640
last_discard_sectors: 8
last_discard_error: 0
```

The expected read result for discarded blocks is either a rejected read or a
metadata miss that does not return stale payload. The observed `dd` read errors
after tombstone insertion are therefore accepted by this test.

### 4. Read path ordering

Expected coverage:

- Read tree hit returns the latest mapping.
- Read tree miss checks unsorted metadata.
- Segment lookup uses key range, Bloom filter, and block table.
- Tombstone records prevent stale disk fallback.
- Disk fallback is used only when no newer IMR-LSM metadata exists.

Evidence source:

- `tests/imr_lsm_read_path_order_test.sh`

Status:

- PASS on Ubuntu VM. The automated script validated read tree hits, tree misses
  falling through to unsorted metadata, segment Bloom/block-table lookup,
  tombstone masking, and disk fallback only after IMR-LSM misses.

Read-path VM evidence:

```text
zone=1
keys: tree=65552 unsorted=65553 segment=65554 delete=65555 fallback=67584

Tree hit:
PASS: tree index hit returns the latest write
PASS: tree hit short-circuits read path: tree_hit_count +1
PASS: read tree lookup hit: read_tree_hit_count +1
PASS: tree hit does not fall through to unsorted: unsorted_hit_count +0
PASS: last_read_tree_key=65552
PASS: last_read_tree_hit=1
PASS: last_read_tree_valid=1

Unsorted hit after tree miss:
PASS: tree miss falls through to unsorted list
PASS: unsorted case tree miss: read_tree_miss_count +1
PASS: unsorted list hit: unsorted_hit_count +1
PASS: unsorted latest record wins before segment: segment_hit_count +0
PASS: unsorted hit avoids disk fallback: fallback_count +0

Segment hit after tree/unsorted miss:
PASS: tree miss falls through to segment bloom/block_table
PASS: segment case tree miss: read_tree_miss_count +1
PASS: segment case skips unsorted source: unsorted_hit_count +0
PASS: segment block_table hit: segment_hit_count +1
PASS: block_table confirms segment hit: block_table_hit_count +1 >= 1
PASS: segment hit avoids disk fallback: fallback_count +0
PASS: last_block_table_hit_key=65554
PASS: last_block_table_hit_valid=1
PASS: last_segment_lookup_count=1 >= 1
PASS: last_bloom_lookup_count=1 >= 1
PASS: last_block_table_lookup_count=1 >= 1

Tombstone masking:
PASS: tree tombstone hides older segment payload
PASS: tombstone is served by read tree: read_tree_hit_count +1
PASS: tree tombstone counted: tombstone_hit_count +1
PASS: tombstone avoids disk fallback: fallback_count +0
PASS: last_read_tree_key=65555
PASS: last_read_tree_hit=1
PASS: last_read_tree_valid=0

Disk fallback after LSM miss:
PASS: fallback case tree miss: read_tree_miss_count +1
PASS: fallback has no unsorted LSM hit: unsorted_hit_count +0
PASS: fallback has no segment LSM hit: segment_hit_count +0
PASS: fallback is not a tombstone: tombstone_hit_count +0
PASS: LSM miss before disk fallback: read_miss_count +1
PASS: disk fallback path reached: fallback_count +1
PASS: read path order validated: tree -> unsorted -> segment/bloom/block_table -> tombstone masking -> disk fallback
```

### 5. Compaction and placement

Expected coverage:

- Threshold-triggered compaction flushes unsorted entries into segment metadata.
- Segment compaction removes obsolete entries and preserves latest records.
- Zone compaction expands one full top+bottom zone into bottom-track regions.
- Tombstoned records are skipped during zone compaction.

Evidence source:

- `tests/imr_lsm_auto_compaction_readback_test.sh`
- `tests/imr_lsm_delete_tombstone_compaction_test.sh`
- `tests/imr_lsm_zone_compaction_test.sh`
- `tests/imr_lsm_zone_tombstone_compaction_test.sh`

Status:

- PASS on Ubuntu VM for the controlled compaction cases: threshold-triggered
  segment creation and readback, selected segment compaction with obsolete/delete
  records, zone-level compaction without tombstones, and zone compaction with
  tombstone skipping.

Auto-compaction VM evidence:

```text
zone=2 first_key=135168 writes=20 threshold=16
PASS: normal insert records: lsm_record_insert_count +20
PASS: automatic threshold compaction: compaction_count +2 >= 1
PASS: segment_count 0 -> 2
PASS: block_table entries in test range=17
PASS: read back uses LSM mappings: fallback_count +0
PASS: normal inserts auto-compacted into segment/block_table and all payloads read back correctly
```

Segment compaction VM evidence:

```text
compact_segment round=1 candidates=2 score=4250
compact_segment round=2 candidates=1 score=5000
compact_segment round=3 candidates=1 score=1000
segment compaction idle after 3 round(s)
PASS: compact executions delta=3, physical copy entries delta=4
PASS: deleted-key metadata is not copied as stale live payload
```

Zone-level compaction VM evidence:

```text
source_zone=0
seed full-zone metadata for VM/debug validation
keys: bottom_first=0 bottom_last=36351 top_first=36352 top_last=65535

PASS: before compact bottom first marker
PASS: before compact bottom last marker
PASS: before compact top first marker
PASS: before compact top last marker
PASS: zone_compaction_count 0 -> 1
PASS: last_zone_compaction_source_zone=0
PASS: last_zone_compaction_dest_zone0=0
PASS: last_zone_compaction_dest_zone1=1
PASS: last_zone_compaction_input_entries=65536
PASS: last_zone_compaction_live_entries=65536
PASS: last_zone_compaction_skipped_entries=0
PASS: last_zone_compaction_failed_entries=0
PASS: last_zone_compaction_error=0
PASS: last_zone_compaction_copied_entries=29184
PASS: after compact bottom first marker
PASS: after compact bottom last marker
PASS: after compact top first marker
PASS: after compact top last marker
PASS: key 0 -> zone 0 bottom pba=3648 zone=0 zone_block=456
PASS: key 36351 -> zone 0 bottom pba=524280 zone=0 zone_block=65535
PASS: key 36352 -> zone 1 bottom pba=527936 zone=1 zone_block=456
PASS: key 65535 -> zone 1 bottom pba=947448 zone=1 zone_block=52895
PASS: zone-level compaction expanded one full top+bottom zone into two bottom-track regions
```

Zone compaction with tombstone skipping VM evidence:

```text
source_zone=2
seed full-zone metadata for VM/debug validation
keys: live_bottom=131072 live_top=167424 delete_top=196607

PASS: deleted top key hides old payload before zone compaction
PASS: live bottom key reads before zone compaction
PASS: live top key reads before zone compaction
PASS: zone_compaction_count 0 -> 1
PASS: last_zone_compaction_source_zone=2
PASS: last_zone_compaction_dest_zone0=2
PASS: last_zone_compaction_dest_zone1=3
PASS: last_zone_compaction_input_entries=65536
PASS: last_zone_compaction_live_entries=65535
PASS: last_zone_compaction_skipped_entries=1
PASS: last_zone_compaction_failed_entries=0
PASS: last_zone_compaction_error=0
PASS: deleted top key hides old payload after zone compaction
PASS: live bottom key survives zone compaction
PASS: live top key survives zone compaction
PASS: deleted top key is not moved into compacted block_table
PASS: live bottom key remains in zone 2 bottom pba=1052224 zone=2 zone_block=456
PASS: live top key expands to zone 3 bottom pba=1576512 zone=3 zone_block=456
PASS: zone compaction skips tombstones, preserves live data, and hides stale payloads
```

### 6. RocksDB-style dynamic level entries

Expected coverage:

- The implementation adapts RocksDB dynamic level bytes into dynamic level
  entries, using metadata node counts rather than SST file bytes.
- Starting from a fresh mapper, initial writes go to L6.
- After 40 4KB writes, the active write level moves to L5.
- After another 40 4KB writes, the active write level moves to L4.
- Dynamic targets scale by the configured ratio while respecting the minimum
  compaction threshold.

Manual evidence already observed:

```text
After 40 4KB writes:
active_write_level: 5
dynamic_base_level: 5
insert_target: L5 unsorted
L6 sorted_count: 33
L6 unsorted_count: 0
L5 unsorted_count: 7
compaction_count: 3
last_compaction_from: L5
last_compaction_to: L6
last_compaction_input: 16
last_compaction_output_total: 33
L6 dynamic_target: 32
```

```text
After a second 40 4KB writes at seek=40:
active_write_level: 4
dynamic_base_level: 4
insert_target: L4 unsorted
L6 sorted_count: 49
L5 sorted_count: 16
L4 unsorted_count: 15
compaction_count: 5
last_compaction_from: L4
last_compaction_to: L5
last_compaction_input: 16
last_compaction_output_total: 16
L4 dynamic_target: 16
L5 dynamic_target: 24
L6 dynamic_target: 48
```

Interpretation:

- The first batch moves data into L6 and then shifts the dynamic base level from
  L6 to L5 as L6 accumulates entries.
- The second batch shifts the dynamic base level from L5 to L4 as the lower
  levels grow.
- With `IMR_LSM_COMPACTION_THRESHOLD=16`, `IMR_LSM_LEVEL_RATIO=2`, and seven
  levels L0 through L6, the 80-entry state produces targets L4=16, L5=24, and
  L6=48, matching the observed metadata.
- L6 can temporarily exceed its dynamic target because it is the bottom level
  and has no lower level to compact into.

Evidence source:

- Manual RocksDB/dynamic-level run.
- `tests/imr_lsm_dynamic_level_entries_test.sh`

Status:

- PASS on Ubuntu VM. The automated script validated `active_write_level` and
  `dynamic_base_level` moving L6 -> L5 -> L4, expected compaction counters,
  L4/L5/L6 metadata distribution, and readback for keys 0, 39, 40, and 79.

### 7. Debugfs validation tunables

Expected coverage:

- `read_tree_limit` can temporarily lower read-tree capacity and trigger LRU
  eviction.
- `compaction_threshold` can be swept for validation.
- `bloom_bits_per_key` can be swept for future segment Bloom filters.
- Writing `0` restores each tunable to its default where supported.

Evidence source:

- `tests/imr_lsm_parameter_sweep_test.sh`

Status:

- PASS on Ubuntu VM for the default 79-zone mapper. The automated sweep covered
  `read_tree_limit`, `compaction_threshold`, and `bloom_bits_per_key`. The
  optional `IMR_LSM_SWEEP_DEVICE_ZONES` loop-device sweep was not run.

Parameter-sweep VM evidence:

```text
read_tree_limit:
PASS: read_tree_limit=1 eviction: read_tree_evict_count +2 >= 1
PASS: read_tree_limit=1 tree_size=1
PASS: read_tree_limit=2 eviction: read_tree_evict_count +2 >= 1
PASS: read_tree_limit=2 tree_size=2
PASS: read_tree_limit=8 eviction: read_tree_evict_count +2 >= 1
PASS: read_tree_limit=8 tree_size=8
PASS: read_tree_limit=4096 tree_size=3

compaction_threshold:
PASS: threshold=8 insert accounting: lsm_record_insert_count +10
PASS: threshold=8 auto compaction: compaction_count +1 >= 1
PASS: threshold=8 readback: no stale fallback: fallback_count +0
PASS: threshold=16 insert accounting: lsm_record_insert_count +18
PASS: threshold=16 auto compaction: compaction_count +1 >= 1
PASS: threshold=16 readback: no stale fallback: fallback_count +0
PASS: threshold=32 insert accounting: lsm_record_insert_count +34
PASS: threshold=32 auto compaction: compaction_count +1 >= 1
PASS: threshold=32 readback: no stale fallback: fallback_count +0

bloom_bits_per_key:
PASS: bloom_bits_per_key=4 segment build: compaction_count +1 >= 1
PASS: bloom_bits_per_key=4 readback: no stale fallback: fallback_count +0
PASS: bloom_bits_per_key=4 new_segments=1
PASS: bloom_bits_per_key=10 segment build: compaction_count +2 >= 1
PASS: bloom_bits_per_key=10 readback: no stale fallback: fallback_count +0
PASS: bloom_bits_per_key=10 new_segments=2
PASS: bloom_bits_per_key=16 segment build: compaction_count +1 >= 1
PASS: bloom_bits_per_key=16 readback: no stale fallback: fallback_count +0
PASS: bloom_bits_per_key=16 new_segments=1

PASS: parameter sweep completed for /dev/mapper/imrsim
PASS: all parameter sweep cases completed
```

## Counter Evidence

The following per-test counter deltas are the primary evidence. A full
post-run snapshot can still be captured as optional supplementary material:

```bash
sudo cat /sys/kernel/debug/imrsim_lsm/stats
sudo cat /sys/kernel/debug/imrsim_lsm/segments
sudo head -80 /sys/kernel/debug/imrsim_lsm/block_table
```

Observed discard/TRIM counter evidence from
`tests/imr_lsm_discard_range_delete_test.sh`:

```text
discard_bio_count: 0 -> 2
discard_delete_count: 0 -> 2
discard_delete_failed_count: 0
delete_count: +3 for range discard, then +1 for single-block discard
tombstone_hit_count: +3 or greater during discarded read checks
last_discard_lba: 1605640
last_discard_sectors: 8
last_discard_error: 0
```

Observed auto-compaction counter evidence from
`tests/imr_lsm_auto_compaction_readback_test.sh`:

```text
lsm_record_insert_count: +20
compaction_count: +2 or greater
segment_count: 0 -> 2
block_table entries in test range: 17
fallback_count: +0 during readback
```

Observed read-path counter evidence from
`tests/imr_lsm_read_path_order_test.sh`:

```text
tree_hit_count: +1 for latest write tree hit
read_tree_hit_count: +1 for tree hit, +1 for tombstone hit
read_tree_miss_count: +1 unsorted case, +1 segment case, +1 fallback case
unsorted_hit_count: +1 only for unsorted case
segment_hit_count: +1 only for segment/block-table case
block_table_hit_count: +1 or greater for segment case
tombstone_hit_count: +1 for tombstone masking
read_miss_count: +1 before disk fallback
fallback_count: +1 only for explicit fallback case
```

Observed delete/tombstone and segment-compaction counter evidence from
`tests/imr_lsm_delete_tombstone_compaction_test.sh`:

```text
segment_compaction_execute_count: +3
segment_output_physical_copy_entry_count: +4
segment_output_physical_copy_failed_count: +0
deleted-key stale reads: rejected or missed deleted key before and after compaction
rewrite-after-delete readback: latest B payload returned before and after compaction
obsolete tombstone metadata: safely dropped or kept metadata-only, never copied/mapped as live payload
```

Observed zone-level compaction counter evidence from
`tests/imr_lsm_zone_compaction_test.sh`:

```text
zone_compaction_count: 0 -> 1
last_zone_compaction_source_zone: 0
last_zone_compaction_dest_zone0: 0
last_zone_compaction_dest_zone1: 1
last_zone_compaction_input_entries: 65536
last_zone_compaction_live_entries: 65536
last_zone_compaction_skipped_entries: 0
last_zone_compaction_failed_entries: 0
last_zone_compaction_error: 0
last_zone_compaction_copied_entries: 29184
bottom marker placement: zone 0 bottom
top marker placement after expansion: zone 1 bottom
```

Observed zone tombstone-compaction counter evidence from
`tests/imr_lsm_zone_tombstone_compaction_test.sh`:

```text
zone_compaction_count: 0 -> 1
last_zone_compaction_source_zone: 2
last_zone_compaction_dest_zone0: 2
last_zone_compaction_dest_zone1: 3
last_zone_compaction_input_entries: 65536
last_zone_compaction_live_entries: 65535
last_zone_compaction_skipped_entries: 1
last_zone_compaction_failed_entries: 0
last_zone_compaction_error: 0
deleted top key: hidden before and after compaction
deleted top key block_table: no active valid compacted entry
live bottom marker placement: zone 2 bottom
live top marker placement after expansion: zone 3 bottom
```

Observed parameter-sweep counter evidence from
`tests/imr_lsm_parameter_sweep_test.sh`:

```text
write size 4096/8192/16384/65536 bytes:
lsm_record_insert_count deltas: +1, +2, +4, +16
fallback_count during readback: +0 for each write size

read_tree_limit 1/2/8/default:
read_tree_evict_count: +2 or greater for limits 1, 2, and 8
tree_size: 1, 2, 8, then 3 after restoring default 4096

compaction_threshold 8/16/32:
lsm_record_insert_count deltas: +10, +18, +34
compaction_count: +1 or greater for each threshold
fallback_count during readback: +0 for each threshold

bloom_bits_per_key 4/10/16:
compaction_count: +1 or greater for each Bloom setting
new_segments: 1, 2, 1
fallback_count during readback: +0 for each Bloom setting
```

## Known Limits

The following items are not yet fully validated by the current controlled test
suite:

- Persistence and reload readback after mapper removal/recreation or module
  unload/reload.
- Filesystem-level workload such as `mkfs.ext4`, mount, create/update/delete
  files, and `fstrim`.
- RocksDB-style put/update/delete/readback workload.
- Concurrent mixed read/write/delete workload with fio.
- Partial-block and non-4KB-aligned bio behavior.
- Zone-full and RMW stress beyond the controlled debugfs validation cases.

The following debugfs controls should be described as validation helpers rather
than production policy controls:

- `seed_full_zone`
- `clear_read_tree`
- `read_tree_limit`
- `compaction_threshold`
- `bloom_bits_per_key`
- `zone_compaction_auto_run`

## Next Evidence To Collect

Collect the following terminal output from the Ubuntu VM and paste it into the
report or into the accompanying review notes:

```bash
uname -a
lsb_release -a
gcc --version | head -1
make --version | head -1

sudo mount -t debugfs none /sys/kernel/debug
sudo dmsetup remove imrsim
sudo dmesg -C
make clean
make
sudo make install
sudo depmod --quick
sudo modprobe -r dm-imrsim
sudo modprobe dm-imrsim
echo "0 $((79*524288)) imrsim /dev/sdb 0" | sudo dmsetup create imrsim

sudo dmsetup ls
sudo dmsetup table imrsim
sudo blockdev --getsz /dev/mapper/imrsim

sudo tests/imr_lsm_dynamic_level_entries_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_auto_compaction_readback_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_read_path_order_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_delete_tombstone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_discard_range_delete_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_zone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_zone_tombstone_compaction_test.sh /dev/mapper/imrsim
sudo tests/imr_lsm_parameter_sweep_test.sh /dev/mapper/imrsim

cat /sys/kernel/debug/imrsim_lsm/stats
cat /sys/kernel/debug/imrsim_lsm/segments
cat /sys/kernel/debug/imrsim_lsm/block_table | head -80
```
