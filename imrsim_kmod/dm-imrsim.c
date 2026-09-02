#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/crc32.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sort.h>
#include <asm/ptrace.h>
#include "imrsim_types.h"
#include "imrsim_ioctl.h"
#include "imrsim_kapi.h"
#include "imrsim_zerror.h"

#ifndef IOCTL_IMRSIM_LSM_DELETE_KEY
#define IOCTL_IMRSIM_LSM_DELETE_KEY          _IOW('d', 1, __u64 *)
#endif

#ifndef READ_ONCE
#define READ_ONCE(x) ACCESS_ONCE(x)
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val)                 \
    do {                                   \
        ACCESS_ONCE(x) = (val);            \
    } while(0)
#endif

/*
 * Device Mapper renamed dm_target::per_bio_data_size to
 * dm_target::per_io_data_size in Linux 4.6.  Keep the allocation size and
 * dm_per_bio_data() lookup tied to the field provided by the build kernel.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
#define IMRSIM_DM_PER_BIO_DATA_SIZE(ti) ((ti)->per_io_data_size)
#else
#define IMRSIM_DM_PER_BIO_DATA_SIZE(ti) ((ti)->per_bio_data_size)
#endif

/*
 The kernel module in the Device Mapper framework is mainly responsible for 
 building the target driver to build the disk structure and function.
*/

/* Some basic disk information */
#define IMR_ZONE_SIZE_SHIFT_DEFAULT      16      /* number of blocks/zone, e.g. 2^16=65536 */
#define IMR_BLOCK_SIZE_SHIFT_DEFAULT     3       /* number of sectors/block, 8   */
#define IMR_SECTOR_SIZE_SHIFT_DEFAULT    9       /* number of bytes/sector, 512  */
#define IMR_TRANSFER_PENALTY             60      /* usec */
#define IMR_TRANSFER_PENALTY_MAX         1000    /* usec */
#define IMR_ROTATE_PENALTY               11000   /* usec ,  5400rpm->  rotate time: 11ms*/
#define IMR_MAX_DISCARD_BLOCKS           1024    /* 4 MiB at the 4 KiB key size */
#define IMRSIM_READ_ZERO_FILL            1       /* unmapped logical read */


#define IMR_ALLOCATION_PHASE             2     /* phase of data distribution (2-3)*/

#define TOP_TRACK_NUM_TOTAL 64
/*
 * The size of a zone is 256MB, divided into 64 track groups (top-bottom), with an average track of 2MB.
 * A group of top-bottom has 4MB, that is, 1024 blocks, and there are 64 groups of top-bottom in a zone.
 */

#define IMR_MAX_CAPACITY                 21474836480

#define IMR_LSM_LEVELS                   7
#define IMR_LSM_DEFAULT_UNSORTED_LEVEL   0
#define IMR_LSM_MAX_LEVEL                (IMR_LSM_LEVELS - 1)
#define IMR_LSM_DEBUG_NODE_LIMIT         0
#define IMR_LSM_COMPACTION_THRESHOLD     16
#define IMR_LSM_COMPACTION_THRESHOLD_MIN 1
#define IMR_LSM_COMPACTION_THRESHOLD_MAX 4096
#define IMR_LSM_LEVEL_RATIO_DEFAULT      10
#define IMR_LSM_LEVEL_RATIO_MIN          2
#define IMR_LSM_LEVEL_RATIO_MAX          1000
/*
 * Stable encoded size of one logical mapping record:
 * key(8) + pba(8) + zone(4) + valid(1) + timestamp(8), rounded to 8 bytes.
 * Do not use sizeof(struct imr_lsm_*_node): those structs contain transient
 * pointers and compiler padding, neither of which is part of an SST-like
 * mapping record.
 */
#define IMR_LSM_RECORD_BYTES             32ULL
#define IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_DEFAULT \
    ((__u64)IMR_LSM_COMPACTION_THRESHOLD * IMR_LSM_RECORD_BYTES)
#define IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_MIN IMR_LSM_RECORD_BYTES
#define IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_MAX (1ULL << 40)
#define IMR_LSM_SCORE_SCALE              1000
#define IMR_LSM_SCORE_BOOST              10
#define IMR_LSM_LEVEL_COMPACTION_WORK_ROUNDS 1
#define IMR_LSM_LEVEL_COMPACTION_BUILD_DELAY_MS_MAX 5000
#define IMR_LSM_SEGMENT_ZONE_MIXED       ((__u32)~0U)
#define IMR_LSM_TRACK_BOTTOM             1
#define IMR_LSM_TRACK_TOP                2
#define IMR_LSM_PLACEMENT_NONE           0
#define IMR_LSM_PLACEMENT_BOTTOM_TO_TOP  1
#define IMR_LSM_ZONE_COMPACTION_NONE     ((__u32)~0U)
#define IMR_LSM_KEY_EMPTY                ((__u32)~0U)
#define IMR_LSM_ZONE_COMPACTION_AUTO_RUN_DEFAULT 1
#define IMR_LSM_ZONE_GC_RATIO_SCALE      1000
#define IMR_LSM_ZONE_GC_MIN_INVALID_RATIO_PERMILLE_DEFAULT 250
#define IMR_LSM_ZONE_GC_MIN_INVALID_RATIO_PERMILLE_MAX \
    IMR_LSM_ZONE_GC_RATIO_SCALE
#define IMR_LSM_ZONE_GC_FREE_LOW_WATERMARK_DEFAULT 3
#define IMR_LSM_ZONE_GC_FREE_LOW_WATERMARK_MAX 1024
#define IMR_LSM_BLOOM_MIN_BITS           256
#define IMR_LSM_BLOOM_MAX_BITS           16384
#define IMR_LSM_BLOOM_BITS_PER_KEY       10
#define IMR_LSM_BLOOM_BITS_PER_KEY_MIN   1
#define IMR_LSM_BLOOM_BITS_PER_KEY_MAX   64
#define IMR_LSM_BLOOM_MIN_HASHES         3
#define IMR_LSM_BLOOM_MAX_HASHES         7
#define IMR_LSM_COMPACTION_MIN_OBSOLETE_RATIO 250
#define IMR_LSM_COMPACTION_MIN_INVALID   1
#define IMR_LSM_COMPACTION_DELETE_BOOST  1000
#define IMR_LSM_COMPACTION_TOMBSTONE_BOOST 500
#define IMR_LSM_COMPACTION_POLICY_AGE_WEIGHT 1
#define IMR_LSM_COMPACTION_POLICY_HOTNESS_WEIGHT -100
#define IMR_LSM_COMPACTION_POLICY_PLACEMENT_WEIGHT -1
#define IMR_LSM_COMPACTION_POLICY_RMW_WEIGHT -1
#define IMR_LSM_COMPACTION_POLICY_ZONE_FULLNESS_WEIGHT 1
#define IMR_LSM_SEGMENT_NONE             ((__u32)~0U)
#define IMR_LSM_READ_TREE_LIMIT          4096

static __u64   IMR_CAPACITY;            /* disk capacity (in sectors) */
static __u32   IMR_NUMZONES;            /* number of zones */
static __u32   IMR_NUMZONES_DEFAULT;
static __u32   IMR_ZONE_SIZE_SHIFT;     
static __u32   IMR_BLOCK_SIZE_SHIFT;

static __u32 IMR_TOP_TRACK_SIZE = 456;      /* number of blocks/topTrack  456 */
static __u32 IMR_BOTTOM_TRACK_SIZE = 568;   /* number of blocks/bottomTrack  568 */

/* 1.2.0 adds persistent active-zone allocator/reverse-map state. */
__u32 VERSION = IMRSIM_VERSION(1,2,0);

struct imrsim_c{             /* Mapped devices in the Device Mapper framework, also known as logical devices. */
    struct dm_dev *dev;      /* block device */
    sector_t       start;    /* starting address */
};

struct imrsim_io_context {
    __u8 append_reserved;
};

/*
 * Module-lifetime lock hierarchy:
 *
 *   imrsim_ioctl_lock -> imr_lsm_compaction_lock ->
 *       imrsim_zone_lock -> imr_lsm_lock
 *
 * The locks must outlive an individual dm target because debugfs exists for
 * the whole module lifetime.  Any entry point that needs both zone state and
 * LSM metadata must take them in the order above.
 */
static DEFINE_MUTEX(imrsim_zone_lock);
static DEFINE_MUTEX(imrsim_ioctl_lock);
static DEFINE_MUTEX(imr_lsm_compaction_lock);
static DECLARE_WAIT_QUEUE_HEAD(imr_lsm_zone_copy_wait);
static bool imr_lsm_zone_copy_active;
static __u32 imr_lsm_zone_copy_source = IMR_LSM_ZONE_COMPACTION_NONE;
static __u32 imr_lsm_zone_copy_dest = IMR_LSM_ZONE_COMPACTION_NONE;

/*
 * Foreground append reservations are serialized by imrsim_zone_lock.  The
 * lock is released before the remapped bio reaches the backing device, so
 * advancing z_map_size here gives concurrent writers disjoint physical slots.
 */
struct imr_lsm_active_zone_allocator {
    __u32 active_zone;
    __u32 free_cursor;
    __u64 next_generation;
};

static struct imr_lsm_active_zone_allocator imr_lsm_allocator = {
    .active_zone = IMR_LSM_ZONE_COMPACTION_NONE,
    .free_cursor = 0,
    .next_generation = 1,
};
static atomic_t imr_lsm_append_writes_inflight = ATOMIC_INIT(0);

/* IMRSIM Statistics */
static struct imrsim_state       *zone_state = NULL;
/* Array of zone status information */
static struct imrsim_zone_status *zone_status = NULL;
/* Bytes available after the mapped data range for serialized zone state. */
static __u64 imrsim_persistence_capacity_bytes;

/* error log */
static __u32 imrsim_dbg_rerr;
static __u32 imrsim_dbg_werr;
static __u32 imrsim_dbg_log_enabled = 0;
static unsigned long imrsim_dev_idle_checkpoint = 0;

/*
 * Non-policy diagnostic counters.  These are intentionally kept outside the
 * persistent zone/LSM state: they describe one live mapper instance and must
 * never influence mapping, compaction selection, or recovery decisions.
 */
struct imrsim_diagnostic_stats {
    atomic64_t level_compaction_queued_at_ns;
    atomic64_t level_compaction_queue_depth;
    atomic64_t level_compaction_queue_depth_max;
    atomic64_t level_compaction_queue_wait_count;
    atomic64_t level_compaction_queue_wait_total_ns;
    atomic64_t level_compaction_queue_wait_max_ns;
    atomic64_t last_level_compaction_queue_wait_ns;
    atomic64_t level_compaction_work_time_count;
    atomic64_t level_compaction_work_total_ns;
    atomic64_t level_compaction_work_max_ns;
    atomic64_t last_level_compaction_work_ns;
    atomic64_t level_compaction_phase;
    atomic64_t level_compaction_prepare_time_count;
    atomic64_t level_compaction_prepare_total_ns;
    atomic64_t level_compaction_prepare_max_ns;
    atomic64_t last_level_compaction_prepare_ns;
    atomic64_t level_compaction_build_time_count;
    atomic64_t level_compaction_build_total_ns;
    atomic64_t level_compaction_build_max_ns;
    atomic64_t last_level_compaction_build_ns;
    atomic64_t level_compaction_publish_time_count;
    atomic64_t level_compaction_publish_total_ns;
    atomic64_t level_compaction_publish_max_ns;
    atomic64_t last_level_compaction_publish_ns;
    atomic64_t level_compaction_publish_conflict_count;
    atomic64_t level_compaction_time_count;
    atomic64_t level_compaction_total_ns;
    atomic64_t level_compaction_max_ns;
    atomic64_t last_level_compaction_ns;
    atomic64_t level_compaction_time_count_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_total_ns_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_max_ns_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_zone_lock_wait_count;
    atomic64_t level_compaction_zone_lock_wait_total_ns;
    atomic64_t level_compaction_zone_lock_wait_max_ns;
    atomic64_t level_compaction_lsm_lock_wait_count;
    atomic64_t level_compaction_lsm_lock_wait_total_ns;
    atomic64_t level_compaction_lsm_lock_wait_max_ns;
    atomic64_t level_compaction_zone_lock_hold_count;
    atomic64_t level_compaction_zone_lock_hold_total_ns;
    atomic64_t level_compaction_zone_lock_hold_max_ns;
    atomic64_t last_level_compaction_zone_lock_hold_ns;
    atomic64_t level_compaction_lsm_lock_hold_count;
    atomic64_t level_compaction_lsm_lock_hold_total_ns;
    atomic64_t level_compaction_lsm_lock_hold_max_ns;
    atomic64_t last_level_compaction_lsm_lock_hold_ns;
    atomic64_t level_compaction_post_round_count;
    atomic64_t level_compaction_post_round_total_ns;
    atomic64_t level_compaction_post_round_max_ns;
    atomic64_t last_level_compaction_post_round_ns;
    atomic64_t level_compaction_post_recalc_count;
    atomic64_t level_compaction_post_recalc_total_ns;
    atomic64_t level_compaction_post_recalc_max_ns;
    atomic64_t last_level_compaction_post_recalc_ns;
    atomic64_t invalid_recalc_total_ns;
    atomic64_t invalid_recalc_max_ns;
    atomic64_t last_invalid_recalc_ns;
    atomic64_t invalid_recalc_segments_scanned_total;
    atomic64_t invalid_recalc_segments_scanned_max;
    atomic64_t invalid_recalc_entries_scanned_total;
    atomic64_t invalid_recalc_entries_scanned_max;
    atomic64_t invalid_incremental_supersede_count;
    atomic64_t invalid_incremental_entries_updated;
    atomic64_t invalid_incremental_segment_publish_count;
    atomic64_t invalid_incremental_segment_publish_entries;
    atomic64_t invalid_incremental_segment_retire_count;
    atomic64_t invalid_incremental_segment_retire_entries;
    atomic64_t invalid_incremental_fallback_recalc_count;
    atomic64_t newest_index_lookup_count;
    atomic64_t newest_index_hit_count;
    atomic64_t newest_index_miss_count;
    atomic64_t newest_index_update_fail_count;
    atomic64_t newest_index_fallback_count;
    atomic64_t level_compaction_input_entries_total;
    atomic64_t level_compaction_input_entries_max;
    atomic64_t last_level_compaction_input_entries;
    atomic64_t level_compaction_output_entries_total;
    atomic64_t level_compaction_output_entries_max;
    atomic64_t last_level_compaction_output_entries;
    atomic64_t level_compaction_input_entries_total_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_input_entries_max_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_output_entries_total_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_output_entries_max_by_level[IMR_LSM_LEVELS];
    atomic64_t level_compaction_coalesced_schedule_count;
    atomic64_t level_compaction_no_work_run_count;
    atomic64_t level_compaction_score_max;
    atomic64_t last_level_compaction_evaluated_score;
    atomic64_t last_level_compaction_schedule_score;
    atomic64_t last_level_compaction_requeue_score;
    atomic64_t foreground_zone_lock_wait_count;
    atomic64_t foreground_zone_lock_wait_total_ns;
    atomic64_t foreground_zone_lock_wait_max_ns;
    atomic64_t foreground_lsm_lock_wait_count;
    atomic64_t foreground_lsm_lock_wait_total_ns;
    atomic64_t foreground_lsm_lock_wait_max_ns;
    atomic64_t flush_bio_count;
    atomic64_t incoming_fua_write_count;
    atomic64_t fua_write_count;
    atomic64_t internal_flush_fua_write_count;
    atomic64_t internal_flush_fua_write_total_ns;
    atomic64_t internal_flush_fua_write_max_ns;
    atomic64_t last_internal_flush_fua_write_ns;
    atomic64_t internal_rmw_fua_write_count;
    atomic64_t partial_io_count;
    atomic64_t partial_read_count;
    atomic64_t partial_rmw_count;
    atomic64_t partial_io_queue_wait_count;
    atomic64_t partial_io_queue_wait_total_ns;
    atomic64_t partial_io_queue_wait_max_ns;
    atomic64_t partial_io_total_ns;
    atomic64_t partial_io_max_ns;
    atomic64_t last_partial_io_ns;
    atomic64_t partial_rmw_total_ns;
    atomic64_t partial_rmw_max_ns;
    atomic64_t last_partial_rmw_ns;
    atomic64_t legacy_rmw_count;
    atomic64_t legacy_rmw_total_ns;
    atomic64_t legacy_rmw_max_ns;
    atomic64_t last_legacy_rmw_ns;
};

static struct imrsim_diagnostic_stats imrsim_diag;

static __u64 imrsim_diag_now_ns(void)
{
    s64 now = ktime_to_ns(ktime_get());

    return now > 0 ? (__u64)now : 0;
}

static __u64 imrsim_diag_elapsed_ns(__u64 start_ns)
{
    __u64 end_ns = imrsim_diag_now_ns();

    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

static void imrsim_diag_set_max(atomic64_t *maximum, __u64 value)
{
    s64 old = atomic64_read(maximum);

    while(value > (__u64)old){
        s64 previous = atomic64_cmpxchg(maximum, old, (s64)value);

        if(previous == old){
            break;
        }
        old = previous;
    }
}

static void imrsim_diag_record_duration(atomic64_t *count,
                                        atomic64_t *total,
                                        atomic64_t *maximum,
                                        atomic64_t *last,
                                        __u64 duration_ns)
{
    if(count){
        atomic64_inc(count);
    }
    atomic64_add((s64)duration_ns, total);
    imrsim_diag_set_max(maximum, duration_ns);
    if(last){
        atomic64_set(last, (s64)duration_ns);
    }
}

static void imrsim_diag_timed_mutex_lock(struct mutex *lock,
                                         atomic64_t *count,
                                         atomic64_t *total,
                                         atomic64_t *maximum)
{
    __u64 start_ns = imrsim_diag_now_ns();

    mutex_lock(lock);
    imrsim_diag_record_duration(count, total, maximum, NULL,
                                imrsim_diag_elapsed_ns(start_ns));
}

static void imrsim_diag_reset(void)
{
    /* The caller holds both mapper locks while no dm target is active. */
    memset(&imrsim_diag, 0, sizeof(imrsim_diag));
}

#define IMRSIM_DATA_LOG(fmt, args...)                                      \
    do {                                                                   \
        if(READ_ONCE(imrsim_dbg_log_enabled) && printk_ratelimit()){       \
            printk(KERN_INFO fmt, ##args);                                 \
        }                                                                  \
    } while(0)

/* IMR-LSM unsorted-write metadata */
struct imr_lsm_unsorted_node {
    __u64 key;
    sector_t pba;
    __u32 zone_idx;
    __u8 valid;
    __u64 timestamp;
    struct imr_lsm_unsorted_node *next;
};

struct imr_lsm_sorted_node {
    __u64 key;
    sector_t pba;
    __u32 zone_idx;
    __u8 valid;
    __u64 timestamp;
    struct imr_lsm_sorted_node *next;
};

struct imr_lsm_level_state {
    struct imr_lsm_unsorted_node *unsorted_head;
    struct imr_lsm_sorted_node *sorted_head;
    __u32 unsorted_count;
    __u32 sorted_count;
};

struct imr_lsm_read_tree_node {
    struct rb_node rb;
    struct list_head lru;
    __u64 key;
    sector_t pba;
    __u8 valid;
    __u64 timestamp;
};

/*
 * Complete mapper-lifetime latest-version index used by invalid accounting.
 * Unlike read_tree, this tree is not an evictable read cache and is never
 * cleared independently of the in-memory LSM metadata.
 */
struct imr_lsm_newest_node {
    struct rb_node rb;
    __u64 key;
    __u64 timestamp;
    __u8 valid;
    /* Active segment records tied at the newest timestamp for this key. */
    struct imr_lsm_block_entry *live_segment_head;
};

struct imr_lsm_block_entry {
    __u64 key;
    sector_t pba;
    __u32 zone_idx;
    sector_t source_pba;
    sector_t output_pba;
    __u8 source_pba_valid;
    __u8 output_mapped;
    __u8 output_copy_planned;
    __u8 output_copied;
    __u8 output_committed;
    __u8 valid;
    __u64 timestamp;
    /* Runtime-only invalid-accounting links; never serialized. */
    struct imr_lsm_segment *accounting_segment;
    struct imr_lsm_block_entry *newest_next;
    __u8 newest_linked;
    __u8 accounting_invalid;
    __u8 accounting_obsolete;
    __u8 accounting_delete_invalid;
};

struct imr_lsm_segment {
    __u32 id;
    __u32 level;
    __u32 zone_idx;
    __u8 track_type;
    __u8 retired;
    __u8 invalid_stats_accounted;
    __u32 node_count;
    __u64 min_key;
    __u64 max_key;
    __u64 min_timestamp;
    __u64 max_timestamp;
    __u32 bloom_key_count;
    __u32 bloom_bits_count;
    __u32 bloom_word_count;
    __u32 bloom_hash_count;
    __u64 *bloom_bits;
    __u32 block_table_count;
    struct imr_lsm_block_entry *block_table;
    __u32 live_count;
    __u32 invalid_count;
    __u32 obsolete_count;
    __u32 tombstone_count;
    __u32 delete_invalid_count;
    __u32 obsolete_ratio_permille;
    __u8 compaction_candidate;
    __u64 compaction_score;
    __u64 read_hit_count;
    __u64 last_read_timestamp;
    __u8 placement_policy;
    __u8 placement_target_track_type;
    __u32 placement_bottom_track_start;
    __u32 placement_bottom_track_end;
    __u64 placement_bottom_key_start;
    __u64 placement_bottom_key_end;
    __u32 placement_top_track_start;
    __u32 placement_top_track_end;
    sector_t placement_top_pba_start;
    sector_t placement_top_pba_end;
    __u8 output_allocated;
    __u8 output_track_type;
    __u32 output_block_count;
    sector_t output_pba_start;
    sector_t output_pba_end;
    struct imr_lsm_segment *next;
};

struct imr_lsm_segment_builder {
    __u32 node_count;
    __u32 zone_idx;
    __u64 min_key;
    __u64 max_key;
    __u64 min_timestamp;
    __u64 max_timestamp;
    __u32 block_table_count;
    __u32 block_table_capacity;
    struct imr_lsm_block_entry *block_table;
};

struct imr_lsm_zone_compaction_plan {
    struct imr_lsm_segment_builder segment_builder;
    struct block_device *bdev;
    sector_t bdev_start;
    __u32 source_zone;
    __u32 dest_zone0;
    __u32 dest_zone1;
    __u32 block_bytes;
    __u32 input_entries;
    __u32 live_entries;
    __u32 skipped_entries;
    __u32 copied_entries;
    __u32 failed_entries;
    sector_t output_start;
    sector_t output_end;
};

enum imr_lsm_level_compaction_phase {
    IMR_LSM_LEVEL_COMPACTION_IDLE = 0,
    IMR_LSM_LEVEL_COMPACTION_PREPARE,
    IMR_LSM_LEVEL_COMPACTION_BUILD,
    IMR_LSM_LEVEL_COMPACTION_PUBLISH,
};

/*
 * A level-compaction plan borrows immutable source/destination lists while
 * imr_lsm_compaction_lock is held.  Expensive allocation, sorting, merging,
 * and Bloom-filter construction use private objects with the global zone/LSM
 * locks dropped.  Publish validates and splices the exact captured L0 node
 * run, so foreground nodes prepended during build are never removed.
 */
struct imr_lsm_level_compaction_plan {
    struct imr_lsm_segment_builder segment_builder;
    struct imr_lsm_unsorted_node *source_unsorted_head;
    struct imr_lsm_unsorted_node *source_unsorted_tail;
    struct imr_lsm_unsorted_node *source_unsorted_after;
    struct imr_lsm_sorted_node *source_sorted_head;
    struct imr_lsm_sorted_node *destination_sorted_head;
    struct imr_lsm_sorted_node *prepared_sorted_head;
    struct imr_lsm_segment *output_segment;
    struct imr_lsm_unsorted_node *retired_unsorted_head;
    struct imr_lsm_sorted_node *retired_source_sorted_head;
    struct imr_lsm_sorted_node *discarded_prepared_sorted_head;
    __u32 source_level;
    __u32 destination_level;
    __u32 source_unsorted_count;
    __u32 source_sorted_count;
    __u32 destination_sorted_count;
    __u32 prepared_sorted_count;
    __u32 input_entries;
    __u32 output_entries;
    __u32 bloom_bits_per_key;
    __u32 build_delay_ms;
    __u64 metadata_epoch;
    struct block_device *output_bdev;
    bool move_sorted;
    bool retire_source;
    bool prepared;
    bool published;
};

struct imr_lsm_read_filter_stats {
    __u64 segment_lookup_count;
    __u64 segment_skip_count;
    __u64 segment_candidate_count;
    __u64 bloom_lookup_count;
    __u64 bloom_negative_count;
    __u64 bloom_maybe_count;
    __u64 block_table_lookup_count;
    __u64 block_table_hit_count;
    __u64 block_table_miss_count;
    bool block_table_hit;
    struct imr_lsm_block_entry block_table_entry;
};

struct imr_lsm_stats {
    __u64 logical_write_count;
    __u64 lsm_record_insert_count;
    __u64 lsm_write_count;
    __u64 delete_count;
    __u64 discard_bio_count;
    __u64 discard_delete_count;
    __u64 discard_delete_failed_count;
    __u64 last_discard_lba;
    __u64 last_discard_sectors;
    int last_discard_error;
    __u64 read_lookup_count;
    __u64 read_miss_count;
    __u64 segment_lookup_count;
    __u64 segment_skip_count;
    __u64 segment_candidate_count;
    __u64 bloom_lookup_count;
    __u64 bloom_negative_count;
    __u64 bloom_maybe_count;
    __u64 block_table_lookup_count;
    __u64 block_table_hit_count;
    __u64 block_table_miss_count;
    __u64 read_tree_lookup_count;
    __u64 read_tree_hit_count;
    __u64 read_tree_miss_count;
    __u64 read_tree_update_count;
    __u64 read_tree_update_fail_count;
    __u64 read_tree_remove_count;
    __u64 read_tree_evict_count;
    __u64 last_read_tree_key;
    __u64 last_read_tree_pba;
    __u64 last_read_tree_timestamp;
    __u64 last_read_tree_valid;
    __u64 last_read_tree_hit;
    __u64 last_segment_read_key;
    __u64 last_segment_lookup_count;
    __u64 last_segment_skip_count;
    __u64 last_segment_candidate_count;
    __u64 last_bloom_lookup_count;
    __u64 last_bloom_negative_count;
    __u64 last_bloom_maybe_count;
    __u64 last_block_table_lookup_count;
    __u64 last_block_table_hit_count;
    __u64 last_block_table_miss_count;
    __u64 last_block_table_hit_key;
    __u64 last_block_table_hit_pba;
    __u64 last_block_table_hit_timestamp;
    __u64 last_block_table_hit_valid;
    __u64 placement_policy_count;
    __u64 placement_bottom_to_top_count;
    __u64 placement_no_target_count;
    __u64 placement_mixed_zone_count;
    __u32 last_placement_segment_id;
    __u8 last_placement_policy;
    __u32 last_placement_bottom_track_start;
    __u32 last_placement_bottom_track_end;
    __u32 last_placement_top_track_start;
    __u32 last_placement_top_track_end;
    sector_t last_placement_top_pba_start;
    sector_t last_placement_top_pba_end;
    __u64 placement_output_alloc_count;
    __u64 placement_output_no_target_count;
    __u64 placement_output_no_space_count;
    __u32 last_placement_output_segment_id;
    __u8 last_placement_output_allocated;
    __u8 last_placement_output_track_type;
    __u32 last_placement_output_block_count;
    sector_t last_placement_output_pba_start;
    sector_t last_placement_output_pba_end;
    __u64 invalid_recalc_count;
    __u64 invalid_segment_count;
    __u64 invalid_entry_count;
    __u64 obsolete_entry_count;
    __u64 tombstone_entry_count;
    __u64 delete_invalid_entry_count;
    __u32 max_obsolete_ratio_permille;
    __u32 max_obsolete_segment_id;
    __u32 last_invalid_recalc_segments;
    __u32 last_invalid_recalc_entries;
    __u64 segment_compaction_selection_count;
    __u64 segment_compaction_candidate_count;
    __u64 segment_compaction_no_candidate_count;
    __u32 segment_compaction_candidate_segment_id;
    __u32 segment_compaction_candidate_level;
    __u64 segment_compaction_candidate_score;
    __u32 segment_compaction_candidate_ratio_permille;
    __u32 segment_compaction_candidate_invalid_count;
    __u32 segment_compaction_candidate_delete_invalid_count;
    __u32 segment_compaction_candidate_tombstone_count;
    __u64 segment_compaction_execute_count;
    __u64 segment_compaction_execute_no_candidate_count;
    __u32 last_segment_compaction_from_id;
    __u32 last_segment_compaction_to_id;
    __u32 last_segment_compaction_input_entries;
    __u32 last_segment_compaction_live_entries;
    __u32 last_segment_compaction_dropped_entries;
    __u64 segment_output_mapping_count;
    __u64 segment_output_mapping_entry_count;
    __u64 segment_output_mapping_no_output_count;
    __u32 last_segment_output_mapping_segment_id;
    __u32 last_segment_output_mapping_entry_count;
    sector_t last_segment_output_mapping_pba_start;
    sector_t last_segment_output_mapping_pba_end;
    __u64 segment_output_copy_plan_count;
    __u64 segment_output_copy_plan_entry_count;
    __u64 segment_output_copy_plan_missing_mapping_count;
    __u32 last_segment_output_copy_plan_segments;
    __u32 last_segment_output_copy_plan_entries;
    __u32 last_segment_output_copy_plan_missing_mappings;
    __u32 last_segment_output_copy_plan_segment_id;
    sector_t last_segment_output_copy_plan_source_pba_start;
    sector_t last_segment_output_copy_plan_source_pba_end;
    sector_t last_segment_output_copy_plan_output_pba_start;
    sector_t last_segment_output_copy_plan_output_pba_end;
    __u64 segment_output_metadata_commit_count;
    __u64 segment_output_metadata_commit_entry_count;
    __u64 segment_output_metadata_commit_already_count;
    __u64 segment_output_metadata_commit_missing_plan_count;
    __u32 last_segment_output_metadata_commit_segments;
    __u32 last_segment_output_metadata_commit_entries;
    __u32 last_segment_output_metadata_commit_already;
    __u32 last_segment_output_metadata_commit_missing_plan;
    __u32 last_segment_output_metadata_commit_segment_id;
    sector_t last_segment_output_metadata_commit_source_pba_start;
    sector_t last_segment_output_metadata_commit_source_pba_end;
    sector_t last_segment_output_metadata_commit_output_pba_start;
    sector_t last_segment_output_metadata_commit_output_pba_end;
    __u64 segment_output_physical_copy_count;
    __u64 segment_output_physical_copy_entry_count;
    __u64 segment_output_physical_copy_failed_count;
    __u32 last_segment_output_physical_copy_segments;
    __u32 last_segment_output_physical_copy_entries;
    __u32 last_segment_output_physical_copy_failed;
    __u32 last_segment_output_physical_copy_segment_id;
    int last_segment_output_physical_copy_error;
    sector_t last_segment_output_physical_copy_source_pba_start;
    sector_t last_segment_output_physical_copy_source_pba_end;
    sector_t last_segment_output_physical_copy_output_pba_start;
    sector_t last_segment_output_physical_copy_output_pba_end;
    __u64 zone_compaction_candidate_count;
    __u32 zone_compaction_candidate_zone;
    __u32 zone_compaction_candidate_dest_zone;
    __u32 zone_compaction_candidate_map_size;
    __u32 zone_compaction_candidate_reclaimable;
    __u32 zone_compaction_candidate_ratio_permille;
    __u32 zone_gc_free_zone_count;
    __u8 zone_gc_pressure;
    __u8 zone_compaction_candidate_pressure;
    __u8 zone_compaction_candidate_ready;
    __u32 last_zone_compaction_candidate_zone;
    __u32 last_zone_compaction_candidate_dest_zone;
    __u32 last_zone_compaction_candidate_map_size;
    __u32 last_zone_compaction_candidate_reclaimable;
    __u32 last_zone_compaction_candidate_ratio_permille;
    __u8 last_zone_compaction_candidate_pressure;
    __u8 last_zone_compaction_candidate_ready;
    __u64 zone_compaction_auto_pending_count;
    __u32 last_zone_compaction_auto_pending_zone;
    __u64 zone_compaction_auto_run_count;
    __u64 zone_compaction_auto_busy_retry_count;
    __u64 zone_compaction_auto_run_failed_count;
    __u32 last_zone_compaction_auto_run_zone;
    int last_zone_compaction_auto_run_error;
    __u64 zone_compaction_count;
    __u64 zone_compaction_failed_count;
    /* Successful zone-GC totals; failed attempts are accounted separately. */
    __u64 zone_compaction_input_entries_total;
    __u64 zone_compaction_live_entries_total;
    __u64 zone_compaction_skipped_entries_total;
    __u64 zone_compaction_copied_entries_total;
    __u64 zone_compaction_committed_entries_total;
    __u64 zone_compaction_failed_entries_total;
    __u32 last_zone_compaction_source_zone;
    __u32 last_zone_compaction_dest_zone0;
    __u32 last_zone_compaction_dest_zone1;
    __u32 last_zone_compaction_input_entries;
    __u32 last_zone_compaction_live_entries;
    __u32 last_zone_compaction_skipped_entries;
    __u32 last_zone_compaction_copied_entries;
    __u32 last_zone_compaction_failed_entries;
    int last_zone_compaction_error;
    sector_t last_zone_compaction_output_pba_start;
    sector_t last_zone_compaction_output_pba_end;
    __u64 tree_hit_count;
    __u64 unsorted_hit_count;
    __u64 segment_hit_count;
    __u64 sorted_hit_count;
    __u64 tombstone_hit_count;
    __u64 fallback_count;
    __u64 compaction_count;
    __u64 level_compaction_work_schedule_count;
    __u64 level_compaction_work_run_count;
    __u64 level_compaction_work_round_count;
    __u64 level_compaction_work_requeue_count;
    __u64 level_compaction_work_error_count;
    int last_level_compaction_work_error;
    __u64 metadata_compaction_input_bytes;
    __u64 metadata_compaction_output_bytes;
    __u64 last_compaction_input_bytes;
    __u64 last_compaction_output_bytes;
    __u32 last_compaction_from;
    __u32 last_compaction_to;
    __u32 last_compaction_input;
    __u32 last_compaction_output_total;
};

struct imr_lsm_metadata {
    bool initialized;
    __u8 zone_compaction_auto_run;
    __u8 zone_compaction_auto_running;
    __u8 zone_compaction_auto_pending;
    __u8 level_compaction_pending;
    __u8 level_compaction_running;
    __u32 zone_compaction_auto_pending_zone;
    __u64 timestamp;
    __u32 active_write_level;
    __u32 base_level;
    __u32 effective_level_multiplier;
    int lowest_unnecessary_level;
    __u64 level_max_bytes[IMR_LSM_LEVELS];
    __u32 next_segment_id;
    __u32 segment_count;
    struct imr_lsm_segment *segment_head;
    struct imr_lsm_segment *segment_tail;
    struct rb_root read_tree;
    struct list_head read_lru;
    __u32 read_tree_limit;
    __u32 read_tree_size;
    struct rb_root newest_tree;
    __u32 newest_tree_size;
    __u8 newest_index_valid;
    struct imr_lsm_stats stats;
    struct imr_lsm_level_state levels[IMR_LSM_LEVELS];
};

static struct imr_lsm_metadata imr_lsm_meta;
static __u64 imr_lsm_metadata_epoch;
static DEFINE_MUTEX(imr_lsm_lock);
static struct dentry *imr_lsm_debugfs_dir;
static struct block_device *imr_lsm_output_bdev;
static sector_t imr_lsm_output_bdev_start;
static struct workqueue_struct *imrsim_partial_io_wq;
/*
 * Unstable debug/VM validation overrides, not production policy knobs.
 * They are reset whenever the singleton dm target is created or destroyed.
 */
static __u32 imr_lsm_compaction_threshold = IMR_LSM_COMPACTION_THRESHOLD;
static __u64 imr_lsm_max_bytes_for_level_base =
    IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_DEFAULT;
static __u32 imr_lsm_max_bytes_for_level_multiplier =
    IMR_LSM_LEVEL_RATIO_DEFAULT;
static __u32 imr_lsm_bloom_bits_per_key = IMR_LSM_BLOOM_BITS_PER_KEY;
static __u32 imr_lsm_level_compaction_build_delay_ms;
static __u32 imr_lsm_zone_gc_min_invalid_ratio_permille =
    IMR_LSM_ZONE_GC_MIN_INVALID_RATIO_PERMILLE_DEFAULT;
static __u32 imr_lsm_zone_gc_free_low_watermark =
    IMR_LSM_ZONE_GC_FREE_LOW_WATERMARK_DEFAULT;
static void imr_lsm_zone_compaction_auto_work(struct work_struct *work);
static DECLARE_WORK(imr_lsm_zone_compaction_work,
                    imr_lsm_zone_compaction_auto_work);
static struct workqueue_struct *imr_lsm_zone_compaction_wq;
static void imr_lsm_level_compaction_auto_work(struct work_struct *work);
static DECLARE_WORK(imr_lsm_level_compaction_work,
                    imr_lsm_level_compaction_auto_work);
static struct workqueue_struct *imr_lsm_level_compaction_wq;

static int imrsim_read_page(struct block_device *dev, sector_t lba,
                            int size, struct page *page);
static int imrsim_write_page(struct block_device *dev, sector_t lba,
                             __u32 size, struct page *page);
static void imrsim_complete_bio(struct bio *bio, int error);

enum imr_lsm_lookup_result {
    IMR_LSM_LOOKUP_MISS = 0,
    IMR_LSM_LOOKUP_VALID,
    IMR_LSM_LOOKUP_DELETED,
};

enum imr_lsm_read_source {
    IMR_LSM_READ_SOURCE_NONE = 0,
    IMR_LSM_READ_SOURCE_UNSORTED,
    IMR_LSM_READ_SOURCE_SEGMENT,
    IMR_LSM_READ_SOURCE_SORTED,
};

/* Multi-device support, currently not supported. */
enum imrsim_target_lifecycle {
    IMRSIM_TARGET_INACTIVE = 0,
    IMRSIM_TARGET_ACTIVE = 1,
    IMRSIM_TARGET_TEARDOWN = 2,
};
int imrsim_single = IMRSIM_TARGET_INACTIVE;

/* Caller must hold imrsim_zone_lock. */
static bool imrsim_target_ready_locked(void)
{
    return imrsim_single == IMRSIM_TARGET_ACTIVE &&
           zone_state && zone_status;
}

/* Constants representing configuration changes */
enum imrsim_conf_change{
    IMR_NO_CHANGE     = 0x00,
    IMR_CONFIG_CHANGE = 0x01,
    IMR_STATS_CHANGE  = 0x02,
    IMR_STATUS_CHANGE = 0x04
};

/* persistent storage */
#define IMR_PSTORE_PG_OFF \
    (offsetof(struct imrsim_state, stats) + \
     offsetof(struct imrsim_stats, zone_stats))
#define IMR_PSTORE_CHECK  1000
#define IMR_PSTORE_QDEPTH 128
#define IMR_PSTORE_PG_GAP 2

/* persistent storage task structure */
static struct imrsim_pstore_task
{
    struct task_struct  *pstore_thread; 
    __u32                stu_zone_idx[IMR_PSTORE_QDEPTH];
    __u8                 stu_zone_idx_cnt;
    __u8                 stu_zone_idx_gap;
    sector_t             pstore_lba;
    unsigned char        flag;              /* three bit for imrsim_conf_change */
}imrsim_ptask;

/* RMW scheme structure */
static struct imrsim_RMW_task
{
    struct task_struct  *task;
    struct bio          *bio;
    sector_t            lba[2];
    __u8                lba_num;
}imrsim_rmw_task;

/* Task completion only; every internal bio has its own wait context. */
static struct completion imrsim_rmw_event;

struct imrsim_partial_io_task
{
    struct work_struct   work;
    struct dm_target    *ti;
    struct bio          *bio;
    __u64                queued_at_ns;
    sector_t             lba;
    sector_t             bio_sectors;
    int                  cdir;
    int                  policy_wflag;
    bool                 zero_fill;
};

/* Caller must hold imrsim_zone_lock. A full save subsumes all dirty queues. */
static void imrsim_ptask_mark_config_change_locked(void)
{
    imrsim_ptask.flag = IMR_CONFIG_CHANGE;
    memset(imrsim_ptask.stu_zone_idx, 0,
           sizeof(imrsim_ptask.stu_zone_idx));
    imrsim_ptask.stu_zone_idx_cnt = 0;
    imrsim_ptask.stu_zone_idx_gap = 0;
    if(imrsim_ptask.pstore_thread){
        wake_up_process(imrsim_ptask.pstore_thread);
    }
}

/* Caller must hold imrsim_zone_lock. */
static void imrsim_ptask_queue_zone_status_locked(__u32 idx);

/* To get the serialized size of the variable-length stats structure. */
static __u32 imrsim_stats_size(void)
{
    return (__u32)(offsetof(struct imrsim_stats, zone_stats) +
                   (__u64)sizeof(struct imrsim_zone_stats) * IMR_NUMZONES);
}

/*
 * The state embeds a one-element variable tail followed by zone_status[] and
 * a trailing magic value.  Use offsetof instead of a hand-written sum so
 * compiler padding is included, and reject the on-disk u32 length overflow.
 */
static int imrsim_state_size_for_zones(__u32 num_zones, __u32 *state_size)
{
    __u64 size;
    __u64 alignment = __alignof__(struct imrsim_zone_status);

    if(!state_size){
        return -EINVAL;
    }

    size = offsetof(struct imrsim_state, stats) +
           offsetof(struct imrsim_stats, zone_stats);
    size += (__u64)sizeof(struct imrsim_zone_stats) * num_zones;
    size = (size + alignment - 1) & ~(alignment - 1);
    size += (__u64)sizeof(struct imrsim_zone_status) * num_zones;
    size += sizeof(__u32);
    if(size > (__u64)((__u32)~0U) ||
       size > (__u64)(~0UL) - (PAGE_SIZE - 1)){
        return -EOVERFLOW;
    }

    *state_size = (__u32)size;
    return 0;
}

static int imrsim_state_size(__u32 *state_size)
{
    return imrsim_state_size_for_zones(IMR_NUMZONES, state_size);
}

static struct imrsim_zone_status *
imrsim_zone_status_ptr(struct imrsim_state *state, __u32 num_zones)
{
    __u64 offset;
    __u64 alignment = __alignof__(struct imrsim_zone_status);

    offset = offsetof(struct imrsim_state, stats) +
             offsetof(struct imrsim_stats, zone_stats);
    offset += (__u64)sizeof(struct imrsim_zone_stats) * num_zones;
    offset = (offset + alignment - 1) & ~(alignment - 1);

    return (struct imrsim_zone_status *)((unsigned char *)state + offset);
}

static __u64 imrsim_state_persistence_bytes(__u32 state_size)
{
    return (((__u64)state_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
}

/* To get how many sectors a zone has. */
static __u32 num_sectors_zone(void)
{
    return ((__u32)1 << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT);
}

/* To get the sector address where the zone starts. */
static __u64 zone_idx_lba(__u64 idx){
    return (idx << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT);
}

static __u32 imrsim_lba_zone_idx(sector_t lba)
{
    return (__u32)(lba >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT);
}

/* Returns the exponent of a power of 2. */
static __u64 index_power_of_2(__u64 num)
{
    __u64 index = 0;
    while(num >>= 1){
        ++index;
    }
    return index;
}

/* Device idle time initialization. */
static void imrsim_dev_idle_init(void)
{
    imrsim_dev_idle_checkpoint = jiffies;
    zone_state->stats.dev_stats.idle_stats.dev_idle_time_max = 0;
    zone_state->stats.dev_stats.idle_stats.dev_idle_time_min = jiffies / HZ;
}

static void imr_lsm_free_sorted_nodes(struct imr_lsm_sorted_node *node)
{
    while(node){
        struct imr_lsm_sorted_node *next = node->next;

        kfree(node);
        node = next;
    }
}

static void imr_lsm_free_unsorted_nodes(struct imr_lsm_unsorted_node *node)
{
    while(node){
        struct imr_lsm_unsorted_node *next = node->next;

        kfree(node);
        node = next;
    }
}

static void imr_lsm_free_sorted_level_locked(__u32 level)
{
    imr_lsm_free_sorted_nodes(imr_lsm_meta.levels[level].sorted_head);

    imr_lsm_meta.levels[level].sorted_head = NULL;
    imr_lsm_meta.levels[level].sorted_count = 0;
}

static void imr_lsm_free_unsorted_level_locked(__u32 level)
{
    struct imr_lsm_unsorted_node *node = imr_lsm_meta.levels[level].unsorted_head;

    imr_lsm_free_unsorted_nodes(node);

    imr_lsm_meta.levels[level].unsorted_head = NULL;
    imr_lsm_meta.levels[level].unsorted_count = 0;
}

static void imr_lsm_free_segments_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;

    while(segment){
        struct imr_lsm_segment *next = segment->next;

        vfree(segment->block_table);
        vfree(segment->bloom_bits);
        kfree(segment);
        segment = next;
    }

    imr_lsm_meta.segment_head = NULL;
    imr_lsm_meta.segment_tail = NULL;
    imr_lsm_meta.segment_count = 0;
    imr_lsm_meta.next_segment_id = 0;
}

static void imr_lsm_init_read_tree_locked(void)
{
    imr_lsm_meta.read_tree.rb_node = NULL;
    INIT_LIST_HEAD(&imr_lsm_meta.read_lru);
    imr_lsm_meta.read_tree_limit = IMR_LSM_READ_TREE_LIMIT;
    imr_lsm_meta.read_tree_size = 0;
}

static __u32 imr_lsm_read_tree_limit_locked(void)
{
    return imr_lsm_meta.read_tree_limit ?
           imr_lsm_meta.read_tree_limit : IMR_LSM_READ_TREE_LIMIT;
}

static __u32 imr_lsm_clear_read_tree_locked(void)
{
    struct rb_node *rb;
    __u32 cleared = 0;

    while((rb = rb_first(&imr_lsm_meta.read_tree))){
        struct imr_lsm_read_tree_node *node =
            rb_entry(rb, struct imr_lsm_read_tree_node, rb);

        rb_erase(&node->rb, &imr_lsm_meta.read_tree);
        list_del(&node->lru);
        kfree(node);
        cleared++;
    }

    imr_lsm_meta.read_tree.rb_node = NULL;
    INIT_LIST_HEAD(&imr_lsm_meta.read_lru);
    imr_lsm_meta.read_tree_size = 0;

    return cleared;
}

static void imr_lsm_free_read_tree_locked(void)
{
    imr_lsm_clear_read_tree_locked();
}

static void imr_lsm_init_newest_index_locked(void)
{
    imr_lsm_meta.newest_tree = RB_ROOT;
    imr_lsm_meta.newest_tree_size = 0;
    imr_lsm_meta.newest_index_valid = 1;
}

static void imr_lsm_free_newest_index_locked(void)
{
    struct rb_node *rb;

    while((rb = rb_first(&imr_lsm_meta.newest_tree))){
        struct imr_lsm_newest_node *node =
            rb_entry(rb, struct imr_lsm_newest_node, rb);

        rb_erase(&node->rb, &imr_lsm_meta.newest_tree);
        kfree(node);
    }

    imr_lsm_meta.newest_tree = RB_ROOT;
    imr_lsm_meta.newest_tree_size = 0;
    imr_lsm_meta.newest_index_valid = 1;
}

static void imr_lsm_release_metadata_locked(void)
{
    __u32 level;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        imr_lsm_free_unsorted_level_locked(level);
        imr_lsm_free_sorted_level_locked(level);
    }
    imr_lsm_free_segments_locked();
    imr_lsm_free_read_tree_locked();
    imr_lsm_free_newest_index_locked();
    imr_lsm_metadata_epoch++;
    if(!imr_lsm_metadata_epoch){
        imr_lsm_metadata_epoch++;
    }
    memset(&imr_lsm_meta, 0, sizeof(imr_lsm_meta));
    imr_lsm_init_read_tree_locked();
    imr_lsm_init_newest_index_locked();
}

static void imr_lsm_initialize_metadata_locked(void)
{
    imr_lsm_release_metadata_locked();
    imr_lsm_meta.initialized = true;
    imr_lsm_meta.zone_compaction_auto_run =
        IMR_LSM_ZONE_COMPACTION_AUTO_RUN_DEFAULT;
    imr_lsm_meta.timestamp = 0;
    imr_lsm_meta.active_write_level = IMR_LSM_DEFAULT_UNSORTED_LEVEL;
    imr_lsm_meta.base_level = IMR_LSM_MAX_LEVEL;
    imr_lsm_meta.lowest_unnecessary_level = -1;
    imr_lsm_meta.stats.last_segment_compaction_from_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_segment_compaction_to_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_placement_output_segment_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_segment_output_mapping_segment_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_segment_output_copy_plan_segment_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_segment_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.last_segment_output_physical_copy_segment_id =
        IMR_LSM_SEGMENT_NONE;
    imr_lsm_meta.stats.zone_compaction_candidate_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.zone_compaction_candidate_dest_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_candidate_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_candidate_dest_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.zone_compaction_auto_pending_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_auto_pending_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_auto_run_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_source_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_dest_zone0 =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_dest_zone1 =
        IMR_LSM_ZONE_COMPACTION_NONE;
}

static void imr_lsm_init_metadata(void)
{
    mutex_lock(&imr_lsm_lock);
    imr_lsm_initialize_metadata_locked();
    mutex_unlock(&imr_lsm_lock);
    printk(KERN_INFO "imrsim: IMR-LSM metadata initialized\n");
}

static struct imr_lsm_read_tree_node *
imr_lsm_tree_find_node_locked(__u64 key)
{
    struct rb_node *rb = imr_lsm_meta.read_tree.rb_node;

    while(rb){
        struct imr_lsm_read_tree_node *node =
            rb_entry(rb, struct imr_lsm_read_tree_node, rb);

        if(key < node->key){
            rb = rb->rb_left;
        }else if(key > node->key){
            rb = rb->rb_right;
        }else{
            return node;
        }
    }

    return NULL;
}

static void imr_lsm_tree_record_last_locked(__u64 key, sector_t pba,
                                            __u8 valid, __u64 timestamp,
                                            __u8 hit)
{
    imr_lsm_meta.stats.last_read_tree_key = key;
    imr_lsm_meta.stats.last_read_tree_pba = pba;
    imr_lsm_meta.stats.last_read_tree_timestamp = timestamp;
    imr_lsm_meta.stats.last_read_tree_valid = valid ? 1 : 0;
    imr_lsm_meta.stats.last_read_tree_hit = hit ? 1 : 0;
}

static enum imr_lsm_lookup_result
imr_lsm_tree_lookup_locked(__u64 key, sector_t *pba, __u64 *timestamp)
{
    struct imr_lsm_read_tree_node *node;

    imr_lsm_meta.stats.read_tree_lookup_count++;
    node = imr_lsm_tree_find_node_locked(key);
    if(!node){
        imr_lsm_meta.stats.read_tree_miss_count++;
        imr_lsm_tree_record_last_locked(key, 0, 0, 0, 0);
        return IMR_LSM_LOOKUP_MISS;
    }

    list_move_tail(&node->lru, &imr_lsm_meta.read_lru);
    imr_lsm_meta.stats.read_tree_hit_count++;
    imr_lsm_tree_record_last_locked(node->key, node->pba, node->valid,
                                    node->timestamp, 1);
    if(timestamp){
        *timestamp = node->timestamp;
    }
    if(node->valid){
        *pba = node->pba;
        return IMR_LSM_LOOKUP_VALID;
    }

    return IMR_LSM_LOOKUP_DELETED;
}

static void imr_lsm_tree_remove_node_locked(
    struct imr_lsm_read_tree_node *node)
{
    rb_erase(&node->rb, &imr_lsm_meta.read_tree);
    list_del(&node->lru);
    kfree(node);
    if(imr_lsm_meta.read_tree_size){
        imr_lsm_meta.read_tree_size--;
    }
    imr_lsm_meta.stats.read_tree_remove_count++;
}

static void imr_lsm_tree_remove_locked(__u64 key)
{
    struct imr_lsm_read_tree_node *node =
        imr_lsm_tree_find_node_locked(key);

    if(node){
        imr_lsm_tree_remove_node_locked(node);
    }
}

static void imr_lsm_tree_evict_locked(void)
{
    __u32 limit = imr_lsm_read_tree_limit_locked();

    while(imr_lsm_meta.read_tree_size > limit &&
          !list_empty(&imr_lsm_meta.read_lru)){
        struct imr_lsm_read_tree_node *node =
            list_first_entry(&imr_lsm_meta.read_lru,
                             struct imr_lsm_read_tree_node, lru);

        rb_erase(&node->rb, &imr_lsm_meta.read_tree);
        list_del(&node->lru);
        kfree(node);
        imr_lsm_meta.read_tree_size--;
        imr_lsm_meta.stats.read_tree_evict_count++;
    }
}

static void imr_lsm_tree_update_locked(__u64 key, sector_t pba,
                                       __u8 valid, __u64 timestamp)
{
    struct rb_node **link = &imr_lsm_meta.read_tree.rb_node;
    struct rb_node *parent = NULL;
    struct imr_lsm_read_tree_node *node;

    while(*link){
        parent = *link;
        node = rb_entry(parent, struct imr_lsm_read_tree_node, rb);
        if(key < node->key){
            link = &parent->rb_left;
        }else if(key > node->key){
            link = &parent->rb_right;
        }else{
            if(timestamp >= node->timestamp){
                node->pba = pba;
                node->valid = valid;
                node->timestamp = timestamp;
                imr_lsm_meta.stats.read_tree_update_count++;
            }
            list_move_tail(&node->lru, &imr_lsm_meta.read_lru);
            return;
        }
    }

    node = kzalloc(sizeof(*node), GFP_NOIO);
    if(!node){
        imr_lsm_meta.stats.read_tree_update_fail_count++;
        return;
    }

    node->key = key;
    node->pba = pba;
    node->valid = valid;
    node->timestamp = timestamp;
    INIT_LIST_HEAD(&node->lru);
    rb_link_node(&node->rb, parent, link);
    rb_insert_color(&node->rb, &imr_lsm_meta.read_tree);
    list_add_tail(&node->lru, &imr_lsm_meta.read_lru);
    imr_lsm_meta.read_tree_size++;
    imr_lsm_meta.stats.read_tree_update_count++;
    imr_lsm_tree_evict_locked();
}

static struct imr_lsm_newest_node *
imr_lsm_newest_index_find_locked(__u64 key)
{
    struct rb_node *rb = imr_lsm_meta.newest_tree.rb_node;

    while(rb){
        struct imr_lsm_newest_node *node =
            rb_entry(rb, struct imr_lsm_newest_node, rb);

        if(key < node->key){
            rb = rb->rb_left;
        }else if(key > node->key){
            rb = rb->rb_right;
        }else{
            return node;
        }
    }

    return NULL;
}

static void imr_lsm_incremental_supersede_locked(
    struct imr_lsm_newest_node *node, __u8 newer_valid);

static int imr_lsm_newest_index_update_locked(__u64 key, __u64 timestamp,
                                               __u8 valid)
{
    struct rb_node **link = &imr_lsm_meta.newest_tree.rb_node;
    struct rb_node *parent = NULL;
    struct imr_lsm_newest_node *node;

    while(*link){
        parent = *link;
        node = rb_entry(parent, struct imr_lsm_newest_node, rb);
        if(key < node->key){
            link = &parent->rb_left;
        }else if(key > node->key){
            link = &parent->rb_right;
        }else{
            if(timestamp >= node->timestamp){
                if(timestamp > node->timestamp &&
                   imr_lsm_meta.newest_index_valid){
                    imr_lsm_incremental_supersede_locked(node, valid);
                }
                node->timestamp = timestamp;
                node->valid = valid;
            }
            return 0;
        }
    }

    node = kzalloc(sizeof(*node), GFP_NOIO);
    if(!node){
        imr_lsm_meta.newest_index_valid = 0;
        atomic64_inc(&imrsim_diag.newest_index_update_fail_count);
        return -ENOMEM;
    }

    node->key = key;
    node->timestamp = timestamp;
    node->valid = valid;
    rb_link_node(&node->rb, parent, link);
    rb_insert_color(&node->rb, &imr_lsm_meta.newest_tree);
    imr_lsm_meta.newest_tree_size++;
    return 0;
}

static __u32 imr_lsm_compaction_threshold_locked(void)
{
    return imr_lsm_compaction_threshold ?
           imr_lsm_compaction_threshold : IMR_LSM_COMPACTION_THRESHOLD;
}

static __u64 imr_lsm_max_bytes_for_level_base_locked(void)
{
    return imr_lsm_max_bytes_for_level_base ?
           imr_lsm_max_bytes_for_level_base :
           IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_DEFAULT;
}

static __u32 imr_lsm_max_bytes_for_level_multiplier_locked(void)
{
    return imr_lsm_max_bytes_for_level_multiplier ?
           imr_lsm_max_bytes_for_level_multiplier :
           IMR_LSM_LEVEL_RATIO_DEFAULT;
}

static __u32 imr_lsm_bloom_bits_per_key_locked(void)
{
    return imr_lsm_bloom_bits_per_key ?
           imr_lsm_bloom_bits_per_key : IMR_LSM_BLOOM_BITS_PER_KEY;
}

static void imr_lsm_reset_validation_overrides_locked(void)
{
    imr_lsm_compaction_threshold = IMR_LSM_COMPACTION_THRESHOLD;
    imr_lsm_max_bytes_for_level_base =
        IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_DEFAULT;
    imr_lsm_max_bytes_for_level_multiplier =
        IMR_LSM_LEVEL_RATIO_DEFAULT;
    imr_lsm_bloom_bits_per_key = IMR_LSM_BLOOM_BITS_PER_KEY;
    imr_lsm_level_compaction_build_delay_ms = 0;
    imr_lsm_zone_gc_min_invalid_ratio_permille =
        IMR_LSM_ZONE_GC_MIN_INVALID_RATIO_PERMILLE_DEFAULT;
    imr_lsm_zone_gc_free_low_watermark =
        IMR_LSM_ZONE_GC_FREE_LOW_WATERMARK_DEFAULT;
}

static __u64 imr_lsm_level_target_bytes_locked(__u32 level)
{
    if(level == 0){
        return (__u64)imr_lsm_compaction_threshold_locked() *
               IMR_LSM_RECORD_BYTES;
    }
    if(level >= IMR_LSM_LEVELS ||
       !imr_lsm_meta.level_max_bytes[level] ||
       imr_lsm_meta.level_max_bytes[level] == ~0ULL){
        return 0;
    }

    return imr_lsm_meta.level_max_bytes[level];
}

static __u32 imr_lsm_level_capacity(__u32 level)
{
    __u64 target_bytes;
    __u64 target_entries;

    if(level == 0 || level >= IMR_LSM_LEVELS){
        return imr_lsm_compaction_threshold_locked();
    }

    target_bytes = imr_lsm_level_target_bytes_locked(level);
    if(!target_bytes){
        return imr_lsm_compaction_threshold_locked();
    }
    target_entries = div64_u64(target_bytes, IMR_LSM_RECORD_BYTES);
    if(target_bytes % IMR_LSM_RECORD_BYTES){
        target_entries++;
    }
    if(!target_entries){
        return 1;
    }
    if(target_entries > (__u32)~0U){
        return (__u32)~0U;
    }

    return (__u32)target_entries;
}

static __u32 imr_lsm_level_total_count_locked(__u32 level)
{
    return imr_lsm_meta.levels[level].unsorted_count +
           imr_lsm_meta.levels[level].sorted_count;
}

static __u64 imr_lsm_level_actual_bytes_locked(__u32 level)
{
    return (__u64)imr_lsm_level_total_count_locked(level) *
           IMR_LSM_RECORD_BYTES;
}

static __u64 imr_lsm_div_round_up_u64(__u64 value, __u32 divisor)
{
    __u64 quotient;

    if(!divisor){
        return value;
    }
    quotient = div64_u64(value, divisor);
    if(value % divisor){
        quotient++;
    }
    return quotient;
}

static __u64 imr_lsm_level1_target_for_multiplier(__u64 last_level_bytes,
                                                  __u32 multiplier)
{
    int level;

    for(level = IMR_LSM_MAX_LEVEL - 1; level >= 1; level--){
        last_level_bytes =
            imr_lsm_div_round_up_u64(last_level_bytes, multiplier);
    }
    return last_level_bytes;
}

/*
 * With a fixed number of levels, an unusually large database may not fit the
 * configured fanout while keeping L1 at or below base bytes. RocksDB raises
 * its effective level multiplier in that case. Use an integer binary search
 * for the smallest multiplier that satisfies the same bound.
 */
static __u32 imr_lsm_effective_level_multiplier(__u64 max_level_bytes,
                                                __u64 base_bytes_max,
                                                __u32 configured_multiplier)
{
    __u32 low = configured_multiplier;
    __u32 high = configured_multiplier;

    if(imr_lsm_level1_target_for_multiplier(max_level_bytes, high) <=
       base_bytes_max){
        return configured_multiplier;
    }

    while(high < (__u32)~0U){
        if(high > ((__u32)~0U) / 2){
            high = (__u32)~0U;
        }else{
            high *= 2;
        }
        if(imr_lsm_level1_target_for_multiplier(max_level_bytes, high) <=
           base_bytes_max){
            break;
        }
    }

    while(low < high){
        __u32 middle = low + (high - low) / 2;

        if(imr_lsm_level1_target_for_multiplier(max_level_bytes, middle) <=
           base_bytes_max){
            high = middle;
        }else{
            low = middle + 1;
        }
    }
    return low;
}

static void imr_lsm_calculate_dynamic_levels_locked(void)
{
    __u64 max_level_bytes = 0;
    __u64 base_bytes_max = imr_lsm_max_bytes_for_level_base_locked();
    __u32 level_multiplier =
        imr_lsm_max_bytes_for_level_multiplier_locked();
    __u64 base_bytes_min;
    __u64 current_target;
    __u64 previous_target;
    int level;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        imr_lsm_meta.level_max_bytes[level] = ~0ULL;
    }
    imr_lsm_meta.active_write_level = IMR_LSM_DEFAULT_UNSORTED_LEVEL;
    imr_lsm_meta.lowest_unnecessary_level = -1;

    for(level = 1; level < IMR_LSM_LEVELS; level++){
        __u64 level_bytes = imr_lsm_level_actual_bytes_locked(level);

        if(level_bytes > max_level_bytes){
            max_level_bytes = level_bytes;
        }
    }

    if(max_level_bytes > base_bytes_max){
        level_multiplier = imr_lsm_effective_level_multiplier(
            max_level_bytes, base_bytes_max, level_multiplier);
    }
    imr_lsm_meta.effective_level_multiplier = level_multiplier;
    base_bytes_min = div64_u64(base_bytes_max, level_multiplier);
    if(!base_bytes_min){
        base_bytes_min = 1;
    }

    /*
     * RocksDB starts an empty/small LSM at the last level.  Only after the
     * largest level grows beyond max_bytes_for_level_base does the base move
     * upward.  Fresh writes still enter L0 and compact directly to base_level.
     */
    if(max_level_bytes <= base_bytes_max){
        imr_lsm_meta.base_level = IMR_LSM_MAX_LEVEL;
        imr_lsm_meta.level_max_bytes[IMR_LSM_MAX_LEVEL] = base_bytes_max;
    }else{
        /*
         * Anchor the last level to the actual largest level size, then work
         * backwards.  This preserves the configured fanout and chooses the
         * first target in (base / multiplier, base] as the dynamic base.
         */
        imr_lsm_meta.level_max_bytes[IMR_LSM_MAX_LEVEL] = max_level_bytes;
        current_target = max_level_bytes;
        imr_lsm_meta.base_level = IMR_LSM_MAX_LEVEL;

        for(level = IMR_LSM_MAX_LEVEL - 1; level >= 1; level--){
            previous_target =
                imr_lsm_div_round_up_u64(current_target, level_multiplier);
            imr_lsm_meta.level_max_bytes[level] = previous_target;
            imr_lsm_meta.base_level = level;
            current_target = previous_target;
            if(previous_target <= base_bytes_max){
                if(previous_target <= base_bytes_min){
                    imr_lsm_meta.level_max_bytes[level] =
                        base_bytes_min + 1;
                }
                break;
            }
        }
    }

    /* Existing data above the newly selected base is drained downward. */
    for(level = 1; level < (int)imr_lsm_meta.base_level; level++){
        if(imr_lsm_level_total_count_locked(level)){
            imr_lsm_meta.lowest_unnecessary_level = level;
        }
    }
}

static void imr_lsm_debugfs_show_active_target_locked(struct seq_file *seq)
{
    imr_lsm_calculate_dynamic_levels_locked();
    seq_printf(seq, "active_write_level: %u\n",
               imr_lsm_meta.active_write_level);
    seq_printf(seq, "dynamic_base_level: %u\n", imr_lsm_meta.base_level);
    seq_printf(seq, "lowest_unnecessary_level: %d\n",
               imr_lsm_meta.lowest_unnecessary_level);
    seq_puts(seq, "level_compaction_dynamic_level_bytes: 1\n");
    seq_puts(seq, "level_compaction_background: 1\n");
    seq_printf(seq, "level_compaction_work_round_limit: %u\n",
               IMR_LSM_LEVEL_COMPACTION_WORK_ROUNDS);
    seq_puts(seq, "level_size_unit: logical_metadata_bytes\n");
    seq_printf(seq, "lsm_record_bytes: %llu\n",
               (unsigned long long)IMR_LSM_RECORD_BYTES);
    seq_printf(seq, "max_bytes_for_level_base: %llu\n",
               (unsigned long long)
               imr_lsm_max_bytes_for_level_base_locked());
    seq_printf(seq, "max_bytes_for_level_multiplier: %u\n",
               imr_lsm_max_bytes_for_level_multiplier_locked());
    seq_printf(seq, "effective_max_bytes_for_level_multiplier: %u\n",
               imr_lsm_meta.effective_level_multiplier);
    seq_puts(seq, "insert_target: L0 unsorted\n");
    seq_puts(seq, "compact_target: dynamic base/next level\n");
}

static __u32 imr_lsm_next_power_of_two_u32(__u32 value)
{
    __u32 result = 1;

    if(value <= 1){
        return 1;
    }

    while(result < value && result <= ((__u32)~0U) / 2){
        result <<= 1;
    }

    return result;
}

static __u32 imr_lsm_bloom_choose_bits(__u32 key_count,
                                       __u32 bits_per_key)
{
    __u64 target_bits;
    __u32 bits;

    if(!key_count){
        return IMR_LSM_BLOOM_MIN_BITS;
    }

    target_bits = (__u64)key_count * bits_per_key;
    if(target_bits < IMR_LSM_BLOOM_MIN_BITS){
        target_bits = IMR_LSM_BLOOM_MIN_BITS;
    }
    if(target_bits > IMR_LSM_BLOOM_MAX_BITS){
        target_bits = IMR_LSM_BLOOM_MAX_BITS;
    }

    bits = imr_lsm_next_power_of_two_u32((__u32)target_bits);
    if(bits < IMR_LSM_BLOOM_MIN_BITS){
        bits = IMR_LSM_BLOOM_MIN_BITS;
    }
    if(bits > IMR_LSM_BLOOM_MAX_BITS){
        bits = IMR_LSM_BLOOM_MAX_BITS;
    }

    return bits;
}

static __u32 imr_lsm_bloom_choose_hashes(__u32 bits_per_key)
{
    __u32 hashes = (bits_per_key * 69) / 100;

    if(hashes < IMR_LSM_BLOOM_MIN_HASHES){
        hashes = IMR_LSM_BLOOM_MIN_HASHES;
    }
    if(hashes > IMR_LSM_BLOOM_MAX_HASHES){
        hashes = IMR_LSM_BLOOM_MAX_HASHES;
    }

    return hashes;
}

static __u32 imr_lsm_bloom_hash(__u64 key, __u32 seed, __u32 bloom_bits)
{
    key ^= ((__u64)seed + 1) * 0x9e3779b97f4a7c15ULL;
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;

    return (__u32)(key & (bloom_bits - 1));
}

static void imr_lsm_bloom_add(__u64 *bloom_words, __u32 bloom_bits,
                              __u32 bloom_hashes, __u64 key)
{
    __u32 hash_idx;

    for(hash_idx = 0; hash_idx < bloom_hashes; hash_idx++){
        __u32 bit = imr_lsm_bloom_hash(key, hash_idx, bloom_bits);

        bloom_words[bit / 64] |= (1ULL << (bit % 64));
    }
}

static bool imr_lsm_bloom_may_contain(const struct imr_lsm_segment *segment,
                                      __u64 key)
{
    __u32 hash_idx;

    if(!segment->bloom_bits || !segment->bloom_bits_count ||
       !segment->bloom_hash_count){
        return true;
    }

    for(hash_idx = 0; hash_idx < segment->bloom_hash_count; hash_idx++){
        __u32 bit = imr_lsm_bloom_hash(key, hash_idx,
                                       segment->bloom_bits_count);

        if(!(segment->bloom_bits[bit / 64] & (1ULL << (bit % 64)))){
            return false;
        }
    }

    return true;
}

static int imr_lsm_segment_build_bloom(
    struct imr_lsm_segment *segment, __u32 configured_bits_per_key)
{
    __u32 key_count = segment->block_table_count;
    __u32 bits = imr_lsm_bloom_choose_bits(key_count,
                                            configured_bits_per_key);
    __u32 words = bits / 64;
    __u32 bits_per_key = key_count ?
        max_t(__u32, 1, bits / key_count) :
        configured_bits_per_key;
    __u32 hashes = imr_lsm_bloom_choose_hashes(bits_per_key);
    __u32 entry_idx;

    if((unsigned long)words >
       (~0UL / sizeof(*segment->bloom_bits))){
        return -ENOMEM;
    }
    segment->bloom_bits = vzalloc(
        (unsigned long)sizeof(*segment->bloom_bits) * words);
    if(!segment->bloom_bits){
        return -ENOMEM;
    }

    segment->bloom_key_count = key_count;
    segment->bloom_bits_count = bits;
    segment->bloom_word_count = words;
    segment->bloom_hash_count = hashes;

    for(entry_idx = 0; entry_idx < segment->block_table_count;
        entry_idx++){
        imr_lsm_bloom_add(segment->bloom_bits, segment->bloom_bits_count,
                          segment->bloom_hash_count,
                          segment->block_table[entry_idx].key);
    }

    return 0;
}

static void imr_lsm_segment_builder_release(
    struct imr_lsm_segment_builder *builder)
{
    vfree(builder->block_table);
    builder->block_table = NULL;
    builder->block_table_count = 0;
    builder->block_table_capacity = 0;
}

static void imr_lsm_segment_release(struct imr_lsm_segment *segment)
{
    if(!segment){
        return;
    }
    vfree(segment->block_table);
    vfree(segment->bloom_bits);
    kfree(segment);
}

static int imr_lsm_segment_builder_reserve_block_table(
    struct imr_lsm_segment_builder *builder)
{
    struct imr_lsm_block_entry *new_table;
    __u32 new_capacity;

    if(builder->block_table_count < builder->block_table_capacity){
        return 0;
    }

    if(builder->block_table_capacity){
        if(builder->block_table_capacity > ((__u32)~0U) / 2){
            return -ENOMEM;
        }
        new_capacity = builder->block_table_capacity * 2;
    }else{
        new_capacity = imr_lsm_compaction_threshold_locked();
    }

    if((unsigned long)new_capacity >
       (~0UL / sizeof(*new_table))){
        return -ENOMEM;
    }
    new_table = vzalloc((unsigned long)sizeof(*new_table) * new_capacity);
    if(!new_table){
        return -ENOMEM;
    }

    if(builder->block_table){
        memcpy(new_table, builder->block_table,
               sizeof(*new_table) * builder->block_table_count);
        vfree(builder->block_table);
    }
    builder->block_table = new_table;
    builder->block_table_capacity = new_capacity;

    return 0;
}

static int imr_lsm_segment_builder_add_block_entry(
    struct imr_lsm_segment_builder *builder, __u64 key, sector_t pba,
    __u32 zone_idx, __u8 valid, __u64 timestamp)
{
    struct imr_lsm_block_entry *entry;
    __u32 pos = 0;
    int ret;

    if(builder->block_table_count &&
       builder->block_table[builder->block_table_count - 1].key < key){
        pos = builder->block_table_count;
    }else{
        while(pos < builder->block_table_count &&
              builder->block_table[pos].key < key){
            pos++;
        }
    }

    if(pos < builder->block_table_count &&
       builder->block_table[pos].key == key){
        entry = &builder->block_table[pos];
        if(timestamp > entry->timestamp){
            entry->pba = pba;
            entry->zone_idx = zone_idx;
            entry->source_pba = 0;
            entry->output_pba = 0;
            entry->source_pba_valid = 0;
            entry->output_mapped = 0;
            entry->output_copy_planned = 0;
            entry->output_copied = 0;
            entry->output_committed = 0;
            entry->valid = valid;
            entry->timestamp = timestamp;
        }
        return 0;
    }

    ret = imr_lsm_segment_builder_reserve_block_table(builder);
    if(ret){
        return ret;
    }

    if(pos < builder->block_table_count){
        memmove(&builder->block_table[pos + 1],
                &builder->block_table[pos],
                sizeof(*builder->block_table) *
                (builder->block_table_count - pos));
    }

    entry = &builder->block_table[pos];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->pba = pba;
    entry->zone_idx = zone_idx;
    entry->valid = valid;
    entry->timestamp = timestamp;
    builder->block_table_count++;

    return 0;
}

static int imr_lsm_segment_builder_add(struct imr_lsm_segment_builder *builder,
                                       __u64 key, sector_t pba,
                                       __u32 zone_idx, __u8 valid,
                                       __u64 timestamp)
{
    int ret;

    ret = imr_lsm_segment_builder_add_block_entry(
        builder, key, pba, zone_idx, valid, timestamp);
    if(ret){
        return ret;
    }

    if(!builder->node_count){
        builder->zone_idx = zone_idx;
        builder->min_key = key;
        builder->max_key = key;
        builder->min_timestamp = timestamp;
        builder->max_timestamp = timestamp;
    }else{
        if(builder->zone_idx != zone_idx){
            builder->zone_idx = IMR_LSM_SEGMENT_ZONE_MIXED;
        }
        if(key < builder->min_key){
            builder->min_key = key;
        }
        if(key > builder->max_key){
            builder->max_key = key;
        }
        if(timestamp < builder->min_timestamp){
            builder->min_timestamp = timestamp;
        }
        if(timestamp > builder->max_timestamp){
            builder->max_timestamp = timestamp;
        }
    }

    builder->node_count++;

    return 0;
}

static int imr_lsm_segment_builder_add_unsorted(
    struct imr_lsm_segment_builder *builder,
    struct imr_lsm_unsorted_node *node)
{
    return imr_lsm_segment_builder_add(builder, node->key, node->pba,
                                       node->zone_idx, node->valid,
                                       node->timestamp);
}

static int imr_lsm_segment_builder_add_sorted(
    struct imr_lsm_segment_builder *builder,
    struct imr_lsm_sorted_node *node)
{
    return imr_lsm_segment_builder_add(builder, node->key, node->pba,
                                       node->zone_idx, node->valid,
                                       node->timestamp);
}

static void imr_lsm_apply_segment_placement_locked(
    struct imr_lsm_segment *segment);
static void imr_lsm_map_segment_output_entries_locked(
    struct imr_lsm_segment *segment);
static void imr_lsm_recalculate_segment_invalid_stats_locked(void);
static bool imr_lsm_account_new_segment_invalid_stats_locked(
    struct imr_lsm_segment *segment);
static void imr_lsm_unaccount_segment_invalid_stats_locked(
    struct imr_lsm_segment *segment);
static void imr_lsm_reevaluate_latest_tombstones_locked(
    const struct imr_lsm_segment *retired_segment);
static void imr_lsm_refresh_max_obsolete_ratio_locked(void);
static void imr_lsm_update_segment_compaction_selection_locked(void);
static bool imr_lsm_find_newer_record_locked(__u64 key, __u64 timestamp,
                                             __u64 *newer_timestamp,
                                             __u8 *newer_valid);
static bool imr_lsm_zone_is_free_locked(__u32 zone_idx);
static __u32 imr_lsm_count_free_zones_locked(void);
static __u32 imr_lsm_find_free_zone_locked(__u32 exclude_zone);
static int imr_lsm_compact_zone(__u32 source_zone);

static int imr_lsm_prepare_segment(
    __u32 level, __u8 track_type,
    struct imr_lsm_segment_builder *builder, __u32 bloom_bits_per_key,
    struct imr_lsm_segment **prepared_segment)
{
    struct imr_lsm_segment *segment;
    int ret;

    *prepared_segment = NULL;
    if(!builder->node_count){
        imr_lsm_segment_builder_release(builder);
        return 0;
    }

    segment = kzalloc(sizeof(*segment), GFP_NOIO);
    if(!segment){
        return -ENOMEM;
    }
    segment->level = level;
    segment->zone_idx = builder->zone_idx;
    segment->track_type = track_type;
    segment->node_count = builder->node_count;
    segment->min_key = builder->min_key;
    segment->max_key = builder->max_key;
    segment->min_timestamp = builder->min_timestamp;
    segment->max_timestamp = builder->max_timestamp;
    segment->block_table_count = builder->block_table_count;
    segment->block_table = builder->block_table;
    ret = imr_lsm_segment_build_bloom(segment, bloom_bits_per_key);
    if(ret){
        /* The builder retains its table when preparation fails. */
        segment->block_table = NULL;
        kfree(segment);
        return ret;
    }

    builder->block_table = NULL;
    builder->block_table_count = 0;
    builder->block_table_capacity = 0;
    *prepared_segment = segment;
    return 0;
}

/* Zone GC discovers live records in physical append order.  Append them in
 * O(1) here and sort the completed table once, avoiding O(n^2) memmoves for a
 * workload whose logical keys are intentionally unrelated to physical slots. */
static int imr_lsm_segment_builder_append_zone_gc(
    struct imr_lsm_segment_builder *builder, __u64 key, sector_t pba,
    sector_t source_pba, __u32 zone_idx, __u64 timestamp)
{
    struct imr_lsm_block_entry *entry;
    int ret;

    ret = imr_lsm_segment_builder_reserve_block_table(builder);
    if(ret){
        return ret;
    }
    entry = &builder->block_table[builder->block_table_count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->pba = pba;
    entry->zone_idx = zone_idx;
    entry->source_pba = source_pba;
    entry->source_pba_valid = 1;
    entry->valid = 1;
    entry->timestamp = timestamp;

    if(!builder->node_count){
        /* GC segments can contain arbitrary logical keys even though their
         * payloads share one physical destination zone.  Mark the segment
         * mixed so legacy logical-zone placement cannot allocate a second,
         * overlapping output range or clear source_pba in the copy plan. */
        builder->zone_idx = IMR_LSM_SEGMENT_ZONE_MIXED;
        builder->min_key = key;
        builder->max_key = key;
        builder->min_timestamp = timestamp;
        builder->max_timestamp = timestamp;
    }else{
        if(key < builder->min_key){
            builder->min_key = key;
        }
        if(key > builder->max_key){
            builder->max_key = key;
        }
        if(timestamp < builder->min_timestamp){
            builder->min_timestamp = timestamp;
        }
        if(timestamp > builder->max_timestamp){
            builder->max_timestamp = timestamp;
        }
    }
    builder->node_count++;
    return 0;
}

static int imr_lsm_block_entry_key_compare(const void *left,
                                            const void *right)
{
    const struct imr_lsm_block_entry *a = left;
    const struct imr_lsm_block_entry *b = right;

    if(a->key < b->key){
        return -1;
    }
    if(a->key > b->key){
        return 1;
    }
    return 0;
}

/* No allocation is allowed here: callers may have started atomic publish. */
static void imr_lsm_publish_prepared_segment_locked(
    struct imr_lsm_segment *segment, bool account_invalid_stats)
{
    segment->id = imr_lsm_meta.next_segment_id++;
    imr_lsm_apply_segment_placement_locked(segment);
    imr_lsm_map_segment_output_entries_locked(segment);

    if(imr_lsm_meta.segment_tail){
        imr_lsm_meta.segment_tail->next = segment;
    }else{
        imr_lsm_meta.segment_head = segment;
    }
    imr_lsm_meta.segment_tail = segment;
    imr_lsm_meta.segment_count++;
    if(account_invalid_stats){
        if(imr_lsm_account_new_segment_invalid_stats_locked(segment)){
            imr_lsm_update_segment_compaction_selection_locked();
        }
    }

    IMRSIM_DATA_LOG("imrsim: IMR-LSM segment id=%u L%u nodes=%u key=%llu-%llu ts=%llu-%llu bloom_keys=%u bloom_bits=%u bloom_hashes=%u table_entries=%u placement=%u bottom_track=%u-%u top_track=%u-%u output=%u output_track=%u output_pba=%llu-%llu\n",
           segment->id, segment->level, segment->node_count,
           (unsigned long long)segment->min_key,
           (unsigned long long)segment->max_key,
           (unsigned long long)segment->min_timestamp,
           (unsigned long long)segment->max_timestamp,
           segment->bloom_key_count, segment->bloom_bits_count,
           segment->bloom_hash_count, segment->block_table_count,
           segment->placement_policy,
           segment->placement_bottom_track_start,
           segment->placement_bottom_track_end,
           segment->placement_top_track_start,
           segment->placement_top_track_end,
           segment->output_allocated, segment->output_track_type,
           (unsigned long long)segment->output_pba_start,
           (unsigned long long)segment->output_pba_end);
}

static int imr_lsm_append_segment_with_accounting_locked(
    __u32 level, __u8 track_type,
    struct imr_lsm_segment_builder *builder,
    bool account_invalid_stats)
{
    struct imr_lsm_segment *segment;
    int ret;

    ret = imr_lsm_prepare_segment(
        level, track_type, builder, imr_lsm_bloom_bits_per_key_locked(),
        &segment);
    if(ret){
        printk(KERN_ERR "imrsim: IMR-LSM segment build failed L%u ret=%d\n",
               level, ret);
        return ret;
    }
    if(segment){
        imr_lsm_publish_prepared_segment_locked(
            segment, account_invalid_stats);
    }

    return 0;
}

static int imr_lsm_append_segment_locked(
    __u32 level, __u8 track_type,
    struct imr_lsm_segment_builder *builder)
{
    return imr_lsm_append_segment_with_accounting_locked(level, track_type,
                                                         builder, true);
}

static __u32 imr_lsm_segment_count_locked(__u32 level)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    __u32 count = 0;

    while(segment){
        if(segment->level == level){
            count++;
        }
        segment = segment->next;
    }

    return count;
}

/*
 * A level compaction materializes every current source-level record in one
 * destination segment. Retire the input segments only after that output has
 * been appended, matching RocksDB's removal of input SSTs after a successful
 * compaction and preventing duplicate active lookup files from accumulating.
 */
static __u32 imr_lsm_retire_level_segments_locked(__u32 level)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    __u32 retired = 0;

    while(segment){
        if(segment->level == level && !segment->retired){
            imr_lsm_unaccount_segment_invalid_stats_locked(segment);
            segment->retired = 1;
            segment->compaction_candidate = 0;
            segment->compaction_score = 0;
            imr_lsm_reevaluate_latest_tombstones_locked(segment);
            retired++;
        }
        segment = segment->next;
    }

    return retired;
}

static __u64 imr_lsm_zone_key_start(__u32 zone_idx)
{
    return (__u64)zone_idx << IMR_ZONE_SIZE_SHIFT;
}

static __u32 imr_lsm_bottom_range_blocks(void)
{
    return IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
}

static __u32 imr_lsm_track_group_blocks(void)
{
    return IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE;
}

static sector_t imr_lsm_zone_bottom_pba(__u32 zone_idx,
                                        __u32 bottom_block_offset)
{
    __u32 track = bottom_block_offset / IMR_BOTTOM_TRACK_SIZE;
    __u32 block = bottom_block_offset % IMR_BOTTOM_TRACK_SIZE;
    __u32 zone_block = track * imr_lsm_track_group_blocks() +
                       IMR_TOP_TRACK_SIZE + block;

    return zone_idx_lba(zone_idx) +
           ((__u64)zone_block << IMR_BLOCK_SIZE_SHIFT);
}

static sector_t imr_lsm_zone_top_pba(__u32 zone_idx, __u32 top_block_offset)
{
    __u32 track = top_block_offset / IMR_TOP_TRACK_SIZE;
    __u32 block = top_block_offset % IMR_TOP_TRACK_SIZE;
    __u32 zone_block = track * imr_lsm_track_group_blocks() + block;

    return zone_idx_lba(zone_idx) +
           ((__u64)zone_block << IMR_BLOCK_SIZE_SHIFT);
}

/* Translate an append slot into the IMR bottom-first, then top-track layout. */
static sector_t imr_lsm_zone_append_pba(__u32 zone_idx, __u32 slot)
{
    __u32 bottom_blocks = imr_lsm_bottom_range_blocks();

    if(slot < bottom_blocks){
        return imr_lsm_zone_bottom_pba(zone_idx, slot);
    }
    return imr_lsm_zone_top_pba(zone_idx, slot - bottom_blocks);
}

static int imr_lsm_zone_append_slot(__u32 zone_idx, sector_t pba,
                                    __u32 *slot)
{
    __u32 group_blocks = imr_lsm_track_group_blocks();
    __u32 bottom_blocks = imr_lsm_bottom_range_blocks();
    sector_t zone_start = zone_idx_lba(zone_idx);
    __u64 zone_block;
    __u32 track;
    __u32 block;

    if(pba < zone_start){
        return -ERANGE;
    }
    zone_block = (__u64)(pba - zone_start) >> IMR_BLOCK_SIZE_SHIFT;
    if(zone_block >= TOTAL_ITEMS){
        return -ERANGE;
    }
    track = (__u32)div64_u64(zone_block, group_blocks);
    block = (__u32)(zone_block % group_blocks);
    if(track >= TOP_TRACK_NUM_TOTAL){
        return -ERANGE;
    }
    if(block < IMR_TOP_TRACK_SIZE){
        *slot = bottom_blocks + track * IMR_TOP_TRACK_SIZE + block;
    }else{
        *slot = track * IMR_BOTTOM_TRACK_SIZE +
                block - IMR_TOP_TRACK_SIZE;
    }
    return *slot < TOTAL_ITEMS ? 0 : -ERANGE;
}

/* Caller holds imrsim_zone_lock. */
static bool imr_lsm_forward_map_lookup_locked(__u64 key, sector_t *pba)
{
    __u32 logical_zone = (__u32)(key >> IMR_ZONE_SIZE_SHIFT);
    __u32 logical_offset = (__u32)(key & (TOTAL_ITEMS - 1));
    int physical_block;

    if(!zone_status || logical_zone >= IMR_NUMZONES){
        return false;
    }
    physical_block = zone_status[logical_zone].z_pba_map[logical_offset];
    if(physical_block == -1){
        return false;
    }
    *pba = (sector_t)(__u32)physical_block << IMR_BLOCK_SIZE_SHIFT;
    return true;
}

/* Caller holds imrsim_zone_lock. */
static int imr_lsm_forward_map_set_locked(__u64 key, sector_t pba)
{
    __u32 logical_zone = (__u32)(key >> IMR_ZONE_SIZE_SHIFT);
    __u32 logical_offset = (__u32)(key & (TOTAL_ITEMS - 1));
    __u64 physical_block = (__u64)pba >> IMR_BLOCK_SIZE_SHIFT;

    if(!zone_status || logical_zone >= IMR_NUMZONES ||
       physical_block >= (__u64)IMR_LSM_KEY_EMPTY){
        return -ERANGE;
    }
    zone_status[logical_zone].z_pba_map[logical_offset] =
        (int)(__u32)physical_block;
    imrsim_ptask_queue_zone_status_locked(logical_zone);
    return 0;
}

static void imr_lsm_reset_segment_output_locked(
    struct imr_lsm_segment *segment)
{
    segment->output_allocated = 0;
    segment->output_track_type = 0;
    segment->output_block_count = 0;
    segment->output_pba_start = 0;
    segment->output_pba_end = 0;
}

static sector_t imr_lsm_segment_output_sector_count(
    const struct imr_lsm_segment *segment)
{
    __u32 block_count = segment->block_table_count;

    if(!block_count){
        block_count = segment->node_count;
    }
    if(!block_count){
        return 0;
    }

    return (sector_t)block_count << IMR_BLOCK_SIZE_SHIFT;
}

static bool imr_lsm_pba_ranges_overlap(sector_t a_start, sector_t a_end,
                                        sector_t b_start, sector_t b_end)
{
    if(a_start > a_end || b_start > b_end){
        return false;
    }

    return a_start <= b_end && b_start <= a_end;
}

static struct imr_lsm_segment *
imr_lsm_find_output_overlap_locked(const struct imr_lsm_segment *segment,
                                   sector_t pba_start, sector_t pba_end)
{
    struct imr_lsm_segment *other = imr_lsm_meta.segment_head;

    while(other){
        if(other != segment && !other->retired && other->output_allocated &&
           imr_lsm_pba_ranges_overlap(pba_start, pba_end,
                                      other->output_pba_start,
                                      other->output_pba_end)){
            return other;
        }
        other = other->next;
    }

    return NULL;
}

static void imr_lsm_record_last_placement_output_locked(
    const struct imr_lsm_segment *segment)
{
    imr_lsm_meta.stats.last_placement_output_segment_id = segment->id;
    imr_lsm_meta.stats.last_placement_output_allocated =
        segment->output_allocated;
    imr_lsm_meta.stats.last_placement_output_track_type =
        segment->output_track_type;
    imr_lsm_meta.stats.last_placement_output_block_count =
        segment->output_block_count;
    imr_lsm_meta.stats.last_placement_output_pba_start =
        segment->output_pba_start;
    imr_lsm_meta.stats.last_placement_output_pba_end =
        segment->output_pba_end;
}

static void imr_lsm_note_segment_output_no_target_locked(
    struct imr_lsm_segment *segment)
{
    imr_lsm_reset_segment_output_locked(segment);
    imr_lsm_meta.stats.placement_output_no_target_count++;
    imr_lsm_record_last_placement_output_locked(segment);
}

static void imr_lsm_allocate_segment_output_locked(
    struct imr_lsm_segment *segment)
{
    sector_t output_sector_count;
    sector_t target_sector_count;
    sector_t cursor;
    sector_t candidate_end;
    __u32 block_count;

    imr_lsm_reset_segment_output_locked(segment);

    if(segment->placement_policy != IMR_LSM_PLACEMENT_BOTTOM_TO_TOP ||
       segment->placement_target_track_type != IMR_LSM_TRACK_TOP ||
       segment->placement_top_pba_start > segment->placement_top_pba_end){
        imr_lsm_meta.stats.placement_output_no_target_count++;
        imr_lsm_record_last_placement_output_locked(segment);
        return;
    }

    output_sector_count = imr_lsm_segment_output_sector_count(segment);
    if(!output_sector_count){
        imr_lsm_meta.stats.placement_output_no_target_count++;
        imr_lsm_record_last_placement_output_locked(segment);
        return;
    }

    target_sector_count =
        segment->placement_top_pba_end - segment->placement_top_pba_start + 1;
    if(output_sector_count > target_sector_count){
        imr_lsm_meta.stats.placement_output_no_space_count++;
        imr_lsm_record_last_placement_output_locked(segment);
        return;
    }

    cursor = segment->placement_top_pba_start;
    while(cursor <= segment->placement_top_pba_end){
        struct imr_lsm_segment *overlap;

        candidate_end = cursor + output_sector_count - 1;
        if(candidate_end < cursor ||
           candidate_end > segment->placement_top_pba_end){
            break;
        }

        overlap = imr_lsm_find_output_overlap_locked(segment, cursor,
                                                     candidate_end);
        if(!overlap){
            block_count = segment->block_table_count ?
                segment->block_table_count : segment->node_count;
            segment->output_allocated = 1;
            segment->output_track_type = segment->placement_target_track_type;
            segment->output_block_count = block_count;
            segment->output_pba_start = cursor;
            segment->output_pba_end = candidate_end;
            imr_lsm_meta.stats.placement_output_alloc_count++;
            imr_lsm_record_last_placement_output_locked(segment);
            return;
        }

        if(overlap->output_pba_end >= segment->placement_top_pba_end){
            break;
        }
        cursor = overlap->output_pba_end + 1;
    }

    imr_lsm_meta.stats.placement_output_no_space_count++;
    imr_lsm_record_last_placement_output_locked(segment);
}

static void imr_lsm_map_segment_output_entries_locked(
    struct imr_lsm_segment *segment)
{
    sector_t cursor;
    __u32 entry_idx;
    __u32 mapped_entries = 0;

    imr_lsm_meta.stats.last_segment_output_mapping_segment_id =
        segment->id;
    imr_lsm_meta.stats.last_segment_output_mapping_entry_count = 0;
    imr_lsm_meta.stats.last_segment_output_mapping_pba_start = 0;
    imr_lsm_meta.stats.last_segment_output_mapping_pba_end = 0;

    if(!segment->output_allocated || !segment->block_table_count){
        imr_lsm_meta.stats.segment_output_mapping_no_output_count++;
        return;
    }

    cursor = segment->output_pba_start;
    for(entry_idx = 0; entry_idx < segment->block_table_count;
        entry_idx++){
        struct imr_lsm_block_entry *entry =
            &segment->block_table[entry_idx];

        entry->output_pba = 0;
        entry->source_pba = 0;
        entry->source_pba_valid = 0;
        entry->output_mapped = 0;
        entry->output_copy_planned = 0;
        entry->output_copied = 0;
        entry->output_committed = 0;
        if(!entry->valid){
            continue;
        }
        if(cursor > segment->output_pba_end){
            break;
        }

        entry->output_pba = cursor;
        entry->output_mapped = 1;
        mapped_entries++;
        cursor += (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    }

    imr_lsm_meta.stats.segment_output_mapping_count++;
    imr_lsm_meta.stats.segment_output_mapping_entry_count += mapped_entries;
    imr_lsm_meta.stats.last_segment_output_mapping_entry_count =
        mapped_entries;
    if(mapped_entries){
        imr_lsm_meta.stats.last_segment_output_mapping_pba_start =
            segment->output_pba_start;
        imr_lsm_meta.stats.last_segment_output_mapping_pba_end =
            cursor - 1;
    }
}

static void imr_lsm_plan_output_copy_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    __u32 planned_segments = 0;
    __u32 planned_entries = 0;
    __u32 missing_mappings = 0;
    __u32 last_segment_id = IMR_LSM_SEGMENT_NONE;
    __u32 last_segment_entries = 0;
    sector_t last_source_start = 0;
    sector_t last_source_end = 0;
    sector_t last_output_start = 0;
    sector_t last_output_end = 0;

    while(segment){
        __u32 entry_idx;
        __u32 segment_entries = 0;
        sector_t segment_source_start = 0;
        sector_t segment_source_end = 0;
        sector_t segment_output_start = 0;
        sector_t segment_output_end = 0;

        if(segment->retired){
            segment = segment->next;
            continue;
        }

        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];
            sector_t source_end;
            sector_t output_end;

            entry->output_copy_planned = 0;
            if(!entry->valid){
                continue;
            }
            if(!entry->output_mapped){
                missing_mappings++;
                continue;
            }

            source_end = entry->pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
            output_end = entry->output_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
            entry->output_copy_planned = 1;
            planned_entries++;
            segment_entries++;

            if(segment_entries == 1){
                segment_source_start = entry->pba;
                segment_source_end = source_end;
                segment_output_start = entry->output_pba;
                segment_output_end = output_end;
            }else{
                if(entry->pba < segment_source_start){
                    segment_source_start = entry->pba;
                }
                if(source_end > segment_source_end){
                    segment_source_end = source_end;
                }
                if(entry->output_pba < segment_output_start){
                    segment_output_start = entry->output_pba;
                }
                if(output_end > segment_output_end){
                    segment_output_end = output_end;
                }
            }
        }

        if(segment_entries){
            planned_segments++;
            last_segment_id = segment->id;
            last_segment_entries = segment_entries;
            last_source_start = segment_source_start;
            last_source_end = segment_source_end;
            last_output_start = segment_output_start;
            last_output_end = segment_output_end;
        }
        segment = segment->next;
    }

    imr_lsm_meta.stats.segment_output_copy_plan_count++;
    imr_lsm_meta.stats.segment_output_copy_plan_entry_count +=
        planned_entries;
    imr_lsm_meta.stats.segment_output_copy_plan_missing_mapping_count +=
        missing_mappings;
    imr_lsm_meta.stats.last_segment_output_copy_plan_segments =
        planned_segments;
    imr_lsm_meta.stats.last_segment_output_copy_plan_entries =
        planned_entries;
    imr_lsm_meta.stats.last_segment_output_copy_plan_missing_mappings =
        missing_mappings;
    imr_lsm_meta.stats.last_segment_output_copy_plan_segment_id =
        last_segment_id;
    imr_lsm_meta.stats.last_segment_output_copy_plan_source_pba_start =
        last_source_start;
    imr_lsm_meta.stats.last_segment_output_copy_plan_source_pba_end =
        last_source_end;
    imr_lsm_meta.stats.last_segment_output_copy_plan_output_pba_start =
        last_output_start;
    imr_lsm_meta.stats.last_segment_output_copy_plan_output_pba_end =
        last_output_end;
}

static void imr_lsm_commit_output_metadata_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    __u32 committed_segments = 0;
    __u32 committed_entries = 0;
    __u32 already_committed = 0;
    __u32 missing_plan = 0;
    __u32 last_segment_id = IMR_LSM_SEGMENT_NONE;
    __u32 last_segment_entries = 0;
    sector_t last_source_start = 0;
    sector_t last_source_end = 0;
    sector_t last_output_start = 0;
    sector_t last_output_end = 0;

    while(segment){
        __u32 entry_idx;
        __u32 segment_entries = 0;
        sector_t segment_source_start = 0;
        sector_t segment_source_end = 0;
        sector_t segment_output_start = 0;
        sector_t segment_output_end = 0;

        if(segment->retired){
            segment = segment->next;
            continue;
        }

        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];
            sector_t source_pba;
            sector_t source_end;
            sector_t output_end;
            __u64 newer_timestamp;
            __u8 newer_valid;

            if(!entry->valid){
                continue;
            }
            if(entry->output_committed){
                already_committed++;
                continue;
            }
            if(!entry->output_mapped || !entry->output_copy_planned){
                missing_plan++;
                continue;
            }

            source_pba = entry->pba;
            source_end = source_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
            output_end = entry->output_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);

            entry->source_pba = source_pba;
            entry->source_pba_valid = 1;
            entry->pba = entry->output_pba;
            entry->output_committed = 1;
            if(!imr_lsm_find_newer_record_locked(entry->key,
                                                 entry->timestamp,
                                                 &newer_timestamp,
                                                 &newer_valid)){
                imr_lsm_tree_update_locked(entry->key, entry->pba,
                                           entry->valid, entry->timestamp);
            }
            committed_entries++;
            segment_entries++;

            if(segment_entries == 1){
                segment_source_start = source_pba;
                segment_source_end = source_end;
                segment_output_start = entry->output_pba;
                segment_output_end = output_end;
            }else{
                if(source_pba < segment_source_start){
                    segment_source_start = source_pba;
                }
                if(source_end > segment_source_end){
                    segment_source_end = source_end;
                }
                if(entry->output_pba < segment_output_start){
                    segment_output_start = entry->output_pba;
                }
                if(output_end > segment_output_end){
                    segment_output_end = output_end;
                }
            }
        }

        if(segment_entries){
            committed_segments++;
            last_segment_id = segment->id;
            last_segment_entries = segment_entries;
            last_source_start = segment_source_start;
            last_source_end = segment_source_end;
            last_output_start = segment_output_start;
            last_output_end = segment_output_end;
        }
        segment = segment->next;
    }

    imr_lsm_meta.stats.segment_output_metadata_commit_count++;
    imr_lsm_meta.stats.segment_output_metadata_commit_entry_count +=
        committed_entries;
    imr_lsm_meta.stats.segment_output_metadata_commit_already_count +=
        already_committed;
    imr_lsm_meta.stats.segment_output_metadata_commit_missing_plan_count +=
        missing_plan;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_segments =
        committed_segments;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_entries =
        committed_entries;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_already =
        already_committed;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_missing_plan =
        missing_plan;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_segment_id =
        last_segment_id;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_source_pba_start =
        last_source_start;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_source_pba_end =
        last_source_end;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_output_pba_start =
        last_output_start;
    imr_lsm_meta.stats.last_segment_output_metadata_commit_output_pba_end =
        last_output_end;
}

static int imr_lsm_copy_output_payload_locked(struct block_device *bdev,
                                              sector_t bdev_start)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    struct page *page;
    void *page_addr;
    __u32 block_bytes;
    __u32 copied_segments = 0;
    __u32 copied_entries = 0;
    __u32 failed_entries = 0;
    __u32 last_segment_id = IMR_LSM_SEGMENT_NONE;
    sector_t last_source_start = 0;
    sector_t last_source_end = 0;
    sector_t last_output_start = 0;
    sector_t last_output_end = 0;
    int last_error = 0;
    int ret = 0;

    if(!bdev){
        ret = -ENODEV;
        goto out_stats;
    }

    block_bytes = ((__u32)1 << IMR_BLOCK_SIZE_SHIFT) <<
                  IMR_SECTOR_SIZE_SHIFT_DEFAULT;
    if(block_bytes > PAGE_SIZE){
        ret = -EOPNOTSUPP;
        goto out_stats;
    }

    page = alloc_page(GFP_NOIO);
    if(!page){
        ret = -ENOMEM;
        goto out_stats;
    }
    page_addr = page_address(page);
    if(!page_addr){
        __free_page(page);
        ret = -ENOMEM;
        goto out_stats;
    }

    while(segment){
        __u32 entry_idx;
        __u32 segment_entries = 0;
        sector_t segment_source_start = 0;
        sector_t segment_source_end = 0;
        sector_t segment_output_start = 0;
        sector_t segment_output_end = 0;

        if(segment->retired){
            segment = segment->next;
            continue;
        }

        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];
            sector_t source_pba;
            sector_t source_end;
            sector_t output_end;

            if(!entry->valid || entry->output_committed ||
               entry->output_copied){
                continue;
            }
            if(!entry->output_mapped || !entry->output_copy_planned){
                continue;
            }

            source_pba = entry->source_pba_valid ?
                entry->source_pba : entry->pba;
            source_end = source_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
            output_end = entry->output_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);

            memset(page_addr, 0, PAGE_SIZE);
            ret = imrsim_read_page(bdev, bdev_start + source_pba,
                                   block_bytes, page);
            if(ret < 0){
                failed_entries++;
                last_error = ret;
                goto copy_done;
            }
            ret = imrsim_write_page(bdev, bdev_start + entry->output_pba,
                                    block_bytes, page);
            if(ret < 0){
                failed_entries++;
                last_error = ret;
                goto copy_done;
            }
            ret = 0;

            entry->source_pba = source_pba;
            entry->source_pba_valid = 1;
            entry->output_copied = 1;
            copied_entries++;
            segment_entries++;

            if(segment_entries == 1){
                segment_source_start = source_pba;
                segment_source_end = source_end;
                segment_output_start = entry->output_pba;
                segment_output_end = output_end;
            }else{
                if(source_pba < segment_source_start){
                    segment_source_start = source_pba;
                }
                if(source_end > segment_source_end){
                    segment_source_end = source_end;
                }
                if(entry->output_pba < segment_output_start){
                    segment_output_start = entry->output_pba;
                }
                if(output_end > segment_output_end){
                    segment_output_end = output_end;
                }
            }
        }

        if(segment_entries){
            copied_segments++;
            last_segment_id = segment->id;
            last_source_start = segment_source_start;
            last_source_end = segment_source_end;
            last_output_start = segment_output_start;
            last_output_end = segment_output_end;
        }
        segment = segment->next;
    }

copy_done:
    __free_page(page);

out_stats:
    if(ret < 0 && !last_error){
        last_error = ret;
    }
    imr_lsm_meta.stats.segment_output_physical_copy_count++;
    imr_lsm_meta.stats.segment_output_physical_copy_entry_count +=
        copied_entries;
    imr_lsm_meta.stats.segment_output_physical_copy_failed_count +=
        failed_entries;
    imr_lsm_meta.stats.last_segment_output_physical_copy_segments =
        copied_segments;
    imr_lsm_meta.stats.last_segment_output_physical_copy_entries =
        copied_entries;
    imr_lsm_meta.stats.last_segment_output_physical_copy_failed =
        failed_entries;
    imr_lsm_meta.stats.last_segment_output_physical_copy_segment_id =
        last_segment_id;
    imr_lsm_meta.stats.last_segment_output_physical_copy_error =
        last_error;
    imr_lsm_meta.stats.last_segment_output_physical_copy_source_pba_start =
        last_source_start;
    imr_lsm_meta.stats.last_segment_output_physical_copy_source_pba_end =
        last_source_end;
    imr_lsm_meta.stats.last_segment_output_physical_copy_output_pba_start =
        last_output_start;
    imr_lsm_meta.stats.last_segment_output_physical_copy_output_pba_end =
        last_output_end;

    return ret;
}

static int imr_lsm_commit_output_locked(__u32 run)
{
    int ret = 0;

    if(!run || run > 3){
        return -EINVAL;
    }

    imr_lsm_plan_output_copy_locked();
    if(run == 3){
        ret = imr_lsm_copy_output_payload_locked(imr_lsm_output_bdev,
                                                 imr_lsm_output_bdev_start);
        if(ret){
            return ret;
        }
    }
    if(run >= 2){
        imr_lsm_commit_output_metadata_locked();
    }

    return 0;
}

static void imr_lsm_record_last_placement_locked(
    const struct imr_lsm_segment *segment)
{
    imr_lsm_meta.stats.last_placement_segment_id = segment->id;
    imr_lsm_meta.stats.last_placement_policy = segment->placement_policy;
    imr_lsm_meta.stats.last_placement_bottom_track_start =
        segment->placement_bottom_track_start;
    imr_lsm_meta.stats.last_placement_bottom_track_end =
        segment->placement_bottom_track_end;
    imr_lsm_meta.stats.last_placement_top_track_start =
        segment->placement_top_track_start;
    imr_lsm_meta.stats.last_placement_top_track_end =
        segment->placement_top_track_end;
    imr_lsm_meta.stats.last_placement_top_pba_start =
        segment->placement_top_pba_start;
    imr_lsm_meta.stats.last_placement_top_pba_end =
        segment->placement_top_pba_end;
    imr_lsm_record_last_placement_output_locked(segment);
}

static void imr_lsm_apply_segment_placement_locked(
    struct imr_lsm_segment *segment)
{
    __u64 zone_key_start;
    __u64 zone_key_end;
    __u64 zone_min_key;
    __u64 zone_max_key;
    __u64 bottom_key_end;
    __u32 bottom_range_blocks = imr_lsm_bottom_range_blocks();
    __u32 group_blocks = imr_lsm_track_group_blocks();
    __u32 top_block_start;
    __u32 top_block_end;

    imr_lsm_meta.stats.placement_policy_count++;

    segment->placement_policy = IMR_LSM_PLACEMENT_NONE;
    segment->placement_target_track_type = 0;
    imr_lsm_reset_segment_output_locked(segment);

    if(segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED){
        imr_lsm_meta.stats.placement_mixed_zone_count++;
        imr_lsm_meta.stats.placement_no_target_count++;
        imr_lsm_note_segment_output_no_target_locked(segment);
        imr_lsm_record_last_placement_locked(segment);
        return;
    }

    zone_key_start = imr_lsm_zone_key_start(segment->zone_idx);
    zone_key_end = zone_key_start + ((__u64)1 << IMR_ZONE_SIZE_SHIFT) - 1;
    if(segment->max_key < zone_key_start || segment->min_key > zone_key_end){
        imr_lsm_meta.stats.placement_no_target_count++;
        imr_lsm_note_segment_output_no_target_locked(segment);
        imr_lsm_record_last_placement_locked(segment);
        return;
    }

    if(segment->min_key > zone_key_start){
        zone_min_key = segment->min_key - zone_key_start;
    }else{
        zone_min_key = 0;
    }
    if(segment->max_key < zone_key_end){
        zone_max_key = segment->max_key - zone_key_start;
    }else{
        zone_max_key = zone_key_end - zone_key_start;
    }
    if(zone_min_key >= bottom_range_blocks){
        imr_lsm_meta.stats.placement_no_target_count++;
        imr_lsm_note_segment_output_no_target_locked(segment);
        imr_lsm_record_last_placement_locked(segment);
        return;
    }

    if(zone_max_key >= bottom_range_blocks){
        zone_max_key = bottom_range_blocks - 1;
    }

    segment->placement_policy = IMR_LSM_PLACEMENT_BOTTOM_TO_TOP;
    segment->placement_target_track_type = IMR_LSM_TRACK_TOP;
    segment->placement_bottom_key_start = zone_key_start + zone_min_key;
    bottom_key_end = zone_key_start + zone_max_key;
    segment->placement_bottom_key_end = bottom_key_end;
    segment->placement_bottom_track_start =
        (__u32)div64_u64(zone_min_key, IMR_BOTTOM_TRACK_SIZE);
    segment->placement_bottom_track_end =
        (__u32)div64_u64(zone_max_key, IMR_BOTTOM_TRACK_SIZE);
    if(segment->placement_bottom_track_start >= TOP_TRACK_NUM_TOTAL){
        segment->placement_bottom_track_start = TOP_TRACK_NUM_TOTAL - 1;
    }
    if(segment->placement_bottom_track_end >= TOP_TRACK_NUM_TOTAL){
        segment->placement_bottom_track_end = TOP_TRACK_NUM_TOTAL - 1;
    }

    segment->placement_top_track_start =
        segment->placement_bottom_track_start;
    segment->placement_top_track_end =
        segment->placement_bottom_track_end;
    top_block_start = segment->placement_top_track_start * group_blocks;
    top_block_end = segment->placement_top_track_end * group_blocks +
                    IMR_TOP_TRACK_SIZE - 1;
    segment->placement_top_pba_start =
        zone_idx_lba(segment->zone_idx) +
        ((__u64)top_block_start << IMR_BLOCK_SIZE_SHIFT);
    segment->placement_top_pba_end =
        zone_idx_lba(segment->zone_idx) +
        ((__u64)top_block_end << IMR_BLOCK_SIZE_SHIFT) +
        (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);

    imr_lsm_meta.stats.placement_bottom_to_top_count++;
    imr_lsm_allocate_segment_output_locked(segment);
    imr_lsm_record_last_placement_locked(segment);
}

static bool imr_lsm_segment_block_table_find(
    const struct imr_lsm_segment *segment, __u64 key,
    struct imr_lsm_block_entry *entry)
{
    __u32 left = 0;
    __u32 right = segment->block_table_count;

    while(left < right){
        __u32 mid = left + ((right - left) / 2);
        struct imr_lsm_block_entry *candidate =
            &segment->block_table[mid];

        if(candidate->key == key){
            *entry = *candidate;
            return true;
        }
        if(candidate->key < key){
            left = mid + 1;
        }else{
            right = mid;
        }
    }

    return false;
}

static void imr_lsm_update_newer_record(__u64 timestamp, __u8 valid,
                                        __u64 *newer_timestamp,
                                        __u8 *newer_valid,
                                        bool *newer_found)
{
    if(timestamp > *newer_timestamp){
        *newer_timestamp = timestamp;
        *newer_valid = valid;
        *newer_found = true;
    }
}

static void imr_lsm_update_latest_record(__u64 timestamp, __u8 valid,
                                         sector_t pba,
                                         __u64 *latest_timestamp,
                                         __u8 *latest_valid,
                                         sector_t *latest_pba,
                                         bool *latest_found)
{
    if(timestamp > *latest_timestamp){
        *latest_timestamp = timestamp;
        *latest_valid = valid;
        *latest_pba = pba;
        *latest_found = true;
    }
}

static bool __maybe_unused
imr_lsm_find_latest_record_locked(__u64 key, __u8 *latest_valid,
                                  sector_t *latest_pba)
{
    struct imr_lsm_segment *segment;
    bool latest_found = false;
    __u64 latest_timestamp = 0;
    __u32 level;

    *latest_valid = 0;
    *latest_pba = 0;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_unsorted_node *node =
            imr_lsm_meta.levels[level].unsorted_head;
        struct imr_lsm_sorted_node *sorted_node =
            imr_lsm_meta.levels[level].sorted_head;

        while(node){
            if(node->key == key){
                imr_lsm_update_latest_record(node->timestamp, node->valid,
                                             node->pba,
                                             &latest_timestamp, latest_valid,
                                             latest_pba,
                                             &latest_found);
            }
            node = node->next;
        }

        while(sorted_node){
            if(sorted_node->key == key){
                imr_lsm_update_latest_record(sorted_node->timestamp,
                                             sorted_node->valid,
                                             sorted_node->pba,
                                             &latest_timestamp, latest_valid,
                                             latest_pba,
                                             &latest_found);
                break;
            }
            if(sorted_node->key > key){
                break;
            }
            sorted_node = sorted_node->next;
        }
    }

    segment = imr_lsm_meta.segment_head;
    while(segment){
        struct imr_lsm_block_entry entry;

        if(segment->retired){
            segment = segment->next;
            continue;
        }
        if(imr_lsm_segment_block_table_find(segment, key, &entry)){
            imr_lsm_update_latest_record(entry.timestamp, entry.valid,
                                         entry.pba,
                                         &latest_timestamp, latest_valid,
                                         latest_pba,
                                         &latest_found);
        }
        segment = segment->next;
    }

    return latest_found;
}

static bool imr_lsm_find_newer_record_scan_locked(__u64 key,
                                                  __u64 timestamp,
                                                  __u64 *newer_timestamp,
                                                  __u8 *newer_valid)
{
    struct imr_lsm_segment *segment;
    bool newer_found = false;
    __u32 level;

    *newer_timestamp = timestamp;
    *newer_valid = 0;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_unsorted_node *node =
            imr_lsm_meta.levels[level].unsorted_head;
        struct imr_lsm_sorted_node *sorted_node =
            imr_lsm_meta.levels[level].sorted_head;

        while(node){
            if(node->key == key && node->timestamp > timestamp){
                imr_lsm_update_newer_record(node->timestamp, node->valid,
                                            newer_timestamp, newer_valid,
                                            &newer_found);
            }
            node = node->next;
        }

        while(sorted_node){
            if(sorted_node->key == key){
                if(sorted_node->timestamp > timestamp){
                    imr_lsm_update_newer_record(sorted_node->timestamp,
                                                sorted_node->valid,
                                                newer_timestamp,
                                                newer_valid,
                                                &newer_found);
                }
                break;
            }
            if(sorted_node->key > key){
                break;
            }
            sorted_node = sorted_node->next;
        }
    }

    segment = imr_lsm_meta.segment_head;
    while(segment){
        struct imr_lsm_block_entry entry;

        if(segment->retired){
            segment = segment->next;
            continue;
        }
        if(imr_lsm_segment_block_table_find(segment, key, &entry) &&
           entry.timestamp > timestamp){
            imr_lsm_update_newer_record(entry.timestamp, entry.valid,
                                        newer_timestamp, newer_valid,
                                        &newer_found);
        }
        segment = segment->next;
    }

    return newer_found;
}

static bool imr_lsm_find_newer_record_locked(__u64 key, __u64 timestamp,
                                             __u64 *newer_timestamp,
                                             __u8 *newer_valid)
{
    struct imr_lsm_newest_node *node;

    atomic64_inc(&imrsim_diag.newest_index_lookup_count);
    if(!imr_lsm_meta.newest_index_valid){
        atomic64_inc(&imrsim_diag.newest_index_fallback_count);
        return imr_lsm_find_newer_record_scan_locked(
            key, timestamp, newer_timestamp, newer_valid);
    }

    node = imr_lsm_newest_index_find_locked(key);
    if(!node){
        /* A zone-compaction entry may be queried while its new segment is
         * still being published.  Preserve correctness with the legacy scan;
         * persistent misses remain visible through the diagnostics. */
        atomic64_inc(&imrsim_diag.newest_index_miss_count);
        atomic64_inc(&imrsim_diag.newest_index_fallback_count);
        return imr_lsm_find_newer_record_scan_locked(
            key, timestamp, newer_timestamp, newer_valid);
    }

    atomic64_inc(&imrsim_diag.newest_index_hit_count);
    *newer_timestamp = timestamp;
    *newer_valid = 0;
    if(node->timestamp <= timestamp){
        return false;
    }

    *newer_timestamp = node->timestamp;
    *newer_valid = node->valid;
    return true;
}

static bool imr_lsm_find_older_valid_record_locked(__u64 key,
                                                   __u64 timestamp)
{
    struct imr_lsm_segment *segment;
    __u32 level;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_unsorted_node *node =
            imr_lsm_meta.levels[level].unsorted_head;
        struct imr_lsm_sorted_node *sorted_node =
            imr_lsm_meta.levels[level].sorted_head;

        while(node){
            if(node->key == key && node->valid &&
               node->timestamp < timestamp){
                return true;
            }
            node = node->next;
        }

        while(sorted_node){
            if(sorted_node->key == key){
                if(sorted_node->valid &&
                   sorted_node->timestamp < timestamp){
                    return true;
                }
                break;
            }
            if(sorted_node->key > key){
                break;
            }
            sorted_node = sorted_node->next;
        }
    }

    segment = imr_lsm_meta.segment_head;
    while(segment){
        struct imr_lsm_block_entry entry;

        if(segment->retired){
            segment = segment->next;
            continue;
        }
        if(imr_lsm_segment_block_table_find(segment, key, &entry) &&
           entry.valid && entry.timestamp < timestamp){
            return true;
        }
        segment = segment->next;
    }

    return false;
}

static bool imr_lsm_zone_map_has_key(__u64 key)
{
    __u32 zone_idx;
    __u64 block_offset;

    if(!zone_status){
        return false;
    }

    zone_idx = (__u32)(key >> IMR_ZONE_SIZE_SHIFT);
    if(zone_idx >= IMR_NUMZONES){
        return false;
    }

    block_offset = key - ((__u64)zone_idx << IMR_ZONE_SIZE_SHIFT);
    if(block_offset >= TOTAL_ITEMS){
        return false;
    }

    return zone_status[zone_idx].z_pba_map[block_offset] != -1;
}

static bool imr_lsm_tombstone_should_keep_locked(
    const struct imr_lsm_block_entry *entry)
{
    /*
     * The persistent logical forward map is still a read-path fallback. Keep the newest
     * tombstone when it is the only thing preventing fallback to old data.
     */
    if(imr_lsm_zone_map_has_key(entry->key)){
        return true;
    }

    return imr_lsm_find_older_valid_record_locked(entry->key,
                                                  entry->timestamp);
}

static bool imr_lsm_segment_entry_should_keep_locked(
    const struct imr_lsm_block_entry *entry)
{
    __u64 newer_timestamp;
    __u8 newer_valid;

    if(imr_lsm_find_newer_record_locked(entry->key, entry->timestamp,
                                        &newer_timestamp, &newer_valid)){
        return false;
    }

    if(entry->valid){
        return true;
    }

    /*
     * A newest tombstone may still be needed to mask older live records.
     * Drop tombstones only when they no longer protect an older version.
     */
    return imr_lsm_tombstone_should_keep_locked(entry);
}

static __u64 imr_lsm_segment_compaction_score(
    const struct imr_lsm_segment *segment)
{
    __u64 score;

    if(segment->retired){
        return 0;
    }
    if(segment->invalid_count < IMR_LSM_COMPACTION_MIN_INVALID){
        return 0;
    }
    if(segment->obsolete_ratio_permille <
       IMR_LSM_COMPACTION_MIN_OBSOLETE_RATIO){
        return 0;
    }

    score = (__u64)segment->obsolete_ratio_permille *
            segment->invalid_count;
    score += (__u64)segment->delete_invalid_count *
             IMR_LSM_COMPACTION_DELETE_BOOST;
    score += (__u64)segment->tombstone_count *
             IMR_LSM_COMPACTION_TOMBSTONE_BOOST;

    return score;
}

static void imr_lsm_detach_live_segment_entry_locked(
    struct imr_lsm_newest_node *owner,
    struct imr_lsm_block_entry *entry)
{
    struct imr_lsm_block_entry **link;

    if(!owner || !entry->newest_linked){
        return;
    }
    link = &owner->live_segment_head;
    while(*link && *link != entry){
        link = &(*link)->newest_next;
    }
    if(*link == entry){
        *link = entry->newest_next;
    }
    entry->newest_next = NULL;
    entry->newest_linked = 0;
}

static void imr_lsm_attach_live_segment_entry_locked(
    struct imr_lsm_newest_node *owner,
    struct imr_lsm_block_entry *entry)
{
    imr_lsm_detach_live_segment_entry_locked(owner, entry);
    entry->newest_next = owner->live_segment_head;
    entry->newest_linked = 1;
    owner->live_segment_head = entry;
}

static void imr_lsm_refresh_segment_invalid_ratio_locked(
    struct imr_lsm_segment *segment)
{
    if(segment->block_table_count){
        segment->obsolete_ratio_permille =
            (__u32)div64_u64((__u64)segment->invalid_count * 1000,
                             segment->block_table_count);
    }else{
        segment->obsolete_ratio_permille = 0;
    }
    segment->compaction_score = imr_lsm_segment_compaction_score(segment);
}

static void imr_lsm_mark_segment_entry_invalid_locked(
    struct imr_lsm_newest_node *owner,
    struct imr_lsm_block_entry *entry, bool obsolete, __u8 newer_valid)
{
    struct imr_lsm_segment *segment = entry->accounting_segment;

    if(!segment || segment->retired ||
       !segment->invalid_stats_accounted){
        imr_lsm_detach_live_segment_entry_locked(owner, entry);
        return;
    }

    if(!entry->accounting_invalid){
        if(segment->live_count){
            segment->live_count--;
        }
        if(!segment->invalid_count){
            imr_lsm_meta.stats.invalid_segment_count++;
        }
        segment->invalid_count++;
        imr_lsm_meta.stats.invalid_entry_count++;
        entry->accounting_invalid = 1;
    }
    if(obsolete && !entry->accounting_obsolete){
        segment->obsolete_count++;
        imr_lsm_meta.stats.obsolete_entry_count++;
        entry->accounting_obsolete = 1;
    }
    if(obsolete && entry->valid && !newer_valid &&
       !entry->accounting_delete_invalid){
        segment->delete_invalid_count++;
        imr_lsm_meta.stats.delete_invalid_entry_count++;
        entry->accounting_delete_invalid = 1;
    }

    imr_lsm_detach_live_segment_entry_locked(owner, entry);
    imr_lsm_refresh_segment_invalid_ratio_locked(segment);
    if(segment->obsolete_ratio_permille >
       imr_lsm_meta.stats.max_obsolete_ratio_permille){
        imr_lsm_meta.stats.max_obsolete_ratio_permille =
            segment->obsolete_ratio_permille;
        imr_lsm_meta.stats.max_obsolete_segment_id = segment->id;
    }
}

static void imr_lsm_incremental_supersede_locked(
    struct imr_lsm_newest_node *node, __u8 newer_valid)
{
    __u64 updated = 0;

    atomic64_inc(&imrsim_diag.invalid_incremental_supersede_count);
    while(node->live_segment_head){
        struct imr_lsm_block_entry *entry = node->live_segment_head;

        imr_lsm_mark_segment_entry_invalid_locked(node, entry, true,
                                                  newer_valid);
        updated++;
    }
    if(updated){
        atomic64_add((s64)updated,
                     &imrsim_diag.invalid_incremental_entries_updated);
    }
}

static void imr_lsm_refresh_max_obsolete_ratio_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    __u32 max_ratio = 0;
    __u32 max_segment_id = 0;

    while(segment){
        if(!segment->retired && segment->invalid_stats_accounted &&
           segment->obsolete_ratio_permille > max_ratio){
            max_ratio = segment->obsolete_ratio_permille;
            max_segment_id = segment->id;
        }
        segment = segment->next;
    }
    imr_lsm_meta.stats.max_obsolete_ratio_permille = max_ratio;
    imr_lsm_meta.stats.max_obsolete_segment_id = max_segment_id;
}

/* Account only the newly published table. Existing entries were updated at
 * newest-index insertion time and are never rescanned on this fast path. */
static bool imr_lsm_account_new_segment_invalid_stats_locked(
    struct imr_lsm_segment *segment)
{
    __u32 entry_idx;

    if(!segment || segment->retired || segment->invalid_stats_accounted){
        return true;
    }
    if(!imr_lsm_meta.newest_index_valid){
        atomic64_inc(&imrsim_diag.invalid_incremental_fallback_recalc_count);
        imr_lsm_recalculate_segment_invalid_stats_locked();
        return false;
    }

    /* Compaction outputs should already exist in the complete newest index.
     * Repair a missing/stale key before changing any aggregate counters. */
    for(entry_idx = 0; entry_idx < segment->block_table_count; entry_idx++){
        struct imr_lsm_block_entry *entry =
            &segment->block_table[entry_idx];
        struct imr_lsm_newest_node *node =
            imr_lsm_newest_index_find_locked(entry->key);

        if(!node || node->timestamp < entry->timestamp){
            if(imr_lsm_newest_index_update_locked(entry->key,
                                                  entry->timestamp,
                                                  entry->valid)){
                atomic64_inc(
                    &imrsim_diag.invalid_incremental_fallback_recalc_count);
                imr_lsm_recalculate_segment_invalid_stats_locked();
                return false;
            }
        }
    }

    segment->live_count = 0;
    segment->invalid_count = 0;
    segment->obsolete_count = 0;
    segment->tombstone_count = 0;
    segment->delete_invalid_count = 0;
    segment->obsolete_ratio_permille = 0;
    for(entry_idx = 0; entry_idx < segment->block_table_count; entry_idx++){
        struct imr_lsm_block_entry *entry =
            &segment->block_table[entry_idx];
        struct imr_lsm_newest_node *node =
            imr_lsm_newest_index_find_locked(entry->key);
        bool invalid = false;

        entry->accounting_segment = segment;
        entry->accounting_invalid = 0;
        entry->accounting_obsolete = 0;
        entry->accounting_delete_invalid = 0;
        entry->newest_next = NULL;
        entry->newest_linked = 0;

        if(!entry->valid){
            segment->tombstone_count++;
            if(!imr_lsm_tombstone_should_keep_locked(entry)){
                invalid = true;
            }
        }
        if(node->timestamp > entry->timestamp){
            segment->obsolete_count++;
            entry->accounting_obsolete = 1;
            invalid = true;
            if(entry->valid && !node->valid){
                segment->delete_invalid_count++;
                entry->accounting_delete_invalid = 1;
            }
        }

        if(invalid){
            segment->invalid_count++;
            entry->accounting_invalid = 1;
        }else{
            segment->live_count++;
            if(node->timestamp == entry->timestamp){
                imr_lsm_attach_live_segment_entry_locked(node, entry);
            }
        }
    }

    segment->invalid_stats_accounted = 1;
    imr_lsm_refresh_segment_invalid_ratio_locked(segment);
    if(segment->invalid_count){
        imr_lsm_meta.stats.invalid_segment_count++;
    }
    imr_lsm_meta.stats.invalid_entry_count += segment->invalid_count;
    imr_lsm_meta.stats.obsolete_entry_count += segment->obsolete_count;
    imr_lsm_meta.stats.tombstone_entry_count += segment->tombstone_count;
    imr_lsm_meta.stats.delete_invalid_entry_count +=
        segment->delete_invalid_count;
    if(segment->obsolete_ratio_permille >
       imr_lsm_meta.stats.max_obsolete_ratio_permille){
        imr_lsm_meta.stats.max_obsolete_ratio_permille =
            segment->obsolete_ratio_permille;
        imr_lsm_meta.stats.max_obsolete_segment_id = segment->id;
    }
    atomic64_inc(&imrsim_diag.invalid_incremental_segment_publish_count);
    atomic64_add((s64)segment->block_table_count,
                 &imrsim_diag.invalid_incremental_segment_publish_entries);
    return true;
}

static void imr_lsm_unaccount_segment_invalid_stats_locked(
    struct imr_lsm_segment *segment)
{
    __u32 entry_idx;

    if(!segment || !segment->invalid_stats_accounted){
        return;
    }
    for(entry_idx = 0; entry_idx < segment->block_table_count; entry_idx++){
        struct imr_lsm_block_entry *entry =
            &segment->block_table[entry_idx];
        struct imr_lsm_newest_node *node = NULL;

        if(entry->newest_linked){
            node = imr_lsm_newest_index_find_locked(entry->key);
        }
        imr_lsm_detach_live_segment_entry_locked(node, entry);
        entry->accounting_segment = NULL;
    }
    if(segment->invalid_count &&
       imr_lsm_meta.stats.invalid_segment_count){
        imr_lsm_meta.stats.invalid_segment_count--;
    }
    imr_lsm_meta.stats.invalid_entry_count -= segment->invalid_count;
    imr_lsm_meta.stats.obsolete_entry_count -= segment->obsolete_count;
    imr_lsm_meta.stats.tombstone_entry_count -= segment->tombstone_count;
    imr_lsm_meta.stats.delete_invalid_entry_count -=
        segment->delete_invalid_count;
    segment->invalid_stats_accounted = 0;
    atomic64_inc(&imrsim_diag.invalid_incremental_segment_retire_count);
    atomic64_add((s64)segment->block_table_count,
                 &imrsim_diag.invalid_incremental_segment_retire_entries);
}

static void imr_lsm_reevaluate_latest_tombstones_locked(
    const struct imr_lsm_segment *retired_segment)
{
    __u32 entry_idx;

    for(entry_idx = 0; entry_idx < retired_segment->block_table_count;
        entry_idx++){
        const struct imr_lsm_block_entry *retired_entry =
            &retired_segment->block_table[entry_idx];
        struct imr_lsm_newest_node *node =
            imr_lsm_newest_index_find_locked(retired_entry->key);
        struct imr_lsm_block_entry *entry;

        if(!node){
            continue;
        }
        entry = node->live_segment_head;
        while(entry){
            struct imr_lsm_block_entry *next = entry->newest_next;

            if(!entry->valid &&
               !imr_lsm_tombstone_should_keep_locked(entry)){
                imr_lsm_mark_segment_entry_invalid_locked(node, entry,
                                                          false, 0);
                atomic64_inc(
                    &imrsim_diag.invalid_incremental_entries_updated);
            }
            entry = next;
        }
    }
}

struct imr_lsm_compaction_policy_score {
    __u64 age;
    __u64 read_hotness;
    __u32 placement_cost;
    __u32 rmw_cost;
    __u32 zone_fullness;
    __s64 final_score;
};

static __u64 imr_lsm_segment_age_locked(const struct imr_lsm_segment *segment)
{
    if(imr_lsm_meta.timestamp <= segment->max_timestamp){
        return 0;
    }

    return imr_lsm_meta.timestamp - segment->max_timestamp;
}

static __u32 imr_lsm_segment_placement_cost_permille(
    const struct imr_lsm_segment *segment)
{
    sector_t target_sectors;
    sector_t output_sectors;
    __u64 cost;

    if(segment->placement_policy != IMR_LSM_PLACEMENT_BOTTOM_TO_TOP ||
       segment->placement_target_track_type != IMR_LSM_TRACK_TOP ||
       segment->placement_top_pba_start > segment->placement_top_pba_end){
        return 1000;
    }

    if(!segment->output_allocated ||
       segment->output_pba_start > segment->output_pba_end){
        return 1000;
    }

    target_sectors =
        segment->placement_top_pba_end - segment->placement_top_pba_start + 1;
    output_sectors = segment->output_pba_end - segment->output_pba_start + 1;
    if(!target_sectors){
        return 1000;
    }

    cost = div64_u64((__u64)output_sectors * 1000, target_sectors);
    if(cost > 1000){
        return 1000;
    }

    return (__u32)cost;
}

static __u32 imr_lsm_segment_rmw_cost_permille(
    const struct imr_lsm_segment *segment)
{
    __u32 entries = segment->block_table_count ?
        segment->block_table_count : segment->node_count;

    if(!entries){
        return 0;
    }

    if(segment->track_type == IMR_LSM_TRACK_TOP){
        return 0;
    }

    if(segment->track_type != IMR_LSM_TRACK_BOTTOM){
        return 500;
    }

    return (__u32)div64_u64((__u64)segment->live_count * 1000, entries);
}

static __u32 imr_lsm_segment_zone_fullness_permille(
    const struct imr_lsm_segment *segment)
{
    __u32 zone_blocks;
    __u64 fullness;

    if(!zone_status || segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED ||
       segment->zone_idx >= IMR_NUMZONES){
        return 0;
    }

    zone_blocks = zone_status[segment->zone_idx].z_length >>
                  IMR_BLOCK_SIZE_SHIFT;
    if(!zone_blocks){
        zone_blocks = TOTAL_ITEMS;
    }
    if(!zone_blocks){
        return 0;
    }

    fullness = div64_u64((__u64)zone_status[segment->zone_idx].z_map_size *
                         1000, zone_blocks);
    if(fullness > 1000){
        return 1000;
    }

    return (__u32)fullness;
}

static void imr_lsm_segment_policy_score_locked(
    const struct imr_lsm_segment *segment,
    struct imr_lsm_compaction_policy_score *policy)
{
    policy->age = imr_lsm_segment_age_locked(segment);
    policy->read_hotness = segment->read_hit_count;
    policy->placement_cost =
        imr_lsm_segment_placement_cost_permille(segment);
    policy->rmw_cost = imr_lsm_segment_rmw_cost_permille(segment);
    policy->zone_fullness =
        imr_lsm_segment_zone_fullness_permille(segment);

    policy->final_score = (__s64)segment->compaction_score;
    policy->final_score += (__s64)policy->age *
        IMR_LSM_COMPACTION_POLICY_AGE_WEIGHT;
    policy->final_score += (__s64)policy->read_hotness *
        IMR_LSM_COMPACTION_POLICY_HOTNESS_WEIGHT;
    policy->final_score += (__s64)policy->placement_cost *
        IMR_LSM_COMPACTION_POLICY_PLACEMENT_WEIGHT;
    policy->final_score += (__s64)policy->rmw_cost *
        IMR_LSM_COMPACTION_POLICY_RMW_WEIGHT;
    policy->final_score += (__s64)policy->zone_fullness *
        IMR_LSM_COMPACTION_POLICY_ZONE_FULLNESS_WEIGHT;
}

static void imr_lsm_update_segment_compaction_selection_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    struct imr_lsm_segment *best_segment = NULL;
    __s64 best_final_score = 0;
    __u64 candidate_count = 0;

    while(segment){
        struct imr_lsm_compaction_policy_score policy;

        segment->compaction_score =
            imr_lsm_segment_compaction_score(segment);
        segment->compaction_candidate =
            segment->compaction_score ? 1 : 0;

        if(segment->compaction_candidate){
            imr_lsm_segment_policy_score_locked(segment, &policy);
            candidate_count++;
            if(!best_segment ||
               policy.final_score > best_final_score ||
               (policy.final_score == best_final_score &&
                segment->compaction_score >
                best_segment->compaction_score) ||
               (policy.final_score == best_final_score &&
                segment->compaction_score ==
                best_segment->compaction_score &&
                segment->obsolete_ratio_permille >
                best_segment->obsolete_ratio_permille) ||
               (policy.final_score == best_final_score &&
                segment->compaction_score ==
                best_segment->compaction_score &&
                segment->obsolete_ratio_permille ==
                best_segment->obsolete_ratio_permille &&
                segment->invalid_count > best_segment->invalid_count)){
                best_segment = segment;
                best_final_score = policy.final_score;
            }
        }
        segment = segment->next;
    }

    imr_lsm_meta.stats.segment_compaction_selection_count++;
    imr_lsm_meta.stats.segment_compaction_candidate_count = candidate_count;
    if(best_segment){
        imr_lsm_meta.stats.segment_compaction_candidate_segment_id =
            best_segment->id;
        imr_lsm_meta.stats.segment_compaction_candidate_level =
            best_segment->level;
        imr_lsm_meta.stats.segment_compaction_candidate_score =
            best_segment->compaction_score;
        imr_lsm_meta.stats.segment_compaction_candidate_ratio_permille =
            best_segment->obsolete_ratio_permille;
        imr_lsm_meta.stats.segment_compaction_candidate_invalid_count =
            best_segment->invalid_count;
        imr_lsm_meta.stats.segment_compaction_candidate_delete_invalid_count =
            best_segment->delete_invalid_count;
        imr_lsm_meta.stats.segment_compaction_candidate_tombstone_count =
            best_segment->tombstone_count;
    }else{
        imr_lsm_meta.stats.segment_compaction_no_candidate_count++;
        imr_lsm_meta.stats.segment_compaction_candidate_segment_id = 0;
        imr_lsm_meta.stats.segment_compaction_candidate_level =
            IMR_LSM_LEVELS;
        imr_lsm_meta.stats.segment_compaction_candidate_score = 0;
        imr_lsm_meta.stats.segment_compaction_candidate_ratio_permille = 0;
        imr_lsm_meta.stats.segment_compaction_candidate_invalid_count = 0;
        imr_lsm_meta.stats.segment_compaction_candidate_delete_invalid_count =
            0;
        imr_lsm_meta.stats.segment_compaction_candidate_tombstone_count = 0;
    }
}

static void imr_lsm_recalculate_segment_invalid_stats_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    struct rb_node *rb;
    __u64 recalc_start_ns = imrsim_diag_now_ns();
    __u32 segment_count = 0;
    __u32 entry_count = 0;
    __u64 invalid_segment_count = 0;
    __u64 invalid_entry_count = 0;
    __u64 obsolete_entry_count = 0;
    __u64 tombstone_entry_count = 0;
    __u64 delete_invalid_entry_count = 0;
    __u32 max_ratio = 0;
    __u32 max_ratio_segment_id = 0;

    for(rb = rb_first(&imr_lsm_meta.newest_tree); rb; rb = rb_next(rb)){
        struct imr_lsm_newest_node *node =
            rb_entry(rb, struct imr_lsm_newest_node, rb);

        node->live_segment_head = NULL;
    }
    while(segment){
        __u32 entry_idx;

        segment->invalid_stats_accounted = 0;
        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];

            entry->accounting_segment = NULL;
            entry->newest_next = NULL;
            entry->newest_linked = 0;
            entry->accounting_invalid = 0;
            entry->accounting_obsolete = 0;
            entry->accounting_delete_invalid = 0;
        }
        if(segment->retired){
            segment_count++;
            segment = segment->next;
            continue;
        }

        segment->live_count = 0;
        segment->invalid_count = 0;
        segment->obsolete_count = 0;
        segment->tombstone_count = 0;
        segment->delete_invalid_count = 0;
        segment->obsolete_ratio_permille = 0;

        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];
            __u64 newer_timestamp;
            __u8 newer_valid;
            bool newer_found;
            bool invalid = false;

            entry_count++;
            if(!entry->valid){
                segment->tombstone_count++;
                tombstone_entry_count++;
                if(!imr_lsm_tombstone_should_keep_locked(entry)){
                    invalid = true;
                }
            }

            newer_found = imr_lsm_find_newer_record_locked(entry->key,
                                                           entry->timestamp,
                                                           &newer_timestamp,
                                                           &newer_valid);
            if(newer_found){
                segment->obsolete_count++;
                obsolete_entry_count++;
                entry->accounting_obsolete = 1;
                invalid = true;
                if(entry->valid && !newer_valid){
                    segment->delete_invalid_count++;
                    delete_invalid_entry_count++;
                    entry->accounting_delete_invalid = 1;
                }
            }

            if(invalid){
                segment->invalid_count++;
                invalid_entry_count++;
                entry->accounting_invalid = 1;
            }else{
                struct imr_lsm_newest_node *node = NULL;

                segment->live_count++;
                if(imr_lsm_meta.newest_index_valid){
                    node = imr_lsm_newest_index_find_locked(entry->key);
                }
                if(node && node->timestamp == entry->timestamp){
                    imr_lsm_attach_live_segment_entry_locked(node, entry);
                }
            }
            entry->accounting_segment = segment;
        }

        if(segment->block_table_count){
            segment->obsolete_ratio_permille =
                (__u32)div64_u64((__u64)segment->invalid_count * 1000,
                                 segment->block_table_count);
        }
        if(segment->invalid_count){
            invalid_segment_count++;
        }
        if(segment->obsolete_ratio_permille > max_ratio){
            max_ratio = segment->obsolete_ratio_permille;
            max_ratio_segment_id = segment->id;
        }
        segment->invalid_stats_accounted = 1;

        segment_count++;
        segment = segment->next;
    }

    imr_lsm_meta.stats.invalid_recalc_count++;
    imr_lsm_meta.stats.invalid_segment_count = invalid_segment_count;
    imr_lsm_meta.stats.invalid_entry_count = invalid_entry_count;
    imr_lsm_meta.stats.obsolete_entry_count = obsolete_entry_count;
    imr_lsm_meta.stats.tombstone_entry_count = tombstone_entry_count;
    imr_lsm_meta.stats.delete_invalid_entry_count =
        delete_invalid_entry_count;
    imr_lsm_meta.stats.max_obsolete_ratio_permille = max_ratio;
    imr_lsm_meta.stats.max_obsolete_segment_id = max_ratio_segment_id;
    imr_lsm_meta.stats.last_invalid_recalc_segments = segment_count;
    imr_lsm_meta.stats.last_invalid_recalc_entries = entry_count;
    imr_lsm_update_segment_compaction_selection_locked();
    imrsim_diag_record_duration(
        NULL, &imrsim_diag.invalid_recalc_total_ns,
        &imrsim_diag.invalid_recalc_max_ns,
        &imrsim_diag.last_invalid_recalc_ns,
        imrsim_diag_elapsed_ns(recalc_start_ns));
    atomic64_add((s64)segment_count,
                 &imrsim_diag.invalid_recalc_segments_scanned_total);
    imrsim_diag_set_max(&imrsim_diag.invalid_recalc_segments_scanned_max,
                        segment_count);
    atomic64_add((s64)entry_count,
                 &imrsim_diag.invalid_recalc_entries_scanned_total);
    imrsim_diag_set_max(&imrsim_diag.invalid_recalc_entries_scanned_max,
                        entry_count);
}

static struct imr_lsm_segment *
imr_lsm_selected_segment_compaction_candidate_locked(void)
{
    struct imr_lsm_segment *segment;

    if(!imr_lsm_meta.stats.segment_compaction_candidate_score){
        return NULL;
    }

    segment = imr_lsm_meta.segment_head;
    while(segment){
        if(!segment->retired &&
           segment->id ==
           imr_lsm_meta.stats.segment_compaction_candidate_segment_id){
            return segment;
        }
        segment = segment->next;
    }

    return NULL;
}

static int imr_lsm_compact_selected_segment_locked(void)
{
    struct imr_lsm_segment *segment;
    struct imr_lsm_segment_builder segment_builder = {0};
    __u32 input_entries;
    __u32 live_entries = 0;
    __u32 dropped_entries = 0;
    __u32 new_segment_id = IMR_LSM_SEGMENT_NONE;
    __u32 entry_idx;
    bool compacted_to_segment = false;
    int ret;

    imr_lsm_recalculate_segment_invalid_stats_locked();
    segment = imr_lsm_selected_segment_compaction_candidate_locked();
    if(!segment){
        imr_lsm_meta.stats.segment_compaction_execute_no_candidate_count++;
        imr_lsm_meta.stats.last_segment_compaction_from_id =
            IMR_LSM_SEGMENT_NONE;
        imr_lsm_meta.stats.last_segment_compaction_to_id =
            IMR_LSM_SEGMENT_NONE;
        imr_lsm_meta.stats.last_segment_compaction_input_entries = 0;
        imr_lsm_meta.stats.last_segment_compaction_live_entries = 0;
        imr_lsm_meta.stats.last_segment_compaction_dropped_entries = 0;
        printk(KERN_INFO "imrsim: IMR-LSM segment compaction no candidate\n");
        return 0;
    }

    input_entries = segment->block_table_count;
    for(entry_idx = 0; entry_idx < segment->block_table_count;
        entry_idx++){
        struct imr_lsm_block_entry *entry =
            &segment->block_table[entry_idx];

        if(!imr_lsm_segment_entry_should_keep_locked(entry)){
            dropped_entries++;
            continue;
        }

        ret = imr_lsm_segment_builder_add(&segment_builder, entry->key,
                                          entry->pba, segment->zone_idx,
                                          entry->valid,
                                          entry->timestamp);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM segment compaction table alloc failed segment=%u\n",
                   segment->id);
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        live_entries++;
    }

    if(segment_builder.node_count){
        new_segment_id = imr_lsm_meta.next_segment_id;
        ret = imr_lsm_append_segment_locked(segment->level,
                                            segment->track_type,
                                            &segment_builder);
        if(ret){
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        compacted_to_segment = true;
    }else{
        imr_lsm_segment_builder_release(&segment_builder);
    }

    imr_lsm_unaccount_segment_invalid_stats_locked(segment);
    segment->retired = 1;
    segment->compaction_candidate = 0;
    segment->compaction_score = 0;
    imr_lsm_reevaluate_latest_tombstones_locked(segment);

    imr_lsm_meta.stats.segment_compaction_execute_count++;
    imr_lsm_meta.stats.last_segment_compaction_from_id = segment->id;
    imr_lsm_meta.stats.last_segment_compaction_to_id = new_segment_id;
    imr_lsm_meta.stats.last_segment_compaction_input_entries = input_entries;
    imr_lsm_meta.stats.last_segment_compaction_live_entries = live_entries;
    imr_lsm_meta.stats.last_segment_compaction_dropped_entries =
        dropped_entries;

    imr_lsm_refresh_max_obsolete_ratio_locked();
    imr_lsm_update_segment_compaction_selection_locked();
    if(compacted_to_segment){
        ret = imr_lsm_commit_output_locked(3);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM selected segment output commit failed old=%u new=%u ret=%d\n",
                   segment->id,
                   new_segment_id,
                   ret);
            return ret;
        }
    }
    printk(KERN_INFO "imrsim: IMR-LSM segment compacted old=%u new=%u input=%u live=%u dropped=%u\n",
           segment->id,
           new_segment_id,
           input_entries,
           live_entries,
           dropped_entries);

    return 0;
}

static void imr_lsm_clear_zone_top_usage(__u32 zone_idx)
{
    __u32 track;

    for(track = 0; track < TOP_TRACK_NUM_TOTAL; track++){
        memset(zone_status[zone_idx].z_tracks[track].isUsedBlock, 0,
               IMR_TOP_TRACK_SIZE * sizeof(__u8));
    }
}

static void imr_lsm_mark_zone_top_full(__u32 zone_idx)
{
    __u32 track;

    for(track = 0; track < TOP_TRACK_NUM_TOTAL; track++){
        memset(zone_status[zone_idx].z_tracks[track].isUsedBlock, 1,
               IMR_TOP_TRACK_SIZE * sizeof(__u8));
    }
}

static void imr_lsm_clear_zone_compaction_auto_pending_locked(__u32 zone_idx)
{
    if(zone_idx != IMR_LSM_ZONE_COMPACTION_NONE &&
       imr_lsm_meta.zone_compaction_auto_pending_zone != zone_idx){
        return;
    }

    WRITE_ONCE(imr_lsm_meta.zone_compaction_auto_pending, 0);
    imr_lsm_meta.zone_compaction_auto_pending_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
}

static void imr_lsm_clear_zone_compaction_candidate_locked(__u32 zone_idx)
{
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone != zone_idx){
        return;
    }

    imr_lsm_meta.stats.zone_compaction_candidate_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.zone_compaction_candidate_dest_zone =
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.zone_compaction_candidate_map_size = 0;
    imr_lsm_meta.stats.zone_compaction_candidate_reclaimable = 0;
    imr_lsm_meta.stats.zone_compaction_candidate_ratio_permille = 0;
    imr_lsm_meta.stats.zone_compaction_candidate_pressure = 0;
    imr_lsm_meta.stats.zone_compaction_candidate_ready = 0;
    imr_lsm_clear_zone_compaction_auto_pending_locked(zone_idx);
}

static void imr_lsm_record_zone_compaction_candidate_locked(__u32 zone_idx)
{
    __u32 source_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    __u32 dest_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    __u32 best_reclaimable = 0;
    __u32 best_ratio_permille = 0;
    __u32 free_count;
    __u32 current_source;
    __u32 current_dest;
    __u64 oldest_generation = (__u64)~0ULL;
    bool pressure;
    bool new_candidate;
    bool candidate_changed;

    (void)zone_idx;
    if(!zone_status){
        return;
    }

    free_count = imr_lsm_count_free_zones_locked();
    pressure = free_count < imr_lsm_zone_gc_free_low_watermark;

    for(zone_idx = 0; zone_idx < IMR_NUMZONES; zone_idx++){
        struct imrsim_zone_status *status = &zone_status[zone_idx];
        __u32 reclaimable;
        __u32 ratio_permille;

        if(zone_idx == imr_lsm_allocator.active_zone ||
           (status->z_conds != Z_COND_FULL &&
            status->z_conds != Z_COND_CLOSED) ||
           !status->z_map_size ||
           status->z_live_count >= status->z_map_size){
            continue;
        }
        reclaimable = status->z_map_size - status->z_live_count;
        ratio_permille = (__u32)div64_u64(
            (__u64)reclaimable * IMR_LSM_ZONE_GC_RATIO_SCALE,
            status->z_map_size);
        if(!pressure &&
           ratio_permille <
               imr_lsm_zone_gc_min_invalid_ratio_permille){
            continue;
        }
        if(source_zone == IMR_LSM_ZONE_COMPACTION_NONE ||
           ratio_permille > best_ratio_permille ||
           (ratio_permille == best_ratio_permille &&
            reclaimable > best_reclaimable) ||
           (ratio_permille == best_ratio_permille &&
            reclaimable == best_reclaimable &&
            status->z_generation < oldest_generation)){
            source_zone = zone_idx;
            best_reclaimable = reclaimable;
            best_ratio_permille = ratio_permille;
            oldest_generation = status->z_generation;
        }
    }

    if(source_zone != IMR_LSM_ZONE_COMPACTION_NONE){
        current_source =
            imr_lsm_meta.stats.zone_compaction_candidate_zone;
        current_dest =
            imr_lsm_meta.stats.zone_compaction_candidate_dest_zone;

        if(current_source == source_zone && current_dest != source_zone &&
           current_dest < IMR_NUMZONES &&
           imr_lsm_zone_is_free_locked(current_dest)){
            dest_zone = current_dest;
        }else{
            dest_zone = imr_lsm_find_free_zone_locked(source_zone);
        }
    }

    new_candidate =
        imr_lsm_meta.stats.zone_compaction_candidate_zone != source_zone;
    candidate_changed = new_candidate ||
        imr_lsm_meta.stats.zone_compaction_candidate_dest_zone != dest_zone ||
        imr_lsm_meta.stats.zone_compaction_candidate_pressure !=
            (pressure ? 1 : 0);
    if(new_candidate && source_zone != IMR_LSM_ZONE_COMPACTION_NONE){
        imr_lsm_meta.stats.zone_compaction_candidate_count++;
    }

    imr_lsm_meta.stats.zone_gc_free_zone_count = free_count;
    imr_lsm_meta.stats.zone_gc_pressure = pressure ? 1 : 0;
    imr_lsm_meta.stats.zone_compaction_candidate_zone = source_zone;
    imr_lsm_meta.stats.zone_compaction_candidate_dest_zone =
        dest_zone;
    imr_lsm_meta.stats.zone_compaction_candidate_map_size =
        source_zone == IMR_LSM_ZONE_COMPACTION_NONE ? 0 :
        zone_status[source_zone].z_map_size;
    imr_lsm_meta.stats.zone_compaction_candidate_reclaimable =
        source_zone == IMR_LSM_ZONE_COMPACTION_NONE ? 0 :
        best_reclaimable;
    imr_lsm_meta.stats.zone_compaction_candidate_ratio_permille =
        source_zone == IMR_LSM_ZONE_COMPACTION_NONE ? 0 :
        best_ratio_permille;
    imr_lsm_meta.stats.zone_compaction_candidate_pressure =
        source_zone == IMR_LSM_ZONE_COMPACTION_NONE ? 0 :
        (pressure ? 1 : 0);
    imr_lsm_meta.stats.zone_compaction_candidate_ready =
        dest_zone != IMR_LSM_ZONE_COMPACTION_NONE;

    imr_lsm_meta.stats.last_zone_compaction_candidate_zone = source_zone;
    imr_lsm_meta.stats.last_zone_compaction_candidate_dest_zone =
        dest_zone;
    imr_lsm_meta.stats.last_zone_compaction_candidate_map_size =
        imr_lsm_meta.stats.zone_compaction_candidate_map_size;
    imr_lsm_meta.stats.last_zone_compaction_candidate_reclaimable =
        imr_lsm_meta.stats.zone_compaction_candidate_reclaimable;
    imr_lsm_meta.stats.last_zone_compaction_candidate_ratio_permille =
        imr_lsm_meta.stats.zone_compaction_candidate_ratio_permille;
    imr_lsm_meta.stats.last_zone_compaction_candidate_pressure =
        imr_lsm_meta.stats.zone_compaction_candidate_pressure;
    imr_lsm_meta.stats.last_zone_compaction_candidate_ready =
        imr_lsm_meta.stats.zone_compaction_candidate_ready;

    if(source_zone == IMR_LSM_ZONE_COMPACTION_NONE){
        imr_lsm_clear_zone_compaction_auto_pending_locked(
            IMR_LSM_ZONE_COMPACTION_NONE);
        return;
    }

    if(candidate_changed){
        printk(KERN_INFO "imrsim: IMR-LSM zone compaction candidate zone=%u dest=%u map_size=%u live=%u reclaimable=%u ratio_permille=%u free=%u pressure=%u ready=%u\n",
               source_zone, dest_zone,
               zone_status[source_zone].z_map_size,
               zone_status[source_zone].z_live_count,
               best_reclaimable,
               best_ratio_permille,
               free_count, pressure ? 1 : 0,
               dest_zone != IMR_LSM_ZONE_COMPACTION_NONE);
    }
}

static bool imr_lsm_defer_zone_compaction_auto_run_locked(__u32 zone_idx)
{
    if(!imr_lsm_meta.zone_compaction_auto_run ||
       imr_lsm_meta.zone_compaction_auto_running){
        return false;
    }
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone != zone_idx ||
       !imr_lsm_meta.stats.zone_compaction_candidate_ready){
        return false;
    }
    if(imr_lsm_meta.zone_compaction_auto_pending &&
       imr_lsm_meta.zone_compaction_auto_pending_zone == zone_idx){
        return false;
    }

    imr_lsm_meta.zone_compaction_auto_pending_zone = zone_idx;
    WRITE_ONCE(imr_lsm_meta.zone_compaction_auto_pending, 1);
    imr_lsm_meta.stats.zone_compaction_auto_pending_count++;
    imr_lsm_meta.stats.last_zone_compaction_auto_pending_zone = zone_idx;

    printk(KERN_INFO "imrsim: IMR-LSM auto zone compaction pending zone=%u\n",
           zone_idx);
    return true;
}

static void imr_lsm_record_zone_compaction_candidate(__u32 zone_idx)
{
    __u32 candidate;

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_record_zone_compaction_candidate_locked(zone_idx);
    candidate = imr_lsm_meta.stats.zone_compaction_candidate_zone;
    if(candidate != IMR_LSM_ZONE_COMPACTION_NONE){
        imr_lsm_defer_zone_compaction_auto_run_locked(candidate);
    }
    mutex_unlock(&imr_lsm_lock);
}

static void imr_lsm_queue_zone_compaction_auto_work(void)
{
    struct workqueue_struct *wq = READ_ONCE(imr_lsm_zone_compaction_wq);

    if(wq){
        queue_work(wq, &imr_lsm_zone_compaction_work);
    }
}

static void imr_lsm_zone_compaction_auto_work(struct work_struct *work)
{
    __u32 zone_idx;
    bool run = false;
    bool queue_next = false;
    int ret;

    (void)work;

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        goto out_zone;
    }
    if(atomic_read(&imr_lsm_append_writes_inflight) > 0){
        /* The last reservation may have sealed its zone before earlier
         * remapped append bios completed.  The final end_io requeues us. */
        goto out_zone;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized ||
       !imr_lsm_meta.zone_compaction_auto_run ||
       !imr_lsm_meta.zone_compaction_auto_pending){
        goto out;
    }

    zone_idx = imr_lsm_meta.zone_compaction_auto_pending_zone;
    imr_lsm_clear_zone_compaction_auto_pending_locked(zone_idx);
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone == zone_idx &&
       imr_lsm_meta.stats.zone_compaction_candidate_ready &&
       !imr_lsm_meta.zone_compaction_auto_running){
        imr_lsm_meta.zone_compaction_auto_running = 1;
        imr_lsm_meta.stats.last_zone_compaction_auto_run_zone = zone_idx;
        run = true;
    }

out:
    mutex_unlock(&imr_lsm_lock);
out_zone:
    mutex_unlock(&imrsim_zone_lock);

    if(!run){
        return;
    }

    ret = imr_lsm_compact_zone(zone_idx);

    mutex_lock(&imr_lsm_lock);
    imr_lsm_meta.zone_compaction_auto_running = 0;
    if(ret == -EBUSY){
        /* A foreground append can start after the worker's initial inflight
         * check but before compact_zone takes imrsim_zone_lock.  Preserve the
         * current candidate and retry after append I/O becomes idle instead
         * of reporting this expected race as a failed GC. */
        imr_lsm_meta.stats.zone_compaction_auto_busy_retry_count++;
        printk(KERN_INFO "imrsim: IMR-LSM auto zone compaction retry zone=%u ret=%d\n",
               zone_idx, ret);
    }else if(ret){
        imr_lsm_meta.stats.last_zone_compaction_auto_run_error = ret;
        imr_lsm_meta.stats.zone_compaction_auto_run_failed_count++;
        printk(KERN_ERR "imrsim: IMR-LSM auto zone compaction failed zone=%u ret=%d\n",
               zone_idx, ret);
    }else{
        imr_lsm_meta.stats.last_zone_compaction_auto_run_error = 0;
        imr_lsm_meta.stats.zone_compaction_auto_run_count++;
        printk(KERN_INFO "imrsim: IMR-LSM auto zone compact zone=%u\n",
               zone_idx);
    }
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone !=
           IMR_LSM_ZONE_COMPACTION_NONE &&
       (ret == -EBUSY ||
        imr_lsm_meta.stats.zone_compaction_candidate_zone != zone_idx)){
        queue_next = imr_lsm_defer_zone_compaction_auto_run_locked(
            imr_lsm_meta.stats.zone_compaction_candidate_zone);
    }
    mutex_unlock(&imr_lsm_lock);
    if(queue_next){
        imr_lsm_queue_zone_compaction_auto_work();
    }
}

/* Caller holds imrsim_zone_lock. */
static void imr_lsm_allocator_reset_locked(void)
{
    imr_lsm_allocator.active_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_allocator.free_cursor = 0;
    imr_lsm_allocator.next_generation = 1;
    atomic_set(&imr_lsm_append_writes_inflight, 0);
}

/*
 * Recover the one open foreground zone from persistent per-zone state.  A
 * crash can leave more than one OPEN marker; only the newest generation is
 * resumed and the older zones are sealed before new reservations begin.
 * Caller holds imrsim_zone_lock.
 */
static int imr_lsm_allocator_recover_locked(void)
{
    __u32 newest_open = IMR_LSM_ZONE_COMPACTION_NONE;
    __u64 newest_generation = 0;
    __u64 max_generation = 0;
    __u32 zone_idx;

    imr_lsm_allocator_reset_locked();
    if(!zone_status){
        return -ENODEV;
    }

    for(zone_idx = 0; zone_idx < IMR_NUMZONES; zone_idx++){
        struct imrsim_zone_status *status = &zone_status[zone_idx];

        if(status->z_map_size > TOTAL_ITEMS ||
           status->z_live_count > status->z_map_size){
            return -EUCLEAN;
        }
        if(status->z_generation > max_generation){
            max_generation = status->z_generation;
        }
        if(!status->z_map_size){
            status->z_live_count = 0;
            status->z_generation = 0;
            if(status->z_conds != Z_COND_RO &&
               status->z_conds != Z_COND_OFFLINE){
                status->z_conds = Z_COND_EMPTY;
            }
            continue;
        }
        if(status->z_map_size == TOTAL_ITEMS){
            status->z_conds = Z_COND_FULL;
            continue;
        }
        if(status->z_conds == Z_COND_OPEN &&
           (newest_open == IMR_LSM_ZONE_COMPACTION_NONE ||
            status->z_generation > newest_generation)){
            newest_open = zone_idx;
            newest_generation = status->z_generation;
        }
    }

    for(zone_idx = 0; zone_idx < IMR_NUMZONES; zone_idx++){
        if(zone_idx != newest_open &&
           zone_status[zone_idx].z_conds == Z_COND_OPEN){
            zone_status[zone_idx].z_conds = Z_COND_CLOSED;
            imrsim_ptask_queue_zone_status_locked(zone_idx);
        }
    }

    imr_lsm_allocator.active_zone = newest_open;
    imr_lsm_allocator.next_generation = max_generation + 1;
    if(!imr_lsm_allocator.next_generation){
        imr_lsm_allocator.next_generation = 1;
    }
    return 0;
}

/* Caller holds imrsim_zone_lock. */
static bool imr_lsm_zone_is_free_locked(__u32 zone_idx)
{
    struct imrsim_zone_status *status;

    if(!zone_status || zone_idx >= IMR_NUMZONES ||
       zone_idx == imr_lsm_allocator.active_zone ||
       zone_idx == imr_lsm_zone_copy_source ||
       zone_idx == imr_lsm_zone_copy_dest){
        return false;
    }
    status = &zone_status[zone_idx];
    return !status->z_map_size && !status->z_live_count &&
           status->z_conds != Z_COND_RO &&
           status->z_conds != Z_COND_OFFLINE;
}

/* Caller holds imrsim_zone_lock. */
static __u32 imr_lsm_count_free_zones_locked(void)
{
    __u32 free_count = 0;
    __u32 zone_idx;

    for(zone_idx = 0; zone_idx < IMR_NUMZONES; zone_idx++){
        if(imr_lsm_zone_is_free_locked(zone_idx)){
            free_count++;
        }
    }
    return free_count;
}

/* Caller holds imrsim_zone_lock. */
static __u32 imr_lsm_find_free_zone_locked(__u32 exclude_zone)
{
    __u32 scanned;

    if(!IMR_NUMZONES){
        return IMR_LSM_ZONE_COMPACTION_NONE;
    }
    for(scanned = 0; scanned < IMR_NUMZONES; scanned++){
        __u32 zone_idx =
            (imr_lsm_allocator.free_cursor + scanned) % IMR_NUMZONES;

        if(zone_idx != exclude_zone &&
           imr_lsm_zone_is_free_locked(zone_idx)){
            imr_lsm_allocator.free_cursor =
                (zone_idx + 1) % IMR_NUMZONES;
            return zone_idx;
        }
    }
    return IMR_LSM_ZONE_COMPACTION_NONE;
}

/* Keep the currently advertised compaction destination in the free pool. */
static __u32 imr_lsm_find_foreground_free_zone_locked(void)
{
    __u32 reserved = IMR_LSM_ZONE_COMPACTION_NONE;
    __u32 free_count = 0;
    __u32 scanned;

    mutex_lock(&imr_lsm_lock);
    if(imr_lsm_meta.stats.zone_compaction_candidate_ready){
        reserved = imr_lsm_meta.stats.zone_compaction_candidate_dest_zone;
    }
    mutex_unlock(&imr_lsm_lock);
    if(!IMR_NUMZONES){
        return IMR_LSM_ZONE_COMPACTION_NONE;
    }
    for(scanned = 0; scanned < IMR_NUMZONES; scanned++){
        if(imr_lsm_zone_is_free_locked(scanned)){
            free_count++;
        }
    }
    /* One empty zone is permanent GC overprovisioning. */
    if(free_count < 2){
        return IMR_LSM_ZONE_COMPACTION_NONE;
    }
    for(scanned = 0; scanned < IMR_NUMZONES; scanned++){
        __u32 zone_idx =
            (imr_lsm_allocator.free_cursor + scanned) % IMR_NUMZONES;

        if(zone_idx != reserved && imr_lsm_zone_is_free_locked(zone_idx)){
            imr_lsm_allocator.free_cursor =
                (zone_idx + 1) % IMR_NUMZONES;
            return zone_idx;
        }
    }
    return IMR_LSM_ZONE_COMPACTION_NONE;
}

/* Caller holds imrsim_zone_lock. */
static void imr_lsm_reclaim_physical_zone_locked(__u32 zone_idx)
{
    struct imrsim_zone_status *status = &zone_status[zone_idx];

    status->z_map_size = 0;
    status->z_live_count = 0;
    status->z_generation = 0;
    status->z_conds = Z_COND_EMPTY;
    memset(status->z_key_map, 0xff, sizeof(status->z_key_map));
    imr_lsm_clear_zone_top_usage(zone_idx);
    imrsim_ptask_queue_zone_status_locked(zone_idx);
}

/*
 * Debug/test helper only.
 *
 * This seeds zone metadata as if the whole zone were already populated, which
 * lets VM validation exercise zone compaction without issuing a full-zone
 * write workload first.  It is not part of the normal LSM data path; formal
 * zone compaction still runs through imr_lsm_compact_zone().
 */
static int imr_lsm_seed_full_zone_locked(__u32 zone_idx)
{
    __u32 offset;

    if(!zone_status || zone_idx >= IMR_NUMZONES){
        return -EINVAL;
    }

    imrsim_ptask_queue_zone_status_locked(zone_idx);
    for(offset = 0; offset < TOTAL_ITEMS; offset++){
        __u64 key = imr_lsm_zone_key_start(zone_idx) + offset;
        sector_t pba = imr_lsm_zone_append_pba(zone_idx, offset);

        zone_status[zone_idx].z_pba_map[offset] =
            (int)(__u32)(pba >> IMR_BLOCK_SIZE_SHIFT);
        zone_status[zone_idx].z_key_map[offset] = (__u32)key;
    }
    zone_status[zone_idx].z_map_size = TOTAL_ITEMS;
    zone_status[zone_idx].z_live_count = TOTAL_ITEMS;
    zone_status[zone_idx].z_generation =
        imr_lsm_allocator.next_generation++;
    zone_status[zone_idx].z_conds = Z_COND_FULL;
    if(imr_lsm_allocator.active_zone == zone_idx){
        imr_lsm_allocator.active_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    }
    imr_lsm_mark_zone_top_full(zone_idx);
    imr_lsm_record_zone_compaction_candidate_locked(zone_idx);

    printk(KERN_INFO "imrsim: IMR-LSM seeded full zone metadata zone=%u entries=%u\n",
           zone_idx, TOTAL_ITEMS);
    return 0;
}

static void imr_lsm_record_zone_compaction_result(__u32 source_zone,
                                                  __u32 dest_zone0,
                                                  __u32 dest_zone1,
                                                  __u32 input_entries,
                                                  __u32 live_entries,
                                                  __u32 skipped_entries,
                                                  __u32 copied_entries,
                                                  __u32 failed_entries,
                                                  sector_t output_start,
                                                  sector_t output_end,
                                                  int error)
{
    imr_lsm_meta.stats.last_zone_compaction_source_zone = source_zone;
    imr_lsm_meta.stats.last_zone_compaction_dest_zone0 = dest_zone0;
    imr_lsm_meta.stats.last_zone_compaction_dest_zone1 = dest_zone1;
    imr_lsm_meta.stats.last_zone_compaction_input_entries = input_entries;
    imr_lsm_meta.stats.last_zone_compaction_live_entries = live_entries;
    imr_lsm_meta.stats.last_zone_compaction_skipped_entries =
        skipped_entries;
    imr_lsm_meta.stats.last_zone_compaction_copied_entries =
        copied_entries;
    imr_lsm_meta.stats.last_zone_compaction_failed_entries =
        failed_entries;
    imr_lsm_meta.stats.last_zone_compaction_error = error;
    imr_lsm_meta.stats.last_zone_compaction_output_pba_start =
        output_start;
    imr_lsm_meta.stats.last_zone_compaction_output_pba_end = output_end;
}

static int imr_lsm_copy_zone_compaction_block(struct page *page,
                                              struct block_device *bdev,
                                              sector_t bdev_start,
                                              sector_t source_pba,
                                              sector_t dest_pba,
                                              __u32 block_bytes,
                                              bool *copied)
{
    int ret;

    *copied = false;
    if(source_pba == dest_pba){
        return 0;
    }
    if(!bdev){
        return -ENODEV;
    }

    ret = imrsim_read_page(bdev, bdev_start + source_pba,
                           block_bytes, page);
    if(ret < 0){
        return ret;
    }
    ret = imrsim_write_page(bdev, bdev_start + dest_pba,
                            block_bytes, page);
    if(ret < 0){
        return ret;
    }

    *copied = true;
    return 0;
}

/* Caller holds imrsim_zone_lock. */
static bool imr_lsm_zone_copy_overlaps_locked(sector_t lba,
                                               sector_t sectors)
{
    __u64 first_zone;
    __u64 last_zone;
    sector_t last_lba;

    if(!imr_lsm_zone_copy_active){
        return false;
    }

    first_zone = (__u64)lba >> IMR_BLOCK_SIZE_SHIFT >>
                 IMR_ZONE_SIZE_SHIFT;
    if(!sectors){
        last_lba = lba;
    }else if(lba > (sector_t)~0ULL - (sectors - 1)){
        last_lba = (sector_t)~0ULL;
    }else{
        last_lba = lba + sectors - 1;
    }
    last_zone = (__u64)last_lba >> IMR_BLOCK_SIZE_SHIFT >>
                IMR_ZONE_SIZE_SHIFT;

    return (first_zone <= imr_lsm_zone_copy_source &&
            imr_lsm_zone_copy_source <= last_zone) ||
           (first_zone <= imr_lsm_zone_copy_dest &&
            imr_lsm_zone_copy_dest <= last_zone);
}

static bool imr_lsm_zone_copy_wait_done(sector_t lba, sector_t sectors)
{
    __u32 source_zone;
    __u32 dest_zone;
    __u64 first_zone;
    __u64 last_zone;
    sector_t last_lba;

    if(!READ_ONCE(imr_lsm_zone_copy_active) ||
       READ_ONCE(imrsim_single) != IMRSIM_TARGET_ACTIVE){
        return true;
    }

    smp_rmb();
    source_zone = READ_ONCE(imr_lsm_zone_copy_source);
    dest_zone = READ_ONCE(imr_lsm_zone_copy_dest);
    first_zone = (__u64)lba >> IMR_BLOCK_SIZE_SHIFT >>
                 IMR_ZONE_SIZE_SHIFT;
    if(!sectors){
        last_lba = lba;
    }else if(lba > (sector_t)~0ULL - (sectors - 1)){
        last_lba = (sector_t)~0ULL;
    }else{
        last_lba = lba + sectors - 1;
    }
    last_zone = (__u64)last_lba >> IMR_BLOCK_SIZE_SHIFT >>
                IMR_ZONE_SIZE_SHIFT;

    return !((first_zone <= source_zone && source_zone <= last_zone) ||
             (first_zone <= dest_zone && dest_zone <= last_zone));
}

/* Caller holds imrsim_zone_lock. */
static void imr_lsm_reserve_zone_copy_locked(__u32 source_zone,
                                              __u32 dest_zone)
{
    WRITE_ONCE(imr_lsm_zone_copy_source, source_zone);
    WRITE_ONCE(imr_lsm_zone_copy_dest, dest_zone);
    smp_wmb();
    WRITE_ONCE(imr_lsm_zone_copy_active, true);
}

/* Caller holds imrsim_zone_lock. */
static void imr_lsm_release_zone_copy_locked(void)
{
    WRITE_ONCE(imr_lsm_zone_copy_active, false);
    WRITE_ONCE(imr_lsm_zone_copy_source, IMR_LSM_ZONE_COMPACTION_NONE);
    WRITE_ONCE(imr_lsm_zone_copy_dest, IMR_LSM_ZONE_COMPACTION_NONE);
}

/* Caller holds imrsim_zone_lock and imr_lsm_lock. */
static int imr_lsm_prepare_zone_compaction_locked(
    __u32 source_zone, struct imr_lsm_zone_compaction_plan *plan)
{
    __u32 slot;
    int ret;

    plan->source_zone = source_zone;
    plan->dest_zone0 = IMR_LSM_ZONE_COMPACTION_NONE;
    plan->dest_zone1 = IMR_LSM_ZONE_COMPACTION_NONE;

    if(!zone_status || source_zone >= IMR_NUMZONES){
        return -EINVAL;
    }
    if(source_zone == imr_lsm_allocator.active_zone ||
       (zone_status[source_zone].z_conds != Z_COND_FULL &&
        zone_status[source_zone].z_conds != Z_COND_CLOSED) ||
       !zone_status[source_zone].z_map_size){
        return -EINVAL;
    }
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone == source_zone &&
       imr_lsm_meta.stats.zone_compaction_candidate_dest_zone !=
           IMR_LSM_ZONE_COMPACTION_NONE &&
       imr_lsm_zone_is_free_locked(
           imr_lsm_meta.stats.zone_compaction_candidate_dest_zone)){
        plan->dest_zone0 =
            imr_lsm_meta.stats.zone_compaction_candidate_dest_zone;
    }else{
        plan->dest_zone0 = imr_lsm_find_free_zone_locked(source_zone);
    }
    if(plan->dest_zone0 == IMR_LSM_ZONE_COMPACTION_NONE){
        return -ENOSPC;
    }
    plan->input_entries = zone_status[source_zone].z_map_size;

    plan->block_bytes = ((__u32)1 << IMR_BLOCK_SIZE_SHIFT) <<
                        IMR_SECTOR_SIZE_SHIFT_DEFAULT;
    if(plan->block_bytes > PAGE_SIZE){
        return -EOPNOTSUPP;
    }
    if(!imr_lsm_output_bdev){
        return -ENODEV;
    }
    plan->bdev = imr_lsm_output_bdev;
    plan->bdev_start = imr_lsm_output_bdev_start;

    for(slot = 0; slot < plan->input_entries; slot++){
        __u32 stored_key = zone_status[source_zone].z_key_map[slot];
        __u64 key;
        sector_t mapped_pba;
        sector_t source_pba;
        sector_t dest_pba;

        if(stored_key == IMR_LSM_KEY_EMPTY){
            plan->skipped_entries++;
            continue;
        }
        key = stored_key;
        source_pba = imr_lsm_zone_append_pba(source_zone, slot);
        if(!imr_lsm_forward_map_lookup_locked(key, &mapped_pba) ||
           mapped_pba != source_pba){
            plan->skipped_entries++;
            continue;
        }

        if(plan->live_entries >= TOTAL_ITEMS){
            return -ENOSPC;
        }
        dest_pba = imr_lsm_zone_append_pba(plan->dest_zone0,
                                           plan->live_entries);
        ret = imr_lsm_segment_builder_append_zone_gc(
            &plan->segment_builder, key, dest_pba, source_pba,
            plan->dest_zone0, ++imr_lsm_meta.timestamp);
        if(ret){
            return ret;
        }
        if(!plan->live_entries){
            plan->output_start = dest_pba;
            plan->output_end = dest_pba +
                (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
        }else{
            if(dest_pba < plan->output_start){
                plan->output_start = dest_pba;
            }
            if(dest_pba +
               (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1) >
               plan->output_end){
                plan->output_end = dest_pba +
                    (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
            }
        }
        plan->live_entries++;
    }

    if(plan->live_entries != zone_status[source_zone].z_live_count){
        printk(KERN_ERR "imrsim: allocator live-count mismatch zone=%u metadata=%u scanned=%u\n",
               source_zone, zone_status[source_zone].z_live_count,
               plan->live_entries);
        return -EUCLEAN;
    }
    if(plan->segment_builder.block_table_count > 1){
        sort(plan->segment_builder.block_table,
             plan->segment_builder.block_table_count,
             sizeof(*plan->segment_builder.block_table),
             imr_lsm_block_entry_key_compare, NULL);
    }

    return 0;
}

static int imr_lsm_execute_zone_compaction_copy(
    struct imr_lsm_zone_compaction_plan *plan)
{
    struct page *page;
    void *page_addr;
    __u32 entry_idx;
    int ret = 0;

    page = alloc_page(GFP_NOIO);
    if(!page){
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        __free_page(page);
        return -ENOMEM;
    }

    for(entry_idx = 0;
        entry_idx < plan->segment_builder.block_table_count;
        entry_idx++){
        struct imr_lsm_block_entry *entry =
            &plan->segment_builder.block_table[entry_idx];
        bool copied;

        if(READ_ONCE(imrsim_single) != IMRSIM_TARGET_ACTIVE){
            plan->failed_entries++;
            ret = -ENODEV;
            break;
        }
        memset(page_addr, 0, PAGE_SIZE);
        ret = imr_lsm_copy_zone_compaction_block(
            page, plan->bdev, plan->bdev_start, entry->source_pba,
            entry->pba, plan->block_bytes, &copied);
        if(ret){
            plan->failed_entries++;
            break;
        }
        if(copied){
            plan->copied_entries++;
        }
        cond_resched();
    }

    __free_page(page);
    return ret;
}

/* Caller holds imrsim_zone_lock and imr_lsm_lock. */
static int imr_lsm_publish_zone_compaction_locked(
    struct imr_lsm_zone_compaction_plan *plan)
{
    struct imr_lsm_segment *new_segment = NULL;
    __u32 entry_idx;
    __u32 committed_entries = 0;
    int ret;

    if(!imrsim_target_ready_locked() ||
       plan->bdev != imr_lsm_output_bdev ||
       plan->bdev_start != imr_lsm_output_bdev_start){
        return -ENODEV;
    }

    /* Link the segment first, then advance the newest-key index for committed
     * copies. Incremental accounting below sees foreground winners as newer
     * and touches only this output plus superseded per-key entries. */
    if(plan->segment_builder.block_table_count){
        ret = imr_lsm_append_segment_with_accounting_locked(
            IMR_LSM_MAX_LEVEL, IMR_LSM_TRACK_BOTTOM,
            &plan->segment_builder, false);
        if(ret){
            return ret;
        }
        new_segment = imr_lsm_meta.segment_tail;
    }

    imrsim_ptask_queue_zone_status_locked(plan->source_zone);
    imrsim_ptask_queue_zone_status_locked(plan->dest_zone0);
    memset(zone_status[plan->dest_zone0].z_key_map, 0xff,
           sizeof(zone_status[plan->dest_zone0].z_key_map));
    for(entry_idx = 0;
        new_segment && entry_idx < new_segment->block_table_count;
        entry_idx++){
        struct imr_lsm_block_entry *entry =
            &new_segment->block_table[entry_idx];
        sector_t mapped_pba;
        __u32 dest_slot;

        ret = imr_lsm_zone_append_slot(plan->dest_zone0, entry->pba,
                                       &dest_slot);
        if(ret){
            return ret;
        }
        zone_status[plan->dest_zone0].z_key_map[dest_slot] =
            (__u32)entry->key;

        /* A foreground overwrite may have won while the copy ran.  In that
         * case retain its newer forward mapping and leave this copied record
         * invalid in the sealed destination. */
        if(imr_lsm_forward_map_lookup_locked(entry->key, &mapped_pba) &&
           mapped_pba == entry->source_pba){
            ret = imr_lsm_forward_map_set_locked(entry->key, entry->pba);
            if(ret){
                return ret;
            }
            committed_entries++;
        }
        imr_lsm_newest_index_update_locked(entry->key, entry->timestamp,
                                           entry->valid);
        imr_lsm_tree_update_locked(entry->key, entry->pba, entry->valid,
                                   entry->timestamp);
    }

    zone_status[plan->dest_zone0].z_map_size = plan->live_entries;
    zone_status[plan->dest_zone0].z_live_count = committed_entries;
    if(plan->live_entries){
        zone_status[plan->dest_zone0].z_generation =
            imr_lsm_allocator.next_generation++;
        if(plan->live_entries < TOTAL_ITEMS &&
           imr_lsm_allocator.active_zone ==
               IMR_LSM_ZONE_COMPACTION_NONE){
            zone_status[plan->dest_zone0].z_conds = Z_COND_OPEN;
            imr_lsm_allocator.active_zone = plan->dest_zone0;
        }else{
            zone_status[plan->dest_zone0].z_conds =
                plan->live_entries == TOTAL_ITEMS ?
                Z_COND_FULL : Z_COND_CLOSED;
        }
    }else{
        zone_status[plan->dest_zone0].z_generation = 0;
        zone_status[plan->dest_zone0].z_conds = Z_COND_EMPTY;
    }
    imr_lsm_clear_zone_top_usage(plan->dest_zone0);
    if(plan->live_entries > imr_lsm_bottom_range_blocks()){
        __u32 top_blocks = plan->live_entries -
                           imr_lsm_bottom_range_blocks();
        __u32 top_slot;

        for(top_slot = 0; top_slot < top_blocks; top_slot++){
            __u32 track = top_slot / IMR_TOP_TRACK_SIZE;
            __u32 block = top_slot % IMR_TOP_TRACK_SIZE;

            zone_status[plan->dest_zone0].z_tracks[track]
                .isUsedBlock[block] = 1;
        }
    }
    imr_lsm_reclaim_physical_zone_locked(plan->source_zone);
    if(new_segment &&
       imr_lsm_account_new_segment_invalid_stats_locked(new_segment)){
        imr_lsm_refresh_max_obsolete_ratio_locked();
        imr_lsm_update_segment_compaction_selection_locked();
    }else if(!new_segment){
        imr_lsm_refresh_max_obsolete_ratio_locked();
        imr_lsm_update_segment_compaction_selection_locked();
    }

    imr_lsm_meta.stats.zone_compaction_count++;
    imr_lsm_meta.stats.zone_compaction_input_entries_total +=
        plan->input_entries;
    imr_lsm_meta.stats.zone_compaction_live_entries_total +=
        plan->live_entries;
    imr_lsm_meta.stats.zone_compaction_skipped_entries_total +=
        plan->skipped_entries;
    imr_lsm_meta.stats.zone_compaction_copied_entries_total +=
        plan->copied_entries;
    imr_lsm_meta.stats.zone_compaction_committed_entries_total +=
        committed_entries;
    imr_lsm_record_zone_compaction_result(
        plan->source_zone, plan->dest_zone0, plan->dest_zone1,
        plan->input_entries, plan->live_entries, plan->skipped_entries,
        plan->copied_entries, plan->failed_entries, plan->output_start,
        plan->output_end, 0);
    imr_lsm_clear_zone_compaction_candidate_locked(plan->source_zone);
    printk(KERN_INFO "imrsim: IMR-LSM zone compacted source=%u dest=%u input=%u live=%u committed=%u skipped=%u copied=%u output=%llu-%llu\n",
           plan->source_zone, plan->dest_zone0,
           plan->input_entries, plan->live_entries,
           committed_entries,
           plan->skipped_entries, plan->copied_entries,
           (unsigned long long)plan->output_start,
           (unsigned long long)plan->output_end);
    return 0;
}

/* Caller holds imr_lsm_lock. */
static void imr_lsm_record_zone_compaction_failure_locked(
    const struct imr_lsm_zone_compaction_plan *plan, int ret)
{
    imr_lsm_meta.stats.zone_compaction_failed_count++;
    imr_lsm_meta.stats.zone_compaction_failed_entries_total +=
        plan->failed_entries;
    imr_lsm_record_zone_compaction_result(
        plan->source_zone, plan->dest_zone0, plan->dest_zone1,
        plan->input_entries, plan->live_entries, plan->skipped_entries,
        plan->copied_entries, plan->failed_entries, plan->output_start,
        plan->output_end, ret);
    printk(KERN_ERR "imrsim: IMR-LSM zone compaction failed source=%u ret=%d live=%u skipped=%u copied=%u failed=%u\n",
           plan->source_zone, ret, plan->live_entries,
           plan->skipped_entries, plan->copied_entries,
           plan->failed_entries);
}

static int imr_lsm_compact_zone(__u32 source_zone)
{
    struct imr_lsm_zone_compaction_plan plan = {0};
    bool reserved = false;
    bool wake_waiters = false;
    int ret;

    plan.source_zone = source_zone;
    plan.dest_zone0 = IMR_LSM_ZONE_COMPACTION_NONE;
    plan.dest_zone1 = IMR_LSM_ZONE_COMPACTION_NONE;

    /* This mutex protects the immutable plan while the global locks are
     * dropped.  Foreground I/O is held only for the two reserved zones. */
    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        ret = -ENODEV;
        goto out_zone;
    }
    if(atomic_read(&imr_lsm_append_writes_inflight) > 0){
        ret = -EBUSY;
        goto out_zone;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_prepare_zone_compaction_locked(source_zone, &plan);
    if(ret){
        imr_lsm_record_zone_compaction_failure_locked(&plan, ret);
        goto out_lsm;
    }
    imr_lsm_reserve_zone_copy_locked(plan.source_zone, plan.dest_zone0);
    reserved = true;
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);

    ret = imr_lsm_execute_zone_compaction_copy(&plan);

    mutex_lock(&imrsim_zone_lock);
    mutex_lock(&imr_lsm_lock);
    if(!ret){
        ret = imr_lsm_publish_zone_compaction_locked(&plan);
    }
    if(ret){
        imr_lsm_record_zone_compaction_failure_locked(&plan, ret);
    }

out_lsm:
    if(reserved){
        imr_lsm_release_zone_copy_locked();
        wake_waiters = true;
    }
    if(reserved && !ret){
        /* Re-evaluate after the reclaimed victim leaves the copy reservation,
         * so free-zone pressure and the next destination include it. */
        imr_lsm_record_zone_compaction_candidate_locked(plan.dest_zone0);
    }
    mutex_unlock(&imr_lsm_lock);
out_zone:
    mutex_unlock(&imrsim_zone_lock);
    if(wake_waiters){
        wake_up_all(&imr_lsm_zone_copy_wait);
    }
    imr_lsm_segment_builder_release(&plan.segment_builder);
    mutex_unlock(&imr_lsm_compaction_lock);
    return ret;
}

static bool imr_lsm_segment_lookup_level_locked(
    __u32 level, __u64 key, struct imr_lsm_read_filter_stats *filter,
    struct imr_lsm_block_entry *level_entry, bool *level_hit)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;
    bool has_level_segment = false;
    bool needs_sorted_fallback = false;

    *level_hit = false;

    while(segment){
        if(segment->level == level){
            has_level_segment = true;
            if(segment->retired){
                segment = segment->next;
                continue;
            }
            filter->segment_lookup_count++;
            if(key < segment->min_key || key > segment->max_key){
                filter->segment_skip_count++;
            }else{
                struct imr_lsm_block_entry entry;

                filter->segment_candidate_count++;
                filter->bloom_lookup_count++;
                if(!segment->bloom_key_count ||
                   imr_lsm_bloom_may_contain(segment, key)){
                    filter->bloom_maybe_count++;
                    if(segment->block_table_count){
                        filter->block_table_lookup_count++;
                        if(imr_lsm_segment_block_table_find(segment, key,
                                                            &entry)){
                            segment->read_hit_count++;
                            segment->last_read_timestamp =
                                imr_lsm_meta.timestamp;
                            filter->block_table_hit_count++;
                            if(!filter->block_table_hit ||
                               entry.timestamp >
                               filter->block_table_entry.timestamp){
                                filter->block_table_hit = true;
                                filter->block_table_entry = entry;
                            }
                            if(!*level_hit ||
                               entry.timestamp > level_entry->timestamp){
                                *level_hit = true;
                                *level_entry = entry;
                            }
                        }else{
                            filter->block_table_miss_count++;
                        }
                    }else{
                        needs_sorted_fallback = true;
                    }
                }else{
                    filter->bloom_negative_count++;
                }
            }
        }
        segment = segment->next;
    }

    /*
     * If this level has sorted nodes but no active segment metadata/table yet,
     * keep the old behavior as a debug fallback.
     */
    if(!has_level_segment){
        return true;
    }

    return needs_sorted_fallback;
}

static __u32 imr_lsm_compaction_dst_level_locked(__u32 level)
{
    __u32 dst;

    if(level >= IMR_LSM_MAX_LEVEL){
        return IMR_LSM_MAX_LEVEL;
    }

    imr_lsm_calculate_dynamic_levels_locked();
    for(dst = level + 1; dst < IMR_LSM_LEVELS; dst++){
        if(imr_lsm_meta.level_max_bytes[dst] != ~0ULL){
            return dst;
        }
    }

    return IMR_LSM_MAX_LEVEL;
}

static __u64 imr_lsm_compaction_score_locked(__u32 level,
                                             __u64 total_downcompact_bytes)
{
    __u64 level_bytes;
    __u64 target;
    __u64 denominator;

    if(level >= IMR_LSM_LEVELS - 1){
        return 0;
    }

    if(level == IMR_LSM_DEFAULT_UNSORTED_LEVEL){
        __u64 level_entries = imr_lsm_level_total_count_locked(level);
        __u64 trigger = imr_lsm_compaction_threshold_locked();

        if(!level_entries){
            return 0;
        }
        return div64_u64(level_entries * IMR_LSM_SCORE_SCALE, trigger);
    }

    level_bytes = imr_lsm_level_actual_bytes_locked(level);
    if(!level_bytes){
        return 0;
    }

    if(imr_lsm_meta.lowest_unnecessary_level >= 0 &&
       level <= (__u32)imr_lsm_meta.lowest_unnecessary_level){
        return IMR_LSM_SCORE_SCALE * IMR_LSM_SCORE_BOOST + 1 +
               ((__u32)imr_lsm_meta.lowest_unnecessary_level - level);
    }

    target = imr_lsm_level_target_bytes_locked(level);
    if(!target){
        target = 1;
    }
    denominator = target + total_downcompact_bytes;
    if(denominator < target){
        denominator = ~0ULL;
    }

    return div64_u64(level_bytes * IMR_LSM_SCORE_SCALE, denominator);
}

static void imr_lsm_record_level_compaction_score(__u64 score)
{
    atomic64_set(&imrsim_diag.last_level_compaction_evaluated_score,
                 (s64)score);
    imrsim_diag_set_max(&imrsim_diag.level_compaction_score_max, score);
}

static __u32 imr_lsm_pick_compaction_level_locked(void)
{
    __u32 best_level = IMR_LSM_LEVELS;
    __u64 best_score = 0;
    __u64 total_downcompact_bytes = 0;
    __u32 level;

    imr_lsm_calculate_dynamic_levels_locked();
    for(level = 0; level < IMR_LSM_LEVELS - 1; level++){
        __u64 score = imr_lsm_compaction_score_locked(level,
                                                      total_downcompact_bytes);
        __u32 level_entries = imr_lsm_level_total_count_locked(level);
        __u64 level_bytes = imr_lsm_level_actual_bytes_locked(level);
        __u64 target_bytes = imr_lsm_level_target_bytes_locked(level);

        if(score > best_score){
            best_score = score;
            best_level = level;
        }

        if(level == IMR_LSM_DEFAULT_UNSORTED_LEVEL){
            if(level_entries >= imr_lsm_compaction_threshold_locked()){
                total_downcompact_bytes += level_bytes;
            }
        }else if(imr_lsm_meta.lowest_unnecessary_level >= 0 &&
           level <= (__u32)imr_lsm_meta.lowest_unnecessary_level){
            total_downcompact_bytes += level_bytes;
        }else if(target_bytes && level_bytes > target_bytes){
            total_downcompact_bytes += level_bytes - target_bytes;
        }
    }

    imr_lsm_record_level_compaction_score(best_score);

    if(best_score >= IMR_LSM_SCORE_SCALE){
        return best_level;
    }

    return IMR_LSM_LEVELS;
}

static void imr_lsm_level_compaction_plan_release(
    struct imr_lsm_level_compaction_plan *plan)
{
    imr_lsm_segment_builder_release(&plan->segment_builder);
    imr_lsm_free_sorted_nodes(plan->prepared_sorted_head);
    imr_lsm_free_sorted_nodes(plan->discarded_prepared_sorted_head);
    imr_lsm_segment_release(plan->output_segment);
    imr_lsm_free_unsorted_nodes(plan->retired_unsorted_head);
    imr_lsm_free_sorted_nodes(plan->retired_source_sorted_head);
    memset(plan, 0, sizeof(*plan));
}

/* Caller holds imrsim_zone_lock and imr_lsm_lock. */
static int imr_lsm_prepare_level_compaction_locked(
    __u32 level, __u32 max_unsorted,
    struct imr_lsm_level_compaction_plan *plan)
{
    struct imr_lsm_unsorted_node *node;
    __u32 available_unsorted;
    __u32 processed;

    if(level >= IMR_LSM_LEVELS){
        return -EINVAL;
    }

    available_unsorted = imr_lsm_meta.levels[level].unsorted_count;
    plan->source_level = level;
    plan->destination_level = imr_lsm_compaction_dst_level_locked(level);
    plan->source_unsorted_count = min(available_unsorted, max_unsorted);
    plan->move_sorted = plan->source_unsorted_count == available_unsorted;
    plan->retire_source = plan->destination_level != level &&
                          plan->move_sorted;
    plan->source_sorted_count = plan->retire_source ?
        imr_lsm_meta.levels[level].sorted_count : 0;

    if(!plan->source_unsorted_count && !plan->source_sorted_count){
        return 0;
    }

    plan->source_unsorted_head =
        imr_lsm_meta.levels[level].unsorted_head;
    node = plan->source_unsorted_head;
    for(processed = 0; processed < plan->source_unsorted_count;
        processed++){
        if(!node){
            return -EUCLEAN;
        }
        plan->source_unsorted_tail = node;
        node = node->next;
    }
    plan->source_unsorted_after = node;
    plan->source_sorted_head = plan->retire_source ?
        imr_lsm_meta.levels[level].sorted_head : NULL;
    plan->destination_sorted_head =
        imr_lsm_meta.levels[plan->destination_level].sorted_head;
    plan->destination_sorted_count =
        imr_lsm_meta.levels[plan->destination_level].sorted_count;
    plan->input_entries = plan->source_unsorted_count +
                          plan->source_sorted_count;
    plan->bloom_bits_per_key = imr_lsm_bloom_bits_per_key_locked();
    plan->build_delay_ms =
        READ_ONCE(imr_lsm_level_compaction_build_delay_ms);
    plan->metadata_epoch = imr_lsm_metadata_epoch;
    plan->output_bdev = imr_lsm_output_bdev;
    plan->prepared = true;
    return 0;
}

static int imr_lsm_build_prepared_sorted_nodes(
    struct imr_lsm_level_compaction_plan *plan)
{
    struct imr_lsm_sorted_node **tail = &plan->prepared_sorted_head;
    __u32 entry_idx;

    for(entry_idx = 0;
        entry_idx < plan->segment_builder.block_table_count;
        entry_idx++){
        struct imr_lsm_block_entry *entry =
            &plan->segment_builder.block_table[entry_idx];
        struct imr_lsm_sorted_node *node;

        node = kzalloc(sizeof(*node), GFP_NOIO);
        if(!node){
            return -ENOMEM;
        }
        node->key = entry->key;
        node->pba = entry->pba;
        node->zone_idx = entry->zone_idx;
        node->valid = entry->valid;
        node->timestamp = entry->timestamp;
        *tail = node;
        tail = &node->next;
        plan->prepared_sorted_count++;
        if(!(entry_idx & 1023)){
            cond_resched();
        }
    }
    return 0;
}

/* imr_lsm_compaction_lock keeps all borrowed input lists alive. */
static int imr_lsm_build_level_compaction_plan(
    struct imr_lsm_level_compaction_plan *plan)
{
    struct imr_lsm_unsorted_node *unsorted_node;
    struct imr_lsm_sorted_node *sorted_node;
    __u32 processed;
    int ret;

    if(plan->build_delay_ms){
        msleep(plan->build_delay_ms);
    }

    unsorted_node = plan->source_unsorted_head;
    for(processed = 0; processed < plan->source_unsorted_count;
        processed++){
        if(!unsorted_node){
            return -EAGAIN;
        }
        ret = imr_lsm_segment_builder_add_unsorted(
            &plan->segment_builder, unsorted_node);
        if(ret){
            return ret;
        }
        unsorted_node = unsorted_node->next;
        if(!(processed & 1023)){
            cond_resched();
        }
    }
    if(unsorted_node != plan->source_unsorted_after){
        return -EAGAIN;
    }

    sorted_node = plan->source_sorted_head;
    for(processed = 0; processed < plan->source_sorted_count;
        processed++){
        if(!sorted_node){
            return -EAGAIN;
        }
        ret = imr_lsm_segment_builder_add_sorted(
            &plan->segment_builder, sorted_node);
        if(ret){
            return ret;
        }
        sorted_node = sorted_node->next;
        if(!(processed & 1023)){
            cond_resched();
        }
    }
    if(sorted_node){
        return -EAGAIN;
    }

    plan->output_entries = plan->segment_builder.block_table_count;
    ret = imr_lsm_build_prepared_sorted_nodes(plan);
    if(ret){
        return ret;
    }
    return imr_lsm_prepare_segment(
        plan->destination_level, IMR_LSM_TRACK_BOTTOM,
        &plan->segment_builder, plan->bloom_bits_per_key,
        &plan->output_segment);
}

static int imr_lsm_validate_level_compaction_plan_locked(
    struct imr_lsm_level_compaction_plan *plan,
    struct imr_lsm_unsorted_node ***source_link)
{
    struct imr_lsm_level_state *source;
    struct imr_lsm_level_state *destination;
    struct imr_lsm_unsorted_node **link;
    struct imr_lsm_unsorted_node *node;
    __u32 processed;

    if(!plan->prepared || plan->published || !plan->output_segment ||
       !imrsim_target_ready_locked() || !imr_lsm_meta.initialized ||
       imr_lsm_metadata_epoch != plan->metadata_epoch ||
       imr_lsm_output_bdev != plan->output_bdev){
        return -EAGAIN;
    }

    source = &imr_lsm_meta.levels[plan->source_level];
    destination = &imr_lsm_meta.levels[plan->destination_level];
    if(destination->sorted_head != plan->destination_sorted_head ||
       destination->sorted_count != plan->destination_sorted_count){
        return -EAGAIN;
    }
    if(plan->retire_source &&
       (source->sorted_head != plan->source_sorted_head ||
        source->sorted_count != plan->source_sorted_count)){
        return -EAGAIN;
    }
    if(source->unsorted_count < plan->source_unsorted_count){
        return -EAGAIN;
    }

    link = &source->unsorted_head;
    while(*link && *link != plan->source_unsorted_head){
        link = &(*link)->next;
    }
    if(plan->source_unsorted_count && !*link){
        return -EAGAIN;
    }
    node = *link;
    for(processed = 0; processed < plan->source_unsorted_count;
        processed++){
        if(!node){
            return -EAGAIN;
        }
        if(processed + 1 == plan->source_unsorted_count &&
           node != plan->source_unsorted_tail){
            return -EAGAIN;
        }
        node = node->next;
    }
    if(node != plan->source_unsorted_after){
        return -EAGAIN;
    }

    *source_link = link;
    return 0;
}

static void imr_lsm_merge_prepared_sorted_locked(
    struct imr_lsm_level_compaction_plan *plan)
{
    struct imr_lsm_level_state *destination =
        &imr_lsm_meta.levels[plan->destination_level];
    struct imr_lsm_sorted_node **link = &destination->sorted_head;
    struct imr_lsm_sorted_node *prepared = plan->prepared_sorted_head;

    while(prepared){
        struct imr_lsm_sorted_node *next = prepared->next;

        while(*link && (*link)->key < prepared->key){
            link = &(*link)->next;
        }
        if(*link && (*link)->key == prepared->key){
            struct imr_lsm_sorted_node *existing = *link;

            if(prepared->timestamp > existing->timestamp){
                existing->pba = prepared->pba;
                existing->zone_idx = prepared->zone_idx;
                existing->valid = prepared->valid;
                existing->timestamp = prepared->timestamp;
            }
            prepared->next = plan->discarded_prepared_sorted_head;
            plan->discarded_prepared_sorted_head = prepared;
            link = &existing->next;
        }else{
            prepared->next = *link;
            *link = prepared;
            destination->sorted_count++;
            link = &prepared->next;
        }
        prepared = next;
    }
    plan->prepared_sorted_head = NULL;
}

/* Caller holds imrsim_zone_lock and imr_lsm_lock. */
static int imr_lsm_publish_level_compaction_locked(
    struct imr_lsm_level_compaction_plan *plan)
{
    struct imr_lsm_level_state *source;
    struct imr_lsm_segment *published_segment;
    struct imr_lsm_unsorted_node **source_link = NULL;
    __u32 retired_segments = 0;
    int ret;

    ret = imr_lsm_validate_level_compaction_plan_locked(plan, &source_link);
    if(ret){
        atomic64_inc(&imrsim_diag.level_compaction_publish_conflict_count);
        return ret;
    }

    source = &imr_lsm_meta.levels[plan->source_level];
    if(plan->source_unsorted_count){
        *source_link = plan->source_unsorted_after;
        plan->source_unsorted_tail->next = NULL;
        plan->retired_unsorted_head = plan->source_unsorted_head;
        source->unsorted_count -= plan->source_unsorted_count;
    }
    if(plan->retire_source){
        plan->retired_source_sorted_head = source->sorted_head;
        source->sorted_head = NULL;
        source->sorted_count = 0;
    }

    imr_lsm_merge_prepared_sorted_locked(plan);
    imr_lsm_publish_prepared_segment_locked(plan->output_segment, false);
    published_segment = plan->output_segment;
    plan->output_segment = NULL;
    if(plan->retire_source){
        retired_segments =
            imr_lsm_retire_level_segments_locked(plan->source_level);
    }
    if(imr_lsm_account_new_segment_invalid_stats_locked(published_segment)){
        imr_lsm_refresh_max_obsolete_ratio_locked();
        imr_lsm_update_segment_compaction_selection_locked();
    }

    imr_lsm_meta.stats.compaction_count++;
    imr_lsm_meta.stats.last_compaction_input_bytes =
        (__u64)plan->input_entries * IMR_LSM_RECORD_BYTES;
    imr_lsm_meta.stats.last_compaction_output_bytes =
        (__u64)plan->output_entries * IMR_LSM_RECORD_BYTES;
    imr_lsm_meta.stats.metadata_compaction_input_bytes +=
        imr_lsm_meta.stats.last_compaction_input_bytes;
    imr_lsm_meta.stats.metadata_compaction_output_bytes +=
        imr_lsm_meta.stats.last_compaction_output_bytes;
    imr_lsm_meta.stats.last_compaction_from = plan->source_level;
    imr_lsm_meta.stats.last_compaction_to = plan->destination_level;
    imr_lsm_meta.stats.last_compaction_input = plan->input_entries;
    imr_lsm_meta.stats.last_compaction_output_total =
        imr_lsm_meta.levels[plan->destination_level].sorted_count;
    imr_lsm_calculate_dynamic_levels_locked();
    plan->published = true;

    IMRSIM_DATA_LOG("imrsim: IMR-LSM published L%u->L%u unsorted=%u sorted=%u remaining_unsorted=%u dst_sorted=%u retired_segments=%u base=L%u lowest_unnecessary=%d\n",
           plan->source_level, plan->destination_level,
           plan->source_unsorted_count, plan->source_sorted_count,
           source->unsorted_count,
           imr_lsm_meta.levels[plan->destination_level].sorted_count,
           retired_segments, imr_lsm_meta.base_level,
           imr_lsm_meta.lowest_unnecessary_level);
    return 0;
}

static bool imr_lsm_level_compaction_needed_locked(void)
{
    __u64 score;

    imr_lsm_calculate_dynamic_levels_locked();
    if(imr_lsm_meta.levels[IMR_LSM_DEFAULT_UNSORTED_LEVEL].unsorted_count >=
       imr_lsm_compaction_threshold_locked()){
        score = imr_lsm_compaction_score_locked(
            IMR_LSM_DEFAULT_UNSORTED_LEVEL, 0);
        imr_lsm_record_level_compaction_score(score);
        return true;
    }
    if(imr_lsm_meta.base_level < IMR_LSM_MAX_LEVEL &&
       imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].unsorted_count){
        imr_lsm_record_level_compaction_score(IMR_LSM_SCORE_SCALE);
        return true;
    }

    return imr_lsm_pick_compaction_level_locked() < IMR_LSM_LEVELS;
}

static void imr_lsm_mark_level_compaction_pending_locked(void)
{
    if(!imr_lsm_level_compaction_needed_locked()){
        return;
    }
    if(imr_lsm_meta.level_compaction_pending){
        atomic64_inc(&imrsim_diag.level_compaction_coalesced_schedule_count);
        return;
    }

    atomic64_set(&imrsim_diag.level_compaction_queued_at_ns,
                 (s64)imrsim_diag_now_ns());
    atomic64_set(&imrsim_diag.level_compaction_queue_depth, 1);
    imrsim_diag_set_max(&imrsim_diag.level_compaction_queue_depth_max, 1);
    atomic64_set(&imrsim_diag.last_level_compaction_schedule_score,
                 atomic64_read(
                     &imrsim_diag.last_level_compaction_evaluated_score));
    imr_lsm_meta.level_compaction_pending = 1;
    imr_lsm_meta.stats.level_compaction_work_schedule_count++;
}

static void imr_lsm_queue_level_compaction_work(void)
{
    struct workqueue_struct *wq = ACCESS_ONCE(imr_lsm_level_compaction_wq);

    if(wq && ACCESS_ONCE(imrsim_single) == IMRSIM_TARGET_ACTIVE &&
       ACCESS_ONCE(imr_lsm_meta.level_compaction_pending) &&
       !ACCESS_ONCE(imr_lsm_meta.level_compaction_running)){
        queue_work(wq, &imr_lsm_level_compaction_work);
    }
}

static void imr_lsm_record_level_compaction_entries(
    __u32 level, __u64 input_entries, __u64 output_entries)
{
    atomic64_add((s64)input_entries,
                 &imrsim_diag.level_compaction_input_entries_total);
    imrsim_diag_set_max(
        &imrsim_diag.level_compaction_input_entries_max, input_entries);
    atomic64_set(&imrsim_diag.last_level_compaction_input_entries,
                 (s64)input_entries);
    atomic64_add((s64)output_entries,
                 &imrsim_diag.level_compaction_output_entries_total);
    imrsim_diag_set_max(
        &imrsim_diag.level_compaction_output_entries_max, output_entries);
    atomic64_set(&imrsim_diag.last_level_compaction_output_entries,
                 (s64)output_entries);

    if(level < IMR_LSM_LEVELS){
        atomic64_add((s64)input_entries,
                     &imrsim_diag
                          .level_compaction_input_entries_total_by_level[
                              level]);
        imrsim_diag_set_max(
            &imrsim_diag.level_compaction_input_entries_max_by_level[level],
            input_entries);
        atomic64_add((s64)output_entries,
                     &imrsim_diag
                          .level_compaction_output_entries_total_by_level[
                              level]);
        imrsim_diag_set_max(
            &imrsim_diag.level_compaction_output_entries_max_by_level[level],
            output_entries);
    }
}

static void imr_lsm_acquire_level_compaction_locks(
    __u64 *zone_hold_start_ns, __u64 *lsm_hold_start_ns)
{
    imrsim_diag_timed_mutex_lock(
        &imrsim_zone_lock,
        &imrsim_diag.level_compaction_zone_lock_wait_count,
        &imrsim_diag.level_compaction_zone_lock_wait_total_ns,
        &imrsim_diag.level_compaction_zone_lock_wait_max_ns);
    *zone_hold_start_ns = imrsim_diag_now_ns();
    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.level_compaction_lsm_lock_wait_count,
        &imrsim_diag.level_compaction_lsm_lock_wait_total_ns,
        &imrsim_diag.level_compaction_lsm_lock_wait_max_ns);
    *lsm_hold_start_ns = imrsim_diag_now_ns();
}

static void imr_lsm_release_level_compaction_locks(__u64 zone_hold_start_ns,
                                                    __u64 lsm_hold_start_ns)
{
    __u64 lsm_hold_ns = imrsim_diag_elapsed_ns(lsm_hold_start_ns);
    __u64 zone_hold_ns;

    mutex_unlock(&imr_lsm_lock);
    zone_hold_ns = imrsim_diag_elapsed_ns(zone_hold_start_ns);
    mutex_unlock(&imrsim_zone_lock);

    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_lsm_lock_hold_count,
        &imrsim_diag.level_compaction_lsm_lock_hold_total_ns,
        &imrsim_diag.level_compaction_lsm_lock_hold_max_ns,
        &imrsim_diag.last_level_compaction_lsm_lock_hold_ns,
        lsm_hold_ns);
    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_zone_lock_hold_count,
        &imrsim_diag.level_compaction_zone_lock_hold_total_ns,
        &imrsim_diag.level_compaction_zone_lock_hold_max_ns,
        &imrsim_diag.last_level_compaction_zone_lock_hold_ns,
        zone_hold_ns);
}

/* Caller holds imr_lsm_compaction_lock for the entire plan lifetime. */
static int imr_lsm_execute_level_compaction(
    bool automatic, __u32 requested_level, bool *did_work,
    __u32 *compacted_level, __u32 *input_entries, __u32 *output_entries)
{
    struct imr_lsm_level_compaction_plan plan = {0};
    __u64 phase_start_ns;
    __u64 zone_hold_start_ns;
    __u64 lsm_hold_start_ns;
    __u32 level = requested_level;
    __u32 max_unsorted = (__u32)~0U;
    int ret = 0;

    *did_work = false;
    *compacted_level = IMR_LSM_LEVELS;
    *input_entries = 0;
    *output_entries = 0;

    atomic64_set(&imrsim_diag.level_compaction_phase,
                 IMR_LSM_LEVEL_COMPACTION_PREPARE);
    phase_start_ns = imrsim_diag_now_ns();
    imr_lsm_acquire_level_compaction_locks(&zone_hold_start_ns,
                                           &lsm_hold_start_ns);
    if(!imrsim_target_ready_locked() || !imr_lsm_output_bdev){
        ret = -ENODEV;
        goto out_prepare_unlock;
    }
    if(!imr_lsm_meta.initialized){
        if(automatic){
            ret = -ENODEV;
            goto out_prepare_unlock;
        }
        imr_lsm_initialize_metadata_locked();
    }

    if(automatic){
        imr_lsm_calculate_dynamic_levels_locked();
        if(imr_lsm_meta.base_level < IMR_LSM_MAX_LEVEL &&
           imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].unsorted_count){
            level = IMR_LSM_MAX_LEVEL;
        }else{
            level = imr_lsm_pick_compaction_level_locked();
            if(level >= IMR_LSM_LEVELS){
                goto out_prepare_unlock;
            }
            if(level == IMR_LSM_DEFAULT_UNSORTED_LEVEL){
                max_unsorted = imr_lsm_compaction_threshold_locked();
            }
        }
    }else if(level >= IMR_LSM_LEVELS){
        ret = -EINVAL;
        goto out_prepare_unlock;
    }

    ret = imr_lsm_prepare_level_compaction_locked(
        level, max_unsorted, &plan);
    if(!ret && plan.prepared){
        *did_work = true;
        *compacted_level = level;
        *input_entries = plan.input_entries;
    }

out_prepare_unlock:
    imr_lsm_release_level_compaction_locks(zone_hold_start_ns,
                                           lsm_hold_start_ns);
    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_prepare_time_count,
        &imrsim_diag.level_compaction_prepare_total_ns,
        &imrsim_diag.level_compaction_prepare_max_ns,
        &imrsim_diag.last_level_compaction_prepare_ns,
        imrsim_diag_elapsed_ns(phase_start_ns));
    if(ret || !plan.prepared){
        goto out;
    }

    atomic64_set(&imrsim_diag.level_compaction_phase,
                 IMR_LSM_LEVEL_COMPACTION_BUILD);
    phase_start_ns = imrsim_diag_now_ns();
    ret = imr_lsm_build_level_compaction_plan(&plan);
    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_build_time_count,
        &imrsim_diag.level_compaction_build_total_ns,
        &imrsim_diag.level_compaction_build_max_ns,
        &imrsim_diag.last_level_compaction_build_ns,
        imrsim_diag_elapsed_ns(phase_start_ns));
    if(ret){
        goto out;
    }
    *output_entries = plan.output_entries;

    atomic64_set(&imrsim_diag.level_compaction_phase,
                 IMR_LSM_LEVEL_COMPACTION_PUBLISH);
    phase_start_ns = imrsim_diag_now_ns();
    imr_lsm_acquire_level_compaction_locks(&zone_hold_start_ns,
                                           &lsm_hold_start_ns);
    ret = imr_lsm_publish_level_compaction_locked(&plan);
    imr_lsm_release_level_compaction_locks(zone_hold_start_ns,
                                           lsm_hold_start_ns);
    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_publish_time_count,
        &imrsim_diag.level_compaction_publish_total_ns,
        &imrsim_diag.level_compaction_publish_max_ns,
        &imrsim_diag.last_level_compaction_publish_ns,
        imrsim_diag_elapsed_ns(phase_start_ns));

out:
    atomic64_set(&imrsim_diag.level_compaction_phase,
                 IMR_LSM_LEVEL_COMPACTION_IDLE);
    imr_lsm_level_compaction_plan_release(&plan);
    return ret;
}

static void imr_lsm_level_compaction_auto_work(struct work_struct *work)
{
    __u32 rounds;
    __u32 successful_rounds = 0;
    __u64 queued_at_ns;
    __u64 round_duration_ns;
    __u64 round_start_ns;
    __u64 work_start_ns;
    __u64 zone_hold_start_ns;
    __u64 lsm_hold_start_ns;
    __u64 post_round_start_ns;
    __u32 compacted_level;
    __u32 input_entries;
    __u32 output_entries;
    bool did_work;
    bool compaction_needed;
    bool requeue = false;
    int ret = 0;

    (void)work;
    /* Close the queue-again window before the worker reaches its locked
     * setup.  The owning work item cannot run concurrently with itself. */
    WRITE_ONCE(imr_lsm_meta.level_compaction_running, 1);
    work_start_ns = imrsim_diag_now_ns();
    queued_at_ns = (__u64)atomic64_xchg(
        &imrsim_diag.level_compaction_queued_at_ns, 0);
    atomic64_set(&imrsim_diag.level_compaction_queue_depth, 0);
    if(queued_at_ns && work_start_ns >= queued_at_ns){
        imrsim_diag_record_duration(
            &imrsim_diag.level_compaction_queue_wait_count,
            &imrsim_diag.level_compaction_queue_wait_total_ns,
            &imrsim_diag.level_compaction_queue_wait_max_ns,
            &imrsim_diag.last_level_compaction_queue_wait_ns,
            work_start_ns - queued_at_ns);
    }
    mutex_lock(&imr_lsm_compaction_lock);
    imr_lsm_acquire_level_compaction_locks(&zone_hold_start_ns,
                                           &lsm_hold_start_ns);
    if(ACCESS_ONCE(imrsim_single) != IMRSIM_TARGET_ACTIVE ||
       !imr_lsm_meta.initialized || !imr_lsm_output_bdev){
        imr_lsm_meta.level_compaction_pending = 0;
        imr_lsm_meta.level_compaction_running = 0;
        atomic64_set(&imrsim_diag.level_compaction_queued_at_ns, 0);
        atomic64_set(&imrsim_diag.level_compaction_queue_depth, 0);
        imr_lsm_release_level_compaction_locks(zone_hold_start_ns,
                                               lsm_hold_start_ns);
        goto out_record_work;
    }

    imr_lsm_meta.level_compaction_pending = 0;
    imr_lsm_meta.level_compaction_running = 1;
    imr_lsm_meta.stats.level_compaction_work_run_count++;
    imr_lsm_release_level_compaction_locks(zone_hold_start_ns,
                                           lsm_hold_start_ns);

    for(rounds = 0; rounds < IMR_LSM_LEVEL_COMPACTION_WORK_ROUNDS;
        rounds++){
        did_work = false;
        round_start_ns = imrsim_diag_now_ns();
        ret = imr_lsm_execute_level_compaction(
            true, IMR_LSM_LEVELS, &did_work, &compacted_level,
            &input_entries, &output_entries);
        if(did_work){
            round_duration_ns = imrsim_diag_elapsed_ns(round_start_ns);
            imrsim_diag_record_duration(
                &imrsim_diag.level_compaction_time_count,
                &imrsim_diag.level_compaction_total_ns,
                &imrsim_diag.level_compaction_max_ns,
                &imrsim_diag.last_level_compaction_ns,
                round_duration_ns);
            if(!ret && compacted_level < IMR_LSM_LEVELS){
                imr_lsm_record_level_compaction_entries(
                    compacted_level, input_entries, output_entries);
                imrsim_diag_record_duration(
                    &imrsim_diag.level_compaction_time_count_by_level[
                        compacted_level],
                    &imrsim_diag.level_compaction_total_ns_by_level[
                        compacted_level],
                    &imrsim_diag.level_compaction_max_ns_by_level[
                        compacted_level],
                    NULL, round_duration_ns);
                successful_rounds++;
            }
        }
        if(ret || !did_work){
            if(!did_work){
                atomic64_inc(
                    &imrsim_diag.level_compaction_no_work_run_count);
            }
            break;
        }
    }

    post_round_start_ns = imrsim_diag_now_ns();
    imr_lsm_acquire_level_compaction_locks(&zone_hold_start_ns,
                                           &lsm_hold_start_ns);
    imr_lsm_meta.stats.level_compaction_work_round_count +=
        successful_rounds;
    if(!ret && imrsim_target_ready_locked() &&
       imr_lsm_meta.initialized && imr_lsm_output_bdev){
        /* A successful publish already refreshed segment-invalid statistics.
         * Only the scheduling decision remains in this short locked tail. */
        compaction_needed = imr_lsm_level_compaction_needed_locked();
        if(compaction_needed){
            if(!imr_lsm_meta.level_compaction_pending){
                atomic64_set(&imrsim_diag.level_compaction_queued_at_ns,
                             (s64)imrsim_diag_now_ns());
                atomic64_set(&imrsim_diag.level_compaction_queue_depth, 1);
                imrsim_diag_set_max(
                    &imrsim_diag.level_compaction_queue_depth_max, 1);
                imr_lsm_meta.level_compaction_pending = 1;
                imr_lsm_meta.stats.level_compaction_work_schedule_count++;
            }
            atomic64_set(
                &imrsim_diag.last_level_compaction_requeue_score,
                atomic64_read(
                    &imrsim_diag.last_level_compaction_evaluated_score));
            imr_lsm_meta.stats.level_compaction_work_requeue_count++;
            requeue = true;
        }else{
            imr_lsm_meta.level_compaction_pending = 0;
            atomic64_set(&imrsim_diag.level_compaction_queued_at_ns, 0);
            atomic64_set(&imrsim_diag.level_compaction_queue_depth, 0);
        }
        imrsim_diag_record_duration(
            &imrsim_diag.level_compaction_post_round_count,
            &imrsim_diag.level_compaction_post_round_total_ns,
            &imrsim_diag.level_compaction_post_round_max_ns,
            &imrsim_diag.last_level_compaction_post_round_ns,
            imrsim_diag_elapsed_ns(post_round_start_ns));
    }else{
        imr_lsm_meta.level_compaction_pending = 0;
        atomic64_set(&imrsim_diag.level_compaction_queued_at_ns, 0);
        atomic64_set(&imrsim_diag.level_compaction_queue_depth, 0);
        if(ret){
            imr_lsm_meta.stats.level_compaction_work_error_count++;
            /* Keep the most recent non-zero failure available for diagnosis.
             * A later successful/no-work run must not erase its errno. */
            imr_lsm_meta.stats.last_level_compaction_work_error = ret;
        }
    }
    imr_lsm_meta.level_compaction_running = 0;
    imr_lsm_release_level_compaction_locks(zone_hold_start_ns,
                                           lsm_hold_start_ns);

    if(requeue){
        imr_lsm_queue_level_compaction_work();
    }

out_record_work:
    mutex_unlock(&imr_lsm_compaction_lock);
    imrsim_diag_record_duration(
        &imrsim_diag.level_compaction_work_time_count,
        &imrsim_diag.level_compaction_work_total_ns,
        &imrsim_diag.level_compaction_work_max_ns,
        &imrsim_diag.last_level_compaction_work_ns,
        imrsim_diag_elapsed_ns(work_start_ns));
}

static int imr_lsm_append_unsorted_node_locked(__u32 level, __u64 key,
                                               sector_t pba, __u32 zone_idx,
                                               __u8 valid)
{
    struct imr_lsm_unsorted_node *node;
    node = kzalloc(sizeof(*node), GFP_NOIO);
    if(!node){
        printk(KERN_ERR "imrsim: IMR-LSM unsorted node alloc failed\n");
        return -ENOMEM;
    }

    node->key = key;
    node->pba = pba;
    node->zone_idx = zone_idx;
    node->valid = valid;
    node->timestamp = ++imr_lsm_meta.timestamp;
    node->next = imr_lsm_meta.levels[level].unsorted_head;
    imr_lsm_meta.levels[level].unsorted_head = node;
    imr_lsm_meta.levels[level].unsorted_count++;
    /* This is a derived acceleration index.  Allocation failure marks it
     * invalid and newer-record queries safely fall back to the legacy scan. */
    imr_lsm_newest_index_update_locked(key, node->timestamp, valid);
    imr_lsm_tree_update_locked(key, pba, valid, node->timestamp);

    IMRSIM_DATA_LOG("imrsim: IMR-LSM append unsorted L%u key=%llu pba=%llu valid=%u ts=%llu\n",
                    level, key, (unsigned long long)pba, valid,
                    node->timestamp);
    if(imr_lsm_meta.levels[level].unsorted_count >=
       imr_lsm_level_capacity(level)){
        IMRSIM_DATA_LOG("imrsim: IMR-LSM compaction should trigger L%u unsorted_count=%u capacity=%u\n",
                        level,
                        imr_lsm_meta.levels[level].unsorted_count,
                        imr_lsm_level_capacity(level));
    }
    imr_lsm_mark_level_compaction_pending_locked();
    return 0;
}

static int imr_lsm_append_unsorting_node(__u32 level, __u64 key,
                                         sector_t pba, __u32 zone_idx)
{
    __u32 target_level = level;
    bool queue_compaction;
    int ret;

    if(level >= IMR_LSM_LEVELS){
        printk(KERN_ERR "imrsim: IMR-LSM invalid level: %u\n", level);
        return -EINVAL;
    }

    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_calculate_dynamic_levels_locked();
    if(level == IMR_LSM_DEFAULT_UNSORTED_LEVEL){
        target_level = imr_lsm_meta.active_write_level;
    }

    ret = imr_lsm_append_unsorted_node_locked(target_level, key, pba, zone_idx, 1);
    if(!ret){
        imr_lsm_meta.stats.lsm_write_count++;
    }
    queue_compaction = imr_lsm_meta.level_compaction_pending;
    mutex_unlock(&imr_lsm_lock);
    if(queue_compaction){
        imr_lsm_queue_level_compaction_work();
    }
    return ret;
}

static int imr_lsm_unsorted_write(__u32 level, __u64 key,
                                  sector_t pba, __u32 zone_idx)
{
    /*
     * The actual payload write is still handled by device-mapper after the bio
     * is remapped. This function records the new SST-like entry only in the
     * unsorted list and intentionally skips sorting, block table rebuild, and RMW.
     */
    return imr_lsm_append_unsorting_node(level, key, pba, zone_idx);
}

static int imr_lsm_insert(__u32 level, __u64 key, sector_t pba, __u32 zone_idx)
{
    return imr_lsm_unsorted_write(level, key, pba, zone_idx);
}

static int imr_lsm_record_insert(__u32 zone_idx, __u64 logical_lba,
                                 sector_t physical_lba)
{
    __u64 key = logical_lba >> IMR_BLOCK_SIZE_SHIFT;
    int ret;

    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.stats.lsm_record_insert_count++;
    mutex_unlock(&imr_lsm_lock);

    ret = imr_lsm_insert(IMR_LSM_DEFAULT_UNSORTED_LEVEL, key,
                         physical_lba, zone_idx);
    return ret;
}

static int imr_lsm_record_insert_and_check_zone_full(__u32 zone_idx,
                                                     __u64 logical_lba,
                                                     sector_t physical_lba)
{
    return imr_lsm_record_insert(zone_idx, logical_lba, physical_lba);
}

static __u32 imrsim_write_block_count(__u64 logical_lba,
                                      sector_t bio_sectors)
{
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    sector_t first_block_offset =
        (sector_t)(logical_lba & (block_sectors - 1));
    sector_t covered_sectors;

    if(!bio_sectors){
        return 0;
    }

    covered_sectors = first_block_offset + bio_sectors;
    return (__u32)((covered_sectors + block_sectors - 1) >>
                   IMR_BLOCK_SIZE_SHIFT);
}

static int imrsim_record_write_mapping_range(__u32 zone_idx,
                                             __u64 logical_lba,
                                             sector_t physical_lba,
                                             sector_t bio_sectors)
{
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    __u64 logical_block_lba =
        logical_lba & ~((__u64)block_sectors - 1);
    sector_t physical_block_lba =
        physical_lba & ~((sector_t)block_sectors - 1);
    __u64 zone_lba = zone_idx_lba(zone_idx);
    __u32 block_count =
        imrsim_write_block_count(logical_lba, bio_sectors);
    __u32 block_idx;

    /*
     * Metadata records are still 4K-key entries.  The partial data-I/O path
     * must merge head/tail sectors into a complete block before publishing a
     * mapping here.
     */
    if(bio_sectors != block_sectors ||
       (logical_lba & (block_sectors - 1)) ||
       (physical_lba & (block_sectors - 1))){
        return IMR_ERR_WRITE_ALIGN;
    }

    imrsim_ptask_queue_zone_status_locked(zone_idx);
    for(block_idx = 0; block_idx < block_count; block_idx++){
        __u64 entry_lba =
            logical_block_lba +
            ((__u64)block_idx << IMR_BLOCK_SIZE_SHIFT);
        sector_t entry_pba =
            physical_block_lba +
            ((sector_t)block_idx << IMR_BLOCK_SIZE_SHIFT);
        __u64 block_offset;
        int ret;

        if(entry_lba < zone_lba){
            return IMR_ERR_OUT_RANGE;
        }

        block_offset = (entry_lba - zone_lba) >> IMR_BLOCK_SIZE_SHIFT;
        if(block_offset >= TOTAL_ITEMS){
            return IMR_ERR_WRITE_BORDER;
        }

        zone_status[zone_idx].z_pba_map[block_offset] =
            (int)(__u32)(entry_pba >> IMR_BLOCK_SIZE_SHIFT);

        ret = imr_lsm_record_insert_and_check_zone_full(zone_idx,
                                                        entry_lba,
                                                        entry_pba);
        if(ret){
            return ret;
        }
    }

    return 0;
}

static void imr_lsm_record_logical_write(void)
{
    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.stats.logical_write_count++;
    mutex_unlock(&imr_lsm_lock);
}

/* Caller holds imrsim_zone_lock. */
static int imr_lsm_activate_zone_locked(__u32 *zone_idx)
{
    struct imrsim_zone_status *status;
    __u32 selected = imr_lsm_allocator.active_zone;

    if(selected != IMR_LSM_ZONE_COMPACTION_NONE){
        if(selected < IMR_NUMZONES){
            status = &zone_status[selected];
            if(status->z_conds == Z_COND_OPEN &&
               status->z_map_size < TOTAL_ITEMS){
                *zone_idx = selected;
                return 0;
            }
        }
        imr_lsm_allocator.active_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    }

    selected = imr_lsm_find_foreground_free_zone_locked();
    if(selected == IMR_LSM_ZONE_COMPACTION_NONE){
        return -ENOSPC;
    }

    status = &zone_status[selected];
    status->z_conds = Z_COND_OPEN;
    status->z_generation = imr_lsm_allocator.next_generation++;
    if(!imr_lsm_allocator.next_generation){
        imr_lsm_allocator.next_generation = 1;
    }
    imr_lsm_allocator.active_zone = selected;
    imrsim_ptask_queue_zone_status_locked(selected);
    *zone_idx = selected;
    printk(KERN_INFO "imrsim: IMR-LSM activated physical zone=%u generation=%llu\n",
           selected, (unsigned long long)status->z_generation);
    return 0;
}

/*
 * Reserve and publish one 4 KiB append mapping.  z_map_size is advanced while
 * imrsim_zone_lock is held, so every writer receives a disjoint slot even
 * though the remapped bios complete asynchronously.
 * Caller holds imrsim_zone_lock.
 */
static int imr_lsm_append_logical_block_locked(__u64 logical_lba,
                                               sector_t *physical_lba,
                                               __u32 *physical_zone)
{
    __u64 key = logical_lba >> IMR_BLOCK_SIZE_SHIFT;
    struct imrsim_zone_status *status;
    sector_t old_pba = 0;
    sector_t new_pba;
    bool old_mapping;
    __u32 old_zone = IMR_LSM_ZONE_COMPACTION_NONE;
    __u32 zone_idx;
    __u32 slot;
    int ret;

    if(key >= ((__u64)IMR_NUMZONES << IMR_ZONE_SIZE_SHIFT) ||
       key >= (__u64)IMR_LSM_KEY_EMPTY){
        return IMR_ERR_OUT_RANGE;
    }

    ret = imr_lsm_activate_zone_locked(&zone_idx);
    if(ret){
        /* A sealed invalid zone may already be reclaimable. */
        imr_lsm_record_zone_compaction_candidate(0);
        if(READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
            imr_lsm_queue_zone_compaction_auto_work();
        }
        return ret;
    }

    status = &zone_status[zone_idx];
    slot = status->z_map_size;
    if(slot >= TOTAL_ITEMS){
        status->z_conds = Z_COND_FULL;
        imr_lsm_allocator.active_zone = IMR_LSM_ZONE_COMPACTION_NONE;
        return -ENOSPC;
    }
    new_pba = imr_lsm_zone_append_pba(zone_idx, slot);
    old_mapping = imr_lsm_forward_map_lookup_locked(key, &old_pba);
    if(old_mapping){
        old_zone = imrsim_lba_zone_idx(old_pba);
        if(old_zone >= IMR_NUMZONES ||
           !zone_status[old_zone].z_live_count){
            return -EUCLEAN;
        }
    }

    status->z_key_map[slot] = (__u32)key;
    status->z_map_size++;
    status->z_live_count++;
    if(status->z_map_size == TOTAL_ITEMS){
        status->z_conds = Z_COND_FULL;
        imr_lsm_allocator.active_zone = IMR_LSM_ZONE_COMPACTION_NONE;
        printk(KERN_INFO "imrsim: IMR-LSM sealed full physical zone=%u generation=%llu\n",
               zone_idx,
               (unsigned long long)status->z_generation);
    }

    ret = imr_lsm_record_insert_and_check_zone_full(
        zone_idx, logical_lba, new_pba);
    if(ret){
        status->z_map_size--;
        status->z_live_count--;
        status->z_key_map[slot] = IMR_LSM_KEY_EMPTY;
        status->z_conds = Z_COND_OPEN;
        imr_lsm_allocator.active_zone = zone_idx;
        return ret;
    }

    ret = imr_lsm_forward_map_set_locked(key, new_pba);
    if(ret){
        return ret;
    }
    if(old_mapping){
        zone_status[old_zone].z_live_count--;
        imrsim_ptask_queue_zone_status_locked(old_zone);
    }

    if(slot >= imr_lsm_bottom_range_blocks()){
        __u32 top_slot = slot - imr_lsm_bottom_range_blocks();
        __u32 track = top_slot / IMR_TOP_TRACK_SIZE;
        __u32 block = top_slot % IMR_TOP_TRACK_SIZE;

        zone_status[zone_idx].z_tracks[track].isUsedBlock[block] = 1;
    }
    imrsim_ptask_queue_zone_status_locked(zone_idx);
    imr_lsm_record_zone_compaction_candidate(zone_idx);
    *physical_lba = new_pba;
    *physical_zone = zone_idx;
    return 0;
}

static enum imr_lsm_lookup_result imr_lsm_read(__u64 key, sector_t *pba)
{
    struct imr_lsm_unsorted_node *node;
    struct imr_lsm_sorted_node *sorted_node;
    enum imr_lsm_lookup_result result = IMR_LSM_LOOKUP_MISS;
    __u64 newest_timestamp = 0;
    __u8 newest_valid = 0;
    enum imr_lsm_read_source newest_source = IMR_LSM_READ_SOURCE_NONE;
    sector_t newest_pba = 0;
    __u64 tree_timestamp = 0;
    __u32 level;
    struct imr_lsm_read_filter_stats filter = {0};

    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    imr_lsm_meta.stats.read_lookup_count++;
    if(!imr_lsm_meta.initialized){
        imr_lsm_meta.stats.read_miss_count++;
        mutex_unlock(&imr_lsm_lock);
        return IMR_LSM_LOOKUP_MISS;
    }

    result = imr_lsm_tree_lookup_locked(key, &newest_pba, &tree_timestamp);
    if(result == IMR_LSM_LOOKUP_VALID){
        *pba = newest_pba;
        imr_lsm_meta.stats.tree_hit_count++;
        IMRSIM_DATA_LOG("imrsim: IMR-LSM read tree hit key=%llu pba=%llu ts=%llu\n",
                        (unsigned long long)key,
                        (unsigned long long)*pba,
                        (unsigned long long)tree_timestamp);
        mutex_unlock(&imr_lsm_lock);
        return result;
    }
    if(result == IMR_LSM_LOOKUP_DELETED){
        imr_lsm_meta.stats.tombstone_hit_count++;
        IMRSIM_DATA_LOG("imrsim: IMR-LSM read tree tombstone key=%llu ts=%llu\n",
                        (unsigned long long)key,
                        (unsigned long long)tree_timestamp);
        mutex_unlock(&imr_lsm_lock);
        return result;
    }

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        node = imr_lsm_meta.levels[level].unsorted_head;
        while(node){
            if(node->key == key && node->timestamp > newest_timestamp){
                newest_timestamp = node->timestamp;
                newest_valid = node->valid;
                newest_source = IMR_LSM_READ_SOURCE_UNSORTED;
                newest_pba = node->pba;
                result = node->valid ? IMR_LSM_LOOKUP_VALID :
                         IMR_LSM_LOOKUP_DELETED;
            }
            node = node->next;
        }
    }

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_block_entry level_entry;
        bool level_hit;
        bool scan_sorted_fallback;

        scan_sorted_fallback =
            imr_lsm_segment_lookup_level_locked(level, key, &filter,
                                                &level_entry, &level_hit);
        if(level_hit && level_entry.timestamp > newest_timestamp){
            newest_timestamp = level_entry.timestamp;
            newest_valid = level_entry.valid;
            newest_source = IMR_LSM_READ_SOURCE_SEGMENT;
            newest_pba = level_entry.pba;
            result = level_entry.valid ? IMR_LSM_LOOKUP_VALID :
                     IMR_LSM_LOOKUP_DELETED;
        }

        if(!scan_sorted_fallback ||
           !imr_lsm_meta.levels[level].sorted_count){
            continue;
        }

        sorted_node = imr_lsm_meta.levels[level].sorted_head;
        while(sorted_node){
            if(sorted_node->key == key){
                if(sorted_node->timestamp > newest_timestamp){
                    newest_timestamp = sorted_node->timestamp;
                    newest_valid = sorted_node->valid;
                    newest_source = IMR_LSM_READ_SOURCE_SORTED;
                    newest_pba = sorted_node->pba;
                    result = sorted_node->valid ? IMR_LSM_LOOKUP_VALID :
                             IMR_LSM_LOOKUP_DELETED;
                }
                break;
            }
            if(sorted_node->key > key){
                break;
            }
            sorted_node = sorted_node->next;
        }
    }

    imr_lsm_meta.stats.segment_lookup_count += filter.segment_lookup_count;
    imr_lsm_meta.stats.segment_skip_count += filter.segment_skip_count;
    imr_lsm_meta.stats.segment_candidate_count +=
        filter.segment_candidate_count;
    imr_lsm_meta.stats.bloom_lookup_count += filter.bloom_lookup_count;
    imr_lsm_meta.stats.bloom_negative_count += filter.bloom_negative_count;
    imr_lsm_meta.stats.bloom_maybe_count += filter.bloom_maybe_count;
    imr_lsm_meta.stats.block_table_lookup_count +=
        filter.block_table_lookup_count;
    imr_lsm_meta.stats.block_table_hit_count += filter.block_table_hit_count;
    imr_lsm_meta.stats.block_table_miss_count += filter.block_table_miss_count;
    imr_lsm_meta.stats.last_segment_read_key = key;
    imr_lsm_meta.stats.last_segment_lookup_count =
        filter.segment_lookup_count;
    imr_lsm_meta.stats.last_segment_skip_count = filter.segment_skip_count;
    imr_lsm_meta.stats.last_segment_candidate_count =
        filter.segment_candidate_count;
    imr_lsm_meta.stats.last_bloom_lookup_count = filter.bloom_lookup_count;
    imr_lsm_meta.stats.last_bloom_negative_count = filter.bloom_negative_count;
    imr_lsm_meta.stats.last_bloom_maybe_count = filter.bloom_maybe_count;
    imr_lsm_meta.stats.last_block_table_lookup_count =
        filter.block_table_lookup_count;
    imr_lsm_meta.stats.last_block_table_hit_count =
        filter.block_table_hit_count;
    imr_lsm_meta.stats.last_block_table_miss_count =
        filter.block_table_miss_count;
    if(filter.block_table_hit){
        imr_lsm_meta.stats.last_block_table_hit_key =
            filter.block_table_entry.key;
        imr_lsm_meta.stats.last_block_table_hit_pba =
            filter.block_table_entry.pba;
        imr_lsm_meta.stats.last_block_table_hit_timestamp =
            filter.block_table_entry.timestamp;
        imr_lsm_meta.stats.last_block_table_hit_valid =
            filter.block_table_entry.valid ? 1 : 0;
    }else{
        imr_lsm_meta.stats.last_block_table_hit_key = 0;
        imr_lsm_meta.stats.last_block_table_hit_pba = 0;
        imr_lsm_meta.stats.last_block_table_hit_timestamp = 0;
        imr_lsm_meta.stats.last_block_table_hit_valid = 0;
    }

    if(result == IMR_LSM_LOOKUP_VALID){
        *pba = newest_pba;
        imr_lsm_tree_update_locked(key, newest_pba, newest_valid,
                                   newest_timestamp);
        if(newest_valid){
            if(newest_source == IMR_LSM_READ_SOURCE_UNSORTED){
                imr_lsm_meta.stats.unsorted_hit_count++;
            }else if(newest_source == IMR_LSM_READ_SOURCE_SEGMENT){
                imr_lsm_meta.stats.segment_hit_count++;
            }else{
                imr_lsm_meta.stats.sorted_hit_count++;
            }
            IMRSIM_DATA_LOG("imrsim: IMR-LSM read hit key=%llu pba=%llu ts=%llu\n",
                            (unsigned long long)key,
                            (unsigned long long)*pba,
                            (unsigned long long)newest_timestamp);
        }
    }else if(result == IMR_LSM_LOOKUP_DELETED){
        imr_lsm_tree_update_locked(key, newest_pba, newest_valid,
                                   newest_timestamp);
        imr_lsm_meta.stats.tombstone_hit_count++;
        IMRSIM_DATA_LOG("imrsim: IMR-LSM read tombstone key=%llu ts=%llu\n",
                        (unsigned long long)key,
                        (unsigned long long)newest_timestamp);
    }else{
        imr_lsm_meta.stats.read_miss_count++;
    }
    mutex_unlock(&imr_lsm_lock);

    return result;
}

/*
 * Delete is append-only: record a valid=0 tombstone instead of modifying disk
 * data in place. The read path treats the newest tombstone as authoritative.
 * Caller holds imr_lsm_lock.
 */
static int imr_lsm_delete_locked(__u64 key)
{
    int ret;

    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_calculate_dynamic_levels_locked();

    imr_lsm_tree_remove_locked(key);
    ret = imr_lsm_append_unsorted_node_locked(imr_lsm_meta.active_write_level,
                                              key, 0, 0, 0);
    if(!ret){
        imr_lsm_meta.stats.delete_count++;
    }
    if(ret){
        return ret;
    }

    IMRSIM_DATA_LOG("imrsim: IMR-LSM delete append tombstone key=%llu\n",
                    (unsigned long long)key);
    return 0;
}

/* Caller holds imrsim_zone_lock and imr_lsm_lock. */
static int imrsim_lsm_delete_key_locked(__u64 key)
{
    __u32 zone_idx;
    __u32 physical_zone;
    __u64 block_offset;
    sector_t old_pba;
    bool old_mapping;
    int ret;

    zone_idx = (__u32)(key >> IMR_ZONE_SIZE_SHIFT);
    if(!imrsim_target_ready_locked() || zone_idx >= IMR_NUMZONES){
        return IMR_ERR_OUT_RANGE;
    }

    block_offset = key - ((__u64)zone_idx << IMR_ZONE_SIZE_SHIFT);
    if(block_offset >= TOTAL_ITEMS){
        return IMR_ERR_OUT_RANGE;
    }

    old_mapping = imr_lsm_forward_map_lookup_locked(key, &old_pba);
    if(old_mapping){
        physical_zone = imrsim_lba_zone_idx(old_pba);
        if(physical_zone >= IMR_NUMZONES ||
           !zone_status[physical_zone].z_live_count){
            return -EUCLEAN;
        }
    }
    ret = imr_lsm_delete_locked(key);
    if(ret){
        return ret;
    }

    zone_status[zone_idx].z_pba_map[block_offset] = -1;
    imrsim_ptask_queue_zone_status_locked(zone_idx);
    if(old_mapping){
        zone_status[physical_zone].z_live_count--;
        imrsim_ptask_queue_zone_status_locked(physical_zone);
        imr_lsm_record_zone_compaction_candidate_locked(physical_zone);
        if(imr_lsm_meta.stats.zone_compaction_candidate_zone !=
           IMR_LSM_ZONE_COMPACTION_NONE){
            imr_lsm_defer_zone_compaction_auto_run_locked(
                imr_lsm_meta.stats.zone_compaction_candidate_zone);
        }
    }

    return 0;
}

int imrsim_lsm_delete_key(__u64 key)
{
    __u32 zone_idx = (__u32)(key >> IMR_ZONE_SIZE_SHIFT);
    sector_t zone_lba = (sector_t)zone_idx << IMR_BLOCK_SIZE_SHIFT <<
                        IMR_ZONE_SIZE_SHIFT;
    bool queue_compaction;
    int ret;

retry_zone_lock:
    imrsim_diag_timed_mutex_lock(
        &imrsim_zone_lock,
        &imrsim_diag.foreground_zone_lock_wait_count,
        &imrsim_diag.foreground_zone_lock_wait_total_ns,
        &imrsim_diag.foreground_zone_lock_wait_max_ns);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    if(imr_lsm_zone_copy_overlaps_locked(zone_lba, 1)){
        mutex_unlock(&imrsim_zone_lock);
        wait_event(imr_lsm_zone_copy_wait,
                   imr_lsm_zone_copy_wait_done(zone_lba, 1));
        goto retry_zone_lock;
    }
    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    ret = imrsim_lsm_delete_key_locked(key);
    queue_compaction = imr_lsm_meta.level_compaction_pending;
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    if(queue_compaction){
        imr_lsm_queue_level_compaction_work();
    }
    if(READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    return ret;
}
EXPORT_SYMBOL(imrsim_lsm_delete_key);

/*
 * Discard metadata is 4K-key granular.  Caller holds imrsim_zone_lock; this
 * helper takes imr_lsm_lock once so the zone map and tombstones are updated
 * under the documented zone -> LSM order.
 */
static int imrsim_lsm_discard_lba_range_locked(sector_t lba,
                                               sector_t sectors)
{
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    sector_t block_count;
    sector_t block_idx;
    __u64 first_key;
    __u64 last_key;
    bool queue_compaction;
    int ret = 0;

    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.stats.discard_bio_count++;
    imr_lsm_meta.stats.last_discard_lba = lba;
    imr_lsm_meta.stats.last_discard_sectors = sectors;
    imr_lsm_meta.stats.last_discard_error = 0;

    if(!imrsim_target_ready_locked()){
        ret = -ENODEV;
        goto out;
    }
    if(!sectors || ((lba | sectors) & (block_sectors - 1))){
        ret = -EINVAL;
        goto out;
    }
    if(sectors > (sector_t)IMR_MAX_DISCARD_BLOCKS * block_sectors){
        ret = -E2BIG;
        goto out;
    }
    if(lba > (sector_t)~0ULL - (sectors - 1)){
        ret = -ERANGE;
        goto out;
    }

    first_key = (__u64)(lba >> IMR_BLOCK_SIZE_SHIFT);
    block_count = sectors >> IMR_BLOCK_SIZE_SHIFT;
    last_key = first_key + (__u64)block_count - 1;
    if(last_key < first_key ||
       last_key >= ((__u64)IMR_NUMZONES << IMR_ZONE_SIZE_SHIFT)){
        ret = IMR_ERR_OUT_RANGE;
        goto out;
    }

    for(block_idx = 0; block_idx < block_count; block_idx++){
        ret = imrsim_lsm_delete_key_locked(first_key + block_idx);
        if(ret){
            goto out;
        }
    }

out:
    imr_lsm_meta.stats.last_discard_error = ret;
    if(ret){
        imr_lsm_meta.stats.discard_delete_failed_count++;
    }else{
        imr_lsm_meta.stats.discard_delete_count++;
    }
    queue_compaction = imr_lsm_meta.level_compaction_pending;
    mutex_unlock(&imr_lsm_lock);
    if(queue_compaction){
        imr_lsm_queue_level_compaction_work();
    }
    if(READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    return ret;
}

static void imr_lsm_record_fallback(void)
{
    imrsim_diag_timed_mutex_lock(
        &imr_lsm_lock,
        &imrsim_diag.foreground_lsm_lock_wait_count,
        &imrsim_diag.foreground_lsm_lock_wait_total_ns,
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    if(imr_lsm_meta.initialized){
        imr_lsm_meta.stats.fallback_count++;
    }
    mutex_unlock(&imr_lsm_lock);
}

static int imr_lsm_debugfs_unsorted_show(struct seq_file *seq, void *unused)
{
    __u32 level;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    if(READ_ONCE(imr_lsm_allocator.active_zone) !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "active_physical_zone: %u\n",
                   READ_ONCE(imr_lsm_allocator.active_zone));
    }else{
        seq_puts(seq, "active_physical_zone: none\n");
    }
    seq_printf(seq, "append_writes_inflight: %d\n",
               atomic_read(&imr_lsm_append_writes_inflight));
    seq_printf(seq, "allocator_free_cursor: %u\n",
               READ_ONCE(imr_lsm_allocator.free_cursor));
    seq_printf(seq, "allocator_next_generation: %llu\n",
               (unsigned long long)
               READ_ONCE(imr_lsm_allocator.next_generation));
    seq_printf(seq, "timestamp: %llu\n",
               (unsigned long long)imr_lsm_meta.timestamp);
    imr_lsm_debugfs_show_active_target_locked(seq);
    seq_printf(seq, "node_limit_per_level: %u (0 means unlimited)\n",
               IMR_LSM_DEBUG_NODE_LIMIT);
    seq_printf(seq, "compaction_base: %u\n",
               imr_lsm_compaction_threshold_locked());
    seq_printf(seq, "level_ratio: %u\n",
               imr_lsm_max_bytes_for_level_multiplier_locked());

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_unsorted_node *node;
        __u32 trigger_capacity = imr_lsm_level_capacity(level);
        __u32 idx = 0;

        seq_printf(seq, "\nlevel %u unsorted_count: %u dynamic_target: %u logical_segments: %u dynamic_target_bytes: %llu actual_bytes: %llu\n",
                   level, imr_lsm_meta.levels[level].unsorted_count,
                   trigger_capacity,
                   imr_lsm_segment_count_locked(level),
                   (unsigned long long)
                   imr_lsm_level_target_bytes_locked(level),
                   (unsigned long long)
                   imr_lsm_level_actual_bytes_locked(level));
        seq_puts(seq, "idx key pba zone valid timestamp\n");

        node = imr_lsm_meta.levels[level].unsorted_head;
        while(node &&
              (!IMR_LSM_DEBUG_NODE_LIMIT || idx < IMR_LSM_DEBUG_NODE_LIMIT)){
            seq_printf(seq, "%u %llu %llu %u %u %llu\n",
                       idx,
                       (unsigned long long)node->key,
                       (unsigned long long)node->pba,
                       node->zone_idx,
                       node->valid,
                       (unsigned long long)node->timestamp);
            node = node->next;
            idx++;
        }
        if(node){
            seq_puts(seq, "...\n");
        }
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_unsorted_open(struct inode *inode, struct file *file)
{
    return single_open(file, imr_lsm_debugfs_unsorted_show, inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_unsorted_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_unsorted_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_sorted_show(struct seq_file *seq, void *unused)
{
    __u32 level;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_printf(seq, "timestamp: %llu\n",
               (unsigned long long)imr_lsm_meta.timestamp);
    imr_lsm_debugfs_show_active_target_locked(seq);
    seq_printf(seq, "node_limit_per_level: %u (0 means unlimited)\n",
               IMR_LSM_DEBUG_NODE_LIMIT);
    seq_printf(seq, "compaction_base: %u\n",
               imr_lsm_compaction_threshold_locked());
    seq_printf(seq, "level_ratio: %u\n",
               imr_lsm_max_bytes_for_level_multiplier_locked());

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_sorted_node *node;
        __u32 trigger_capacity = imr_lsm_level_capacity(level);
        __u32 idx = 0;

        seq_printf(seq, "\nlevel %u sorted_count: %u dynamic_target: %u logical_segments: %u dynamic_target_bytes: %llu actual_bytes: %llu\n",
                   level, imr_lsm_meta.levels[level].sorted_count,
                   trigger_capacity,
                   imr_lsm_segment_count_locked(level),
                   (unsigned long long)
                   imr_lsm_level_target_bytes_locked(level),
                   (unsigned long long)
                   imr_lsm_level_actual_bytes_locked(level));
        seq_puts(seq, "idx key pba zone valid timestamp\n");

        node = imr_lsm_meta.levels[level].sorted_head;
        while(node &&
              (!IMR_LSM_DEBUG_NODE_LIMIT || idx < IMR_LSM_DEBUG_NODE_LIMIT)){
            seq_printf(seq, "%u %llu %llu %u %u %llu\n",
                       idx,
                       (unsigned long long)node->key,
                       (unsigned long long)node->pba,
                       node->zone_idx,
                       node->valid,
                       (unsigned long long)node->timestamp);
            node = node->next;
            idx++;
        }
        if(node){
            seq_puts(seq, "...\n");
        }
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_sorted_open(struct inode *inode, struct file *file)
{
    return single_open(file, imr_lsm_debugfs_sorted_show, inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_sorted_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_sorted_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static const char *imr_lsm_segment_track_name(__u8 track_type)
{
    switch(track_type){
    case IMR_LSM_TRACK_BOTTOM:
        return "bottom";
    case IMR_LSM_TRACK_TOP:
        return "top";
    default:
        return "unknown";
    }
}

static const char *imr_lsm_segment_placement_name(__u8 placement_policy)
{
    switch(placement_policy){
    case IMR_LSM_PLACEMENT_BOTTOM_TO_TOP:
        return "bottom_to_top";
    case IMR_LSM_PLACEMENT_NONE:
        return "none";
    default:
        return "unknown";
    }
}

static const char *imr_lsm_segment_state_name(
    const struct imr_lsm_segment *segment)
{
    return segment->retired ? "retired" : "active";
}

static __u64 imr_lsm_segment_bloom_word(
    const struct imr_lsm_segment *segment, __u32 word_idx)
{
    if(!segment->bloom_bits || word_idx >= segment->bloom_word_count){
        return 0;
    }

    return segment->bloom_bits[word_idx];
}

static int imr_lsm_debugfs_segments_show(struct seq_file *seq, void *unused)
{
    struct imr_lsm_segment *segment;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_printf(seq, "next_segment_id: %u\n", imr_lsm_meta.next_segment_id);
    seq_printf(seq, "segment_count: %u\n", imr_lsm_meta.segment_count);
    seq_puts(seq, "id level zone track state nodes min_key max_key min_ts max_ts bloom_keys bloom_bits bloom_words bloom_hashes table_entries live invalid obsolete tombstone delete_invalid obsolete_ratio_permille placement target bottom_track_start bottom_track_end top_track_start top_track_end output_allocated output_track output_blocks output_pba_start output_pba_end bloom0 bloom1 bloom2 bloom3\n");

    segment = imr_lsm_meta.segment_head;
    while(segment){
        if(segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED){
            seq_printf(seq, "%u L%u mixed %s %s %u %llu %llu %llu %llu %u %u %u %u %u %u %u %u %u %u %u %s %s %u %u %u %u %u %s %u %llu %llu %016llx %016llx %016llx %016llx\n",
                       segment->id,
                       segment->level,
                       imr_lsm_segment_track_name(segment->track_type),
                       imr_lsm_segment_state_name(segment),
                       segment->node_count,
                       (unsigned long long)segment->min_key,
                       (unsigned long long)segment->max_key,
                       (unsigned long long)segment->min_timestamp,
                       (unsigned long long)segment->max_timestamp,
                       segment->bloom_key_count,
                       segment->bloom_bits_count,
                       segment->bloom_word_count,
                       segment->bloom_hash_count,
                       segment->block_table_count,
                       segment->live_count,
                       segment->invalid_count,
                       segment->obsolete_count,
                       segment->tombstone_count,
                       segment->delete_invalid_count,
                       segment->obsolete_ratio_permille,
                       imr_lsm_segment_placement_name(segment->placement_policy),
                       imr_lsm_segment_track_name(segment->placement_target_track_type),
                       segment->placement_bottom_track_start,
                       segment->placement_bottom_track_end,
                       segment->placement_top_track_start,
                       segment->placement_top_track_end,
                       segment->output_allocated,
                       imr_lsm_segment_track_name(segment->output_track_type),
                       segment->output_block_count,
                       (unsigned long long)segment->output_pba_start,
                       (unsigned long long)segment->output_pba_end,
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 0),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 1),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 2),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 3));
        }else{
            seq_printf(seq, "%u L%u %u %s %s %u %llu %llu %llu %llu %u %u %u %u %u %u %u %u %u %u %u %s %s %u %u %u %u %u %s %u %llu %llu %016llx %016llx %016llx %016llx\n",
                       segment->id,
                       segment->level,
                       segment->zone_idx,
                       imr_lsm_segment_track_name(segment->track_type),
                       imr_lsm_segment_state_name(segment),
                       segment->node_count,
                       (unsigned long long)segment->min_key,
                       (unsigned long long)segment->max_key,
                       (unsigned long long)segment->min_timestamp,
                       (unsigned long long)segment->max_timestamp,
                       segment->bloom_key_count,
                       segment->bloom_bits_count,
                       segment->bloom_word_count,
                       segment->bloom_hash_count,
                       segment->block_table_count,
                       segment->live_count,
                       segment->invalid_count,
                       segment->obsolete_count,
                       segment->tombstone_count,
                       segment->delete_invalid_count,
                       segment->obsolete_ratio_permille,
                       imr_lsm_segment_placement_name(segment->placement_policy),
                       imr_lsm_segment_track_name(segment->placement_target_track_type),
                       segment->placement_bottom_track_start,
                       segment->placement_bottom_track_end,
                       segment->placement_top_track_start,
                       segment->placement_top_track_end,
                       segment->output_allocated,
                       imr_lsm_segment_track_name(segment->output_track_type),
                       segment->output_block_count,
                       (unsigned long long)segment->output_pba_start,
                       (unsigned long long)segment->output_pba_end,
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 0),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 1),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 2),
                       (unsigned long long)imr_lsm_segment_bloom_word(segment, 3));
        }
        segment = segment->next;
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_segments_open(struct inode *inode, struct file *file)
{
    return single_open(file, imr_lsm_debugfs_segments_show, inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_segments_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_segments_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_placement_show(struct seq_file *seq, void *unused)
{
    struct imr_lsm_segment *segment;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_puts(seq, "segment_id level zone policy target bottom_key_start bottom_key_end bottom_track_start bottom_track_end top_track_start top_track_end top_pba_start top_pba_end output_allocated output_track output_blocks output_pba_start output_pba_end\n");

    segment = imr_lsm_meta.segment_head;
    while(segment){
        if(segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED){
            seq_printf(seq, "%u L%u mixed %s %s %llu %llu %u %u %u %u %llu %llu %u %s %u %llu %llu\n",
                       segment->id,
                       segment->level,
                       imr_lsm_segment_placement_name(segment->placement_policy),
                       imr_lsm_segment_track_name(segment->placement_target_track_type),
                       (unsigned long long)segment->placement_bottom_key_start,
                       (unsigned long long)segment->placement_bottom_key_end,
                       segment->placement_bottom_track_start,
                       segment->placement_bottom_track_end,
                       segment->placement_top_track_start,
                       segment->placement_top_track_end,
                       (unsigned long long)segment->placement_top_pba_start,
                       (unsigned long long)segment->placement_top_pba_end,
                       segment->output_allocated,
                       imr_lsm_segment_track_name(segment->output_track_type),
                       segment->output_block_count,
                       (unsigned long long)segment->output_pba_start,
                       (unsigned long long)segment->output_pba_end);
        }else{
            seq_printf(seq, "%u L%u %u %s %s %llu %llu %u %u %u %u %llu %llu %u %s %u %llu %llu\n",
                       segment->id,
                       segment->level,
                       segment->zone_idx,
                       imr_lsm_segment_placement_name(segment->placement_policy),
                       imr_lsm_segment_track_name(segment->placement_target_track_type),
                       (unsigned long long)segment->placement_bottom_key_start,
                       (unsigned long long)segment->placement_bottom_key_end,
                       segment->placement_bottom_track_start,
                       segment->placement_bottom_track_end,
                       segment->placement_top_track_start,
                       segment->placement_top_track_end,
                       (unsigned long long)segment->placement_top_pba_start,
                       (unsigned long long)segment->placement_top_pba_end,
                       segment->output_allocated,
                       imr_lsm_segment_track_name(segment->output_track_type),
                       segment->output_block_count,
                       (unsigned long long)segment->output_pba_start,
                       (unsigned long long)segment->output_pba_end);
        }
        segment = segment->next;
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_placement_open(struct inode *inode,
                                          struct file *file)
{
    return single_open(file, imr_lsm_debugfs_placement_show,
                       inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_placement_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_placement_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_obsolete_show(struct seq_file *seq, void *unused)
{
    struct imr_lsm_segment *segment;

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_recalculate_segment_invalid_stats_locked();
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_puts(seq, "segment_id level zone state entries live invalid obsolete tombstone delete_invalid obsolete_ratio_permille\n");

    segment = imr_lsm_meta.segment_head;
    while(segment){
        if(segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED){
            seq_printf(seq, "%u L%u mixed %s %u %u %u %u %u %u %u\n",
                       segment->id,
                       segment->level,
                       imr_lsm_segment_state_name(segment),
                       segment->block_table_count,
                       segment->live_count,
                       segment->invalid_count,
                       segment->obsolete_count,
                       segment->tombstone_count,
                       segment->delete_invalid_count,
                       segment->obsolete_ratio_permille);
        }else{
            seq_printf(seq, "%u L%u %u %s %u %u %u %u %u %u %u\n",
                       segment->id,
                       segment->level,
                       segment->zone_idx,
                       imr_lsm_segment_state_name(segment),
                       segment->block_table_count,
                       segment->live_count,
                       segment->invalid_count,
                       segment->obsolete_count,
                       segment->tombstone_count,
                       segment->delete_invalid_count,
                       segment->obsolete_ratio_permille);
        }
        segment = segment->next;
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);

    return 0;
}

static int imr_lsm_debugfs_obsolete_open(struct inode *inode,
                                         struct file *file)
{
    return single_open(file, imr_lsm_debugfs_obsolete_show,
                       inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_obsolete_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_obsolete_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_compaction_policy_show(struct seq_file *seq,
                                                  void *unused)
{
    struct imr_lsm_segment *segment;

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_recalculate_segment_invalid_stats_locked();
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_printf(seq, "min_obsolete_ratio_permille: %u\n",
               IMR_LSM_COMPACTION_MIN_OBSOLETE_RATIO);
    seq_printf(seq, "min_invalid_count: %u\n",
               IMR_LSM_COMPACTION_MIN_INVALID);
    seq_printf(seq, "delete_invalid_boost: %u\n",
               IMR_LSM_COMPACTION_DELETE_BOOST);
    seq_printf(seq, "tombstone_boost: %u\n",
               IMR_LSM_COMPACTION_TOMBSTONE_BOOST);
    seq_printf(seq, "policy_age_weight: %d\n",
               IMR_LSM_COMPACTION_POLICY_AGE_WEIGHT);
    seq_printf(seq, "policy_hotness_weight: %d\n",
               IMR_LSM_COMPACTION_POLICY_HOTNESS_WEIGHT);
    seq_printf(seq, "policy_placement_weight: %d\n",
               IMR_LSM_COMPACTION_POLICY_PLACEMENT_WEIGHT);
    seq_printf(seq, "policy_rmw_weight: %d\n",
               IMR_LSM_COMPACTION_POLICY_RMW_WEIGHT);
    seq_printf(seq, "policy_zone_fullness_weight: %d\n",
               IMR_LSM_COMPACTION_POLICY_ZONE_FULLNESS_WEIGHT);
    seq_printf(seq, "candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_candidate_count);
    if(imr_lsm_meta.stats.segment_compaction_candidate_score){
        struct imr_lsm_compaction_policy_score selected_policy;
        struct imr_lsm_segment *selected_segment = NULL;

        seq_printf(seq, "selected_segment_id: %u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_segment_id);
        seq_printf(seq, "selected_level: L%u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_level);
        seq_printf(seq, "selected_score: %llu\n",
                   (unsigned long long)imr_lsm_meta.stats.segment_compaction_candidate_score);
        seq_printf(seq, "selected_obsolete_ratio_permille: %u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_ratio_permille);
        seq_printf(seq, "selected_invalid_count: %u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_invalid_count);
        seq_printf(seq, "selected_delete_invalid_count: %u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_delete_invalid_count);
        seq_printf(seq, "selected_tombstone_count: %u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_tombstone_count);
        segment = imr_lsm_meta.segment_head;
        while(segment){
            if(!segment->retired &&
               segment->id ==
               imr_lsm_meta.stats.segment_compaction_candidate_segment_id){
                selected_segment = segment;
                break;
            }
            segment = segment->next;
        }
        if(selected_segment){
            imr_lsm_segment_policy_score_locked(selected_segment,
                                                &selected_policy);
            seq_printf(seq, "selected_final_score: %lld\n",
                       (long long)selected_policy.final_score);
        }
    }else{
        seq_puts(seq, "selected_segment_id: none\n");
    }

    seq_puts(seq, "segment_id level zone state entries invalid obsolete_ratio_permille delete_invalid tombstone candidate score age read_hotness placement_cost rmw_cost zone_fullness final_score\n");
    segment = imr_lsm_meta.segment_head;
    while(segment){
        struct imr_lsm_compaction_policy_score policy;

        imr_lsm_segment_policy_score_locked(segment, &policy);
        if(segment->zone_idx == IMR_LSM_SEGMENT_ZONE_MIXED){
            seq_printf(seq, "%u L%u mixed %s %u %u %u %u %u %u %llu %llu %llu %u %u %u %lld\n",
                       segment->id,
                       segment->level,
                       imr_lsm_segment_state_name(segment),
                       segment->block_table_count,
                       segment->invalid_count,
                       segment->obsolete_ratio_permille,
                       segment->delete_invalid_count,
                       segment->tombstone_count,
                       segment->compaction_candidate ? 1 : 0,
                       (unsigned long long)segment->compaction_score,
                       (unsigned long long)policy.age,
                       (unsigned long long)policy.read_hotness,
                       policy.placement_cost,
                       policy.rmw_cost,
                       policy.zone_fullness,
                       (long long)policy.final_score);
        }else{
            seq_printf(seq, "%u L%u %u %s %u %u %u %u %u %u %llu %llu %llu %u %u %u %lld\n",
                       segment->id,
                       segment->level,
                       segment->zone_idx,
                       imr_lsm_segment_state_name(segment),
                       segment->block_table_count,
                       segment->invalid_count,
                       segment->obsolete_ratio_permille,
                       segment->delete_invalid_count,
                       segment->tombstone_count,
                       segment->compaction_candidate ? 1 : 0,
                       (unsigned long long)segment->compaction_score,
                       (unsigned long long)policy.age,
                       (unsigned long long)policy.read_hotness,
                       policy.placement_cost,
                       policy.rmw_cost,
                       policy.zone_fullness,
                       (long long)policy.final_score);
        }
        segment = segment->next;
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);

    return 0;
}

static int imr_lsm_debugfs_compaction_policy_open(struct inode *inode,
                                                  struct file *file)
{
    return single_open(file, imr_lsm_debugfs_compaction_policy_show,
                       inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_compaction_policy_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_compaction_policy_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_block_table_show(struct seq_file *seq,
                                            void *unused)
{
    struct imr_lsm_segment *segment;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    seq_puts(seq, "segment_id level state entry key pba source_valid source_pba output_mapped output_pba copy_planned output_copied output_committed valid timestamp\n");

    segment = imr_lsm_meta.segment_head;
    while(segment){
        __u32 entry_idx;

        for(entry_idx = 0; entry_idx < segment->block_table_count;
            entry_idx++){
            struct imr_lsm_block_entry *entry =
                &segment->block_table[entry_idx];

            seq_printf(seq, "%u L%u %s %u %llu %llu %u %llu %u %llu %u %u %u %u %llu\n",
                       segment->id,
                       segment->level,
                       imr_lsm_segment_state_name(segment),
                       entry_idx,
                       (unsigned long long)entry->key,
                       (unsigned long long)entry->pba,
                       entry->source_pba_valid ? 1 : 0,
                       (unsigned long long)entry->source_pba,
                       entry->output_mapped ? 1 : 0,
                       (unsigned long long)entry->output_pba,
                       entry->output_copy_planned ? 1 : 0,
                       entry->output_copied ? 1 : 0,
                       entry->output_committed ? 1 : 0,
                       entry->valid ? 1 : 0,
                       (unsigned long long)entry->timestamp);
        }
        segment = segment->next;
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_block_table_open(struct inode *inode,
                                            struct file *file)
{
    return single_open(file, imr_lsm_debugfs_block_table_show,
                       inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_block_table_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_block_table_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_read_tree_show(struct seq_file *seq, void *unused)
{
    struct imr_lsm_read_tree_node *node;
    __u32 idx = 0;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "capacity: %u\n", imr_lsm_read_tree_limit_locked());
    seq_printf(seq, "size: %u\n", imr_lsm_meta.read_tree_size);
    seq_puts(seq, "idx key pba valid timestamp\n");
    list_for_each_entry(node, &imr_lsm_meta.read_lru, lru){
        if(IMR_LSM_DEBUG_NODE_LIMIT && idx >= IMR_LSM_DEBUG_NODE_LIMIT){
            seq_puts(seq, "...\n");
            break;
        }
        seq_printf(seq, "%u %llu %llu %u %llu\n",
                   idx,
                   (unsigned long long)node->key,
                   (unsigned long long)node->pba,
                   node->valid ? 1 : 0,
                   (unsigned long long)node->timestamp);
        idx++;
    }
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_read_tree_open(struct inode *inode,
                                          struct file *file)
{
    return single_open(file, imr_lsm_debugfs_read_tree_show,
                       inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_read_tree_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_read_tree_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static ssize_t imr_lsm_debugfs_clear_read_tree_write(struct file *file,
                                                     const char __user *ubuf,
                                                     size_t count,
                                                     loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 run;
    __u32 cleared = 0;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &run);
    if(ret){
        return ret;
    }
    if(!run){
        return -EINVAL;
    }

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(imr_lsm_meta.initialized){
        cleared = imr_lsm_clear_read_tree_locked();
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);

    printk(KERN_INFO "imrsim: IMR-LSM cleared read tree entries=%u\n",
           cleared);
    return count;
}

static const struct file_operations imr_lsm_debugfs_clear_read_tree_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_clear_read_tree_write,
    .llseek = no_llseek,
};

static int imr_lsm_debugfs_read_tree_limit_show(struct seq_file *seq,
                                                void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n", imr_lsm_read_tree_limit_locked());
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_read_tree_limit_open(struct inode *inode,
                                                struct file *file)
{
    return single_open(file, imr_lsm_debugfs_read_tree_limit_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_read_tree_limit_write(struct file *file,
                                                     const char __user *ubuf,
                                                     size_t count,
                                                     loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 limit;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &limit);
    if(ret){
        return ret;
    }
    if(limit > IMR_LSM_READ_TREE_LIMIT){
        return -EINVAL;
    }
    if(!limit){
        limit = IMR_LSM_READ_TREE_LIMIT;
    }

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.read_tree_limit = limit;
    imr_lsm_tree_evict_locked();
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);

    printk(KERN_INFO "imrsim: IMR-LSM read tree limit=%u\n", limit);
    return count;
}

static const struct file_operations imr_lsm_debugfs_read_tree_limit_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_read_tree_limit_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_read_tree_limit_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_compaction_threshold_show(struct seq_file *seq,
                                                     void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n", imr_lsm_compaction_threshold_locked());
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_compaction_threshold_open(struct inode *inode,
                                                     struct file *file)
{
    return single_open(file, imr_lsm_debugfs_compaction_threshold_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_compaction_threshold_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 threshold;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &threshold);
    if(ret){
        return ret;
    }
    if(!threshold){
        threshold = IMR_LSM_COMPACTION_THRESHOLD;
    }
    if(threshold < IMR_LSM_COMPACTION_THRESHOLD_MIN ||
       threshold > IMR_LSM_COMPACTION_THRESHOLD_MAX){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_compaction_threshold = threshold;
    if(imr_lsm_meta.initialized){
        imr_lsm_calculate_dynamic_levels_locked();
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    printk(KERN_INFO "imrsim: IMR-LSM validation compaction threshold=%u\n",
           threshold);
    return count;
}

static const struct file_operations imr_lsm_debugfs_compaction_threshold_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_compaction_threshold_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_compaction_threshold_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_max_bytes_for_level_base_show(
    struct seq_file *seq, void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%llu\n", (unsigned long long)
               imr_lsm_max_bytes_for_level_base_locked());
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_max_bytes_for_level_base_open(
    struct inode *inode, struct file *file)
{
    return single_open(file,
                       imr_lsm_debugfs_max_bytes_for_level_base_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_max_bytes_for_level_base_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    unsigned long long bytes;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtoull(buf, 0, &bytes);
    if(ret){
        return ret;
    }
    if(!bytes){
        bytes = IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_DEFAULT;
    }
    if(bytes < IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_MIN ||
       bytes > IMR_LSM_MAX_BYTES_FOR_LEVEL_BASE_MAX){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_max_bytes_for_level_base = (__u64)bytes;
    if(imr_lsm_meta.initialized){
        imr_lsm_calculate_dynamic_levels_locked();
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    printk(KERN_INFO "imrsim: IMR-LSM validation max bytes for level base=%llu\n",
           bytes);
    return count;
}

static const struct file_operations
imr_lsm_debugfs_max_bytes_for_level_base_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_max_bytes_for_level_base_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_max_bytes_for_level_base_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_max_bytes_for_level_multiplier_show(
    struct seq_file *seq, void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n",
               imr_lsm_max_bytes_for_level_multiplier_locked());
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_max_bytes_for_level_multiplier_open(
    struct inode *inode, struct file *file)
{
    return single_open(file,
                       imr_lsm_debugfs_max_bytes_for_level_multiplier_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_max_bytes_for_level_multiplier_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 multiplier;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &multiplier);
    if(ret){
        return ret;
    }
    if(!multiplier){
        multiplier = IMR_LSM_LEVEL_RATIO_DEFAULT;
    }
    if(multiplier < IMR_LSM_LEVEL_RATIO_MIN ||
       multiplier > IMR_LSM_LEVEL_RATIO_MAX){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_max_bytes_for_level_multiplier = multiplier;
    if(imr_lsm_meta.initialized){
        imr_lsm_calculate_dynamic_levels_locked();
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    printk(KERN_INFO "imrsim: IMR-LSM validation max bytes for level multiplier=%u\n",
           multiplier);
    return count;
}

static const struct file_operations
imr_lsm_debugfs_max_bytes_for_level_multiplier_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_max_bytes_for_level_multiplier_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_max_bytes_for_level_multiplier_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_bloom_bits_per_key_show(struct seq_file *seq,
                                                   void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n", imr_lsm_bloom_bits_per_key_locked());
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_bloom_bits_per_key_open(struct inode *inode,
                                                   struct file *file)
{
    return single_open(file, imr_lsm_debugfs_bloom_bits_per_key_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_bloom_bits_per_key_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 bits_per_key;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &bits_per_key);
    if(ret){
        return ret;
    }
    if(!bits_per_key){
        bits_per_key = IMR_LSM_BLOOM_BITS_PER_KEY;
    }
    if(bits_per_key < IMR_LSM_BLOOM_BITS_PER_KEY_MIN ||
       bits_per_key > IMR_LSM_BLOOM_BITS_PER_KEY_MAX){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_bloom_bits_per_key = bits_per_key;
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    printk(KERN_INFO "imrsim: IMR-LSM validation bloom bits/key=%u\n",
           bits_per_key);
    return count;
}

static const struct file_operations imr_lsm_debugfs_bloom_bits_per_key_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_bloom_bits_per_key_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_bloom_bits_per_key_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_level_compaction_build_delay_ms_show(
    struct seq_file *seq, void *unused)
{
    seq_printf(seq, "%u\n",
               READ_ONCE(imr_lsm_level_compaction_build_delay_ms));
    return 0;
}

static int imr_lsm_debugfs_level_compaction_build_delay_ms_open(
    struct inode *inode, struct file *file)
{
    return single_open(
        file, imr_lsm_debugfs_level_compaction_build_delay_ms_show,
        inode->i_private);
}

static ssize_t imr_lsm_debugfs_level_compaction_build_delay_ms_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 delay_ms;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &delay_ms);
    if(ret){
        return ret;
    }
    if(delay_ms > IMR_LSM_LEVEL_COMPACTION_BUILD_DELAY_MS_MAX){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    WRITE_ONCE(imr_lsm_level_compaction_build_delay_ms, delay_ms);
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    printk(KERN_INFO "imrsim: IMR-LSM validation level compaction build delay=%u ms\n",
           delay_ms);
    return count;
}

static const struct file_operations
imr_lsm_debugfs_level_compaction_build_delay_ms_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_level_compaction_build_delay_ms_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_level_compaction_build_delay_ms_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static void imrsim_diag_seq_atomic64(struct seq_file *seq, const char *name,
                                     atomic64_t *value)
{
    seq_printf(seq, "%s: %llu\n", name,
               (unsigned long long)atomic64_read(value));
}

static int imr_lsm_debugfs_stats_show(struct seq_file *seq, void *unused)
{
    __u32 level;

    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    imr_lsm_debugfs_show_active_target_locked(seq);
    seq_printf(seq, "compaction_threshold: %u\n",
               imr_lsm_compaction_threshold_locked());
    seq_printf(seq, "max_bytes_for_level_base: %llu\n",
               (unsigned long long)
               imr_lsm_max_bytes_for_level_base_locked());
    seq_printf(seq, "max_bytes_for_level_multiplier: %u\n",
               imr_lsm_max_bytes_for_level_multiplier_locked());
    seq_printf(seq, "lsm_record_bytes: %llu\n",
               (unsigned long long)IMR_LSM_RECORD_BYTES);
    for(level = 0; level < IMR_LSM_LEVELS; level++){
        seq_printf(seq, "level%u_actual_bytes: %llu\n", level,
                   (unsigned long long)
                   imr_lsm_level_actual_bytes_locked(level));
        seq_printf(seq, "level%u_target_bytes: %llu\n", level,
                   (unsigned long long)
                   imr_lsm_level_target_bytes_locked(level));
    }
    seq_printf(seq, "bloom_bits_per_key: %u\n",
               imr_lsm_bloom_bits_per_key_locked());
    seq_printf(seq, "level_compaction_build_delay_ms: %u\n",
               READ_ONCE(imr_lsm_level_compaction_build_delay_ms));
    imrsim_diag_seq_atomic64(seq, "level_compaction_phase",
        &imrsim_diag.level_compaction_phase);
    seq_printf(seq, "logical_write_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.logical_write_count);
    seq_printf(seq, "lsm_record_insert_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.lsm_record_insert_count);
    seq_printf(seq, "lsm_write_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.lsm_write_count);
    seq_printf(seq, "delete_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.delete_count);
    seq_printf(seq, "discard_bio_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.discard_bio_count);
    seq_printf(seq, "discard_delete_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.discard_delete_count);
    seq_printf(seq, "discard_delete_failed_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.discard_delete_failed_count);
    seq_printf(seq, "last_discard_lba: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_discard_lba);
    seq_printf(seq, "last_discard_sectors: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_discard_sectors);
    seq_printf(seq, "last_discard_error: %d\n",
               imr_lsm_meta.stats.last_discard_error);
    seq_printf(seq, "read_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_lookup_count);
    seq_printf(seq, "read_miss_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_miss_count);
    seq_printf(seq, "segment_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_lookup_count);
    seq_printf(seq, "segment_skip_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_skip_count);
    seq_printf(seq, "segment_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_candidate_count);
    seq_printf(seq, "bloom_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.bloom_lookup_count);
    seq_printf(seq, "bloom_negative_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.bloom_negative_count);
    seq_printf(seq, "bloom_maybe_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.bloom_maybe_count);
    seq_printf(seq, "block_table_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.block_table_lookup_count);
    seq_printf(seq, "block_table_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.block_table_hit_count);
    seq_printf(seq, "block_table_miss_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.block_table_miss_count);
    seq_printf(seq, "read_tree_capacity: %u\n",
               imr_lsm_read_tree_limit_locked());
    seq_printf(seq, "read_tree_size: %u\n", imr_lsm_meta.read_tree_size);
    seq_printf(seq, "read_tree_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_lookup_count);
    seq_printf(seq, "read_tree_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_hit_count);
    seq_printf(seq, "read_tree_miss_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_miss_count);
    seq_printf(seq, "read_tree_update_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_update_count);
    seq_printf(seq, "read_tree_update_fail_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_update_fail_count);
    seq_printf(seq, "read_tree_remove_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_remove_count);
    seq_printf(seq, "read_tree_evict_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.read_tree_evict_count);
    seq_printf(seq, "newest_index_size: %u\n",
               imr_lsm_meta.newest_tree_size);
    seq_printf(seq, "newest_index_valid: %u\n",
               imr_lsm_meta.newest_index_valid ? 1 : 0);
    imrsim_diag_seq_atomic64(seq, "newest_index_lookup_count",
        &imrsim_diag.newest_index_lookup_count);
    imrsim_diag_seq_atomic64(seq, "newest_index_hit_count",
        &imrsim_diag.newest_index_hit_count);
    imrsim_diag_seq_atomic64(seq, "newest_index_miss_count",
        &imrsim_diag.newest_index_miss_count);
    imrsim_diag_seq_atomic64(seq, "newest_index_update_fail_count",
        &imrsim_diag.newest_index_update_fail_count);
    imrsim_diag_seq_atomic64(seq, "newest_index_fallback_count",
        &imrsim_diag.newest_index_fallback_count);
    seq_printf(seq, "last_read_tree_key: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_read_tree_key);
    seq_printf(seq, "last_read_tree_pba: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_read_tree_pba);
    seq_printf(seq, "last_read_tree_timestamp: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_read_tree_timestamp);
    seq_printf(seq, "last_read_tree_valid: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_read_tree_valid);
    seq_printf(seq, "last_read_tree_hit: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_read_tree_hit);
    seq_printf(seq, "last_segment_read_key: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_read_key);
    seq_printf(seq, "last_segment_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_lookup_count);
    seq_printf(seq, "last_segment_skip_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_skip_count);
    seq_printf(seq, "last_segment_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_candidate_count);
    seq_printf(seq, "last_bloom_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_bloom_lookup_count);
    seq_printf(seq, "last_bloom_negative_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_bloom_negative_count);
    seq_printf(seq, "last_bloom_maybe_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_bloom_maybe_count);
    seq_printf(seq, "last_block_table_lookup_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_lookup_count);
    seq_printf(seq, "last_block_table_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_hit_count);
    seq_printf(seq, "last_block_table_miss_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_miss_count);
    seq_printf(seq, "last_block_table_hit_key: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_hit_key);
    seq_printf(seq, "last_block_table_hit_pba: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_hit_pba);
    seq_printf(seq, "last_block_table_hit_timestamp: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_hit_timestamp);
    seq_printf(seq, "last_block_table_hit_valid: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_block_table_hit_valid);
    seq_printf(seq, "placement_policy_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_policy_count);
    seq_printf(seq, "placement_bottom_to_top_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_bottom_to_top_count);
    seq_printf(seq, "placement_no_target_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_no_target_count);
    seq_printf(seq, "placement_mixed_zone_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_mixed_zone_count);
    seq_printf(seq, "last_placement_segment_id: %u\n",
               imr_lsm_meta.stats.last_placement_segment_id);
    seq_printf(seq, "last_placement_policy: %s\n",
               imr_lsm_segment_placement_name(imr_lsm_meta.stats.last_placement_policy));
    seq_printf(seq, "last_placement_bottom_track_start: %u\n",
               imr_lsm_meta.stats.last_placement_bottom_track_start);
    seq_printf(seq, "last_placement_bottom_track_end: %u\n",
               imr_lsm_meta.stats.last_placement_bottom_track_end);
    seq_printf(seq, "last_placement_top_track_start: %u\n",
               imr_lsm_meta.stats.last_placement_top_track_start);
    seq_printf(seq, "last_placement_top_track_end: %u\n",
               imr_lsm_meta.stats.last_placement_top_track_end);
    seq_printf(seq, "last_placement_top_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_placement_top_pba_start);
    seq_printf(seq, "last_placement_top_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_placement_top_pba_end);
    seq_printf(seq, "placement_output_alloc_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_output_alloc_count);
    seq_printf(seq, "placement_output_no_target_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_output_no_target_count);
    seq_printf(seq, "placement_output_no_space_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.placement_output_no_space_count);
    seq_printf(seq, "last_placement_output_segment_id: %u\n",
               imr_lsm_meta.stats.last_placement_output_segment_id);
    seq_printf(seq, "last_placement_output_allocated: %u\n",
               imr_lsm_meta.stats.last_placement_output_allocated);
    seq_printf(seq, "last_placement_output_track: %s\n",
               imr_lsm_segment_track_name(imr_lsm_meta.stats.last_placement_output_track_type));
    seq_printf(seq, "last_placement_output_block_count: %u\n",
               imr_lsm_meta.stats.last_placement_output_block_count);
    seq_printf(seq, "last_placement_output_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_placement_output_pba_start);
    seq_printf(seq, "last_placement_output_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_placement_output_pba_end);
    seq_printf(seq, "invalid_recalc_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.invalid_recalc_count);
    imrsim_diag_seq_atomic64(seq, "invalid_recalc_total_ns",
        &imrsim_diag.invalid_recalc_total_ns);
    imrsim_diag_seq_atomic64(seq, "invalid_recalc_max_ns",
        &imrsim_diag.invalid_recalc_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_invalid_recalc_ns",
        &imrsim_diag.last_invalid_recalc_ns);
    imrsim_diag_seq_atomic64(seq,
        "invalid_recalc_segments_scanned_total",
        &imrsim_diag.invalid_recalc_segments_scanned_total);
    imrsim_diag_seq_atomic64(seq,
        "invalid_recalc_segments_scanned_max",
        &imrsim_diag.invalid_recalc_segments_scanned_max);
    imrsim_diag_seq_atomic64(seq,
        "invalid_recalc_entries_scanned_total",
        &imrsim_diag.invalid_recalc_entries_scanned_total);
    imrsim_diag_seq_atomic64(seq,
        "invalid_recalc_entries_scanned_max",
        &imrsim_diag.invalid_recalc_entries_scanned_max);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_supersede_count",
        &imrsim_diag.invalid_incremental_supersede_count);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_entries_updated",
        &imrsim_diag.invalid_incremental_entries_updated);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_segment_publish_count",
        &imrsim_diag.invalid_incremental_segment_publish_count);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_segment_publish_entries",
        &imrsim_diag.invalid_incremental_segment_publish_entries);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_segment_retire_count",
        &imrsim_diag.invalid_incremental_segment_retire_count);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_segment_retire_entries",
        &imrsim_diag.invalid_incremental_segment_retire_entries);
    imrsim_diag_seq_atomic64(seq,
        "invalid_incremental_fallback_recalc_count",
        &imrsim_diag.invalid_incremental_fallback_recalc_count);
    seq_printf(seq, "invalid_segment_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.invalid_segment_count);
    seq_printf(seq, "invalid_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.invalid_entry_count);
    seq_printf(seq, "obsolete_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.obsolete_entry_count);
    seq_printf(seq, "tombstone_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.tombstone_entry_count);
    seq_printf(seq, "delete_invalid_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.delete_invalid_entry_count);
    seq_printf(seq, "max_obsolete_ratio_permille: %u\n",
               imr_lsm_meta.stats.max_obsolete_ratio_permille);
    seq_printf(seq, "max_obsolete_segment_id: %u\n",
               imr_lsm_meta.stats.max_obsolete_segment_id);
    seq_printf(seq, "last_invalid_recalc_segments: %u\n",
               imr_lsm_meta.stats.last_invalid_recalc_segments);
    seq_printf(seq, "last_invalid_recalc_entries: %u\n",
               imr_lsm_meta.stats.last_invalid_recalc_entries);
    seq_printf(seq, "segment_compaction_selection_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_selection_count);
    seq_printf(seq, "segment_compaction_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_candidate_count);
    seq_printf(seq, "segment_compaction_no_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_no_candidate_count);
    seq_printf(seq, "segment_compaction_candidate_segment_id: %u\n",
               imr_lsm_meta.stats.segment_compaction_candidate_segment_id);
    if(imr_lsm_meta.stats.segment_compaction_candidate_level <
       IMR_LSM_LEVELS){
        seq_printf(seq, "segment_compaction_candidate_level: L%u\n",
                   imr_lsm_meta.stats.segment_compaction_candidate_level);
    }else{
        seq_puts(seq, "segment_compaction_candidate_level: none\n");
    }
    seq_printf(seq, "segment_compaction_candidate_score: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_candidate_score);
    seq_printf(seq, "segment_compaction_candidate_ratio_permille: %u\n",
               imr_lsm_meta.stats.segment_compaction_candidate_ratio_permille);
    seq_printf(seq, "segment_compaction_candidate_invalid_count: %u\n",
               imr_lsm_meta.stats.segment_compaction_candidate_invalid_count);
    seq_printf(seq, "segment_compaction_candidate_delete_invalid_count: %u\n",
               imr_lsm_meta.stats.segment_compaction_candidate_delete_invalid_count);
    seq_printf(seq, "segment_compaction_candidate_tombstone_count: %u\n",
               imr_lsm_meta.stats.segment_compaction_candidate_tombstone_count);
    seq_printf(seq, "segment_compaction_execute_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_execute_count);
    seq_printf(seq, "segment_compaction_execute_no_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_compaction_execute_no_candidate_count);
    if(imr_lsm_meta.stats.last_segment_compaction_from_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_compaction_from_id: %u\n",
                   imr_lsm_meta.stats.last_segment_compaction_from_id);
    }else{
        seq_puts(seq, "last_segment_compaction_from_id: none\n");
    }
    if(imr_lsm_meta.stats.last_segment_compaction_to_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_compaction_to_id: %u\n",
                   imr_lsm_meta.stats.last_segment_compaction_to_id);
    }else{
        seq_puts(seq, "last_segment_compaction_to_id: none\n");
    }
    seq_printf(seq, "last_segment_compaction_input_entries: %u\n",
               imr_lsm_meta.stats.last_segment_compaction_input_entries);
    seq_printf(seq, "last_segment_compaction_live_entries: %u\n",
               imr_lsm_meta.stats.last_segment_compaction_live_entries);
    seq_printf(seq, "last_segment_compaction_dropped_entries: %u\n",
               imr_lsm_meta.stats.last_segment_compaction_dropped_entries);
    seq_printf(seq, "segment_output_mapping_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_mapping_count);
    seq_printf(seq, "segment_output_mapping_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_mapping_entry_count);
    seq_printf(seq, "segment_output_mapping_no_output_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_mapping_no_output_count);
    if(imr_lsm_meta.stats.last_segment_output_mapping_segment_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_output_mapping_segment_id: %u\n",
                   imr_lsm_meta.stats.last_segment_output_mapping_segment_id);
    }else{
        seq_puts(seq, "last_segment_output_mapping_segment_id: none\n");
    }
    seq_printf(seq, "last_segment_output_mapping_entry_count: %u\n",
               imr_lsm_meta.stats.last_segment_output_mapping_entry_count);
    seq_printf(seq, "last_segment_output_mapping_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_mapping_pba_start);
    seq_printf(seq, "last_segment_output_mapping_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_mapping_pba_end);
    seq_printf(seq, "segment_output_copy_plan_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_copy_plan_count);
    seq_printf(seq, "segment_output_copy_plan_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_copy_plan_entry_count);
    seq_printf(seq, "segment_output_copy_plan_missing_mapping_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_copy_plan_missing_mapping_count);
    seq_printf(seq, "last_segment_output_copy_plan_segments: %u\n",
               imr_lsm_meta.stats.last_segment_output_copy_plan_segments);
    seq_printf(seq, "last_segment_output_copy_plan_entries: %u\n",
               imr_lsm_meta.stats.last_segment_output_copy_plan_entries);
    seq_printf(seq, "last_segment_output_copy_plan_missing_mappings: %u\n",
               imr_lsm_meta.stats.last_segment_output_copy_plan_missing_mappings);
    if(imr_lsm_meta.stats.last_segment_output_copy_plan_segment_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_output_copy_plan_segment_id: %u\n",
                   imr_lsm_meta.stats.last_segment_output_copy_plan_segment_id);
    }else{
        seq_puts(seq, "last_segment_output_copy_plan_segment_id: none\n");
    }
    seq_printf(seq, "last_segment_output_copy_plan_source_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_copy_plan_source_pba_start);
    seq_printf(seq, "last_segment_output_copy_plan_source_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_copy_plan_source_pba_end);
    seq_printf(seq, "last_segment_output_copy_plan_output_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_copy_plan_output_pba_start);
    seq_printf(seq, "last_segment_output_copy_plan_output_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_copy_plan_output_pba_end);
    seq_printf(seq, "segment_output_metadata_commit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_metadata_commit_count);
    seq_printf(seq, "segment_output_metadata_commit_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_metadata_commit_entry_count);
    seq_printf(seq, "segment_output_metadata_commit_already_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_metadata_commit_already_count);
    seq_printf(seq, "segment_output_metadata_commit_missing_plan_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_metadata_commit_missing_plan_count);
    seq_printf(seq, "last_segment_output_metadata_commit_segments: %u\n",
               imr_lsm_meta.stats.last_segment_output_metadata_commit_segments);
    seq_printf(seq, "last_segment_output_metadata_commit_entries: %u\n",
               imr_lsm_meta.stats.last_segment_output_metadata_commit_entries);
    seq_printf(seq, "last_segment_output_metadata_commit_already: %u\n",
               imr_lsm_meta.stats.last_segment_output_metadata_commit_already);
    seq_printf(seq, "last_segment_output_metadata_commit_missing_plan: %u\n",
               imr_lsm_meta.stats.last_segment_output_metadata_commit_missing_plan);
    if(imr_lsm_meta.stats.last_segment_output_metadata_commit_segment_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_output_metadata_commit_segment_id: %u\n",
                   imr_lsm_meta.stats.last_segment_output_metadata_commit_segment_id);
    }else{
        seq_puts(seq, "last_segment_output_metadata_commit_segment_id: none\n");
    }
    seq_printf(seq, "last_segment_output_metadata_commit_source_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_metadata_commit_source_pba_start);
    seq_printf(seq, "last_segment_output_metadata_commit_source_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_metadata_commit_source_pba_end);
    seq_printf(seq, "last_segment_output_metadata_commit_output_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_metadata_commit_output_pba_start);
    seq_printf(seq, "last_segment_output_metadata_commit_output_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_metadata_commit_output_pba_end);
    seq_printf(seq, "segment_output_physical_copy_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_physical_copy_count);
    seq_printf(seq, "segment_output_physical_copy_entry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_physical_copy_entry_count);
    seq_printf(seq, "segment_output_physical_copy_failed_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_output_physical_copy_failed_count);
    seq_printf(seq, "last_segment_output_physical_copy_segments: %u\n",
               imr_lsm_meta.stats.last_segment_output_physical_copy_segments);
    seq_printf(seq, "last_segment_output_physical_copy_entries: %u\n",
               imr_lsm_meta.stats.last_segment_output_physical_copy_entries);
    seq_printf(seq, "last_segment_output_physical_copy_failed: %u\n",
               imr_lsm_meta.stats.last_segment_output_physical_copy_failed);
    if(imr_lsm_meta.stats.last_segment_output_physical_copy_segment_id !=
       IMR_LSM_SEGMENT_NONE){
        seq_printf(seq, "last_segment_output_physical_copy_segment_id: %u\n",
                   imr_lsm_meta.stats.last_segment_output_physical_copy_segment_id);
    }else{
        seq_puts(seq, "last_segment_output_physical_copy_segment_id: none\n");
    }
    seq_printf(seq, "last_segment_output_physical_copy_error: %d\n",
               imr_lsm_meta.stats.last_segment_output_physical_copy_error);
    seq_printf(seq, "last_segment_output_physical_copy_source_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_physical_copy_source_pba_start);
    seq_printf(seq, "last_segment_output_physical_copy_source_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_physical_copy_source_pba_end);
    seq_printf(seq, "last_segment_output_physical_copy_output_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_physical_copy_output_pba_start);
    seq_printf(seq, "last_segment_output_physical_copy_output_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_segment_output_physical_copy_output_pba_end);
    seq_printf(seq, "zone_gc_min_invalid_ratio_permille: %u\n",
               imr_lsm_zone_gc_min_invalid_ratio_permille);
    seq_printf(seq, "zone_gc_free_low_watermark: %u\n",
               imr_lsm_zone_gc_free_low_watermark);
    seq_printf(seq, "zone_gc_free_zone_count: %u\n",
               imr_lsm_meta.stats.zone_gc_free_zone_count);
    seq_printf(seq, "zone_gc_pressure: %u\n",
               imr_lsm_meta.stats.zone_gc_pressure);
    seq_printf(seq, "zone_compaction_candidate_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_candidate_count);
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "zone_compaction_candidate_zone: %u\n",
                   imr_lsm_meta.stats.zone_compaction_candidate_zone);
    }else{
        seq_puts(seq, "zone_compaction_candidate_zone: none\n");
    }
    if(imr_lsm_meta.stats.zone_compaction_candidate_dest_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "zone_compaction_candidate_dest_zone: %u\n",
                   imr_lsm_meta.stats.zone_compaction_candidate_dest_zone);
    }else{
        seq_puts(seq, "zone_compaction_candidate_dest_zone: none\n");
    }
    seq_printf(seq, "zone_compaction_candidate_map_size: %u\n",
               imr_lsm_meta.stats.zone_compaction_candidate_map_size);
    seq_printf(seq, "zone_compaction_candidate_reclaimable: %u\n",
               imr_lsm_meta.stats.zone_compaction_candidate_reclaimable);
    seq_printf(seq, "zone_compaction_candidate_ratio_permille: %u\n",
               imr_lsm_meta.stats.zone_compaction_candidate_ratio_permille);
    seq_printf(seq, "zone_compaction_candidate_pressure: %u\n",
               imr_lsm_meta.stats.zone_compaction_candidate_pressure);
    seq_printf(seq, "zone_compaction_candidate_ready: %u\n",
               imr_lsm_meta.stats.zone_compaction_candidate_ready);
    if(imr_lsm_meta.stats.last_zone_compaction_candidate_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_candidate_zone: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_candidate_zone);
    }else{
        seq_puts(seq, "last_zone_compaction_candidate_zone: none\n");
    }
    if(imr_lsm_meta.stats.last_zone_compaction_candidate_dest_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_candidate_dest_zone: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_candidate_dest_zone);
    }else{
        seq_puts(seq, "last_zone_compaction_candidate_dest_zone: none\n");
    }
    seq_printf(seq, "last_zone_compaction_candidate_map_size: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_map_size);
    seq_printf(seq, "last_zone_compaction_candidate_reclaimable: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_reclaimable);
    seq_printf(seq, "last_zone_compaction_candidate_ratio_permille: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_ratio_permille);
    seq_printf(seq, "last_zone_compaction_candidate_pressure: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_pressure);
    seq_printf(seq, "last_zone_compaction_candidate_ready: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_ready);
    seq_printf(seq, "zone_compaction_auto_run_enabled: %u\n",
               imr_lsm_meta.zone_compaction_auto_run ? 1 : 0);
    seq_printf(seq, "zone_compaction_auto_running: %u\n",
               imr_lsm_meta.zone_compaction_auto_running ? 1 : 0);
    seq_printf(seq, "zone_compaction_auto_pending: %u\n",
               imr_lsm_meta.zone_compaction_auto_pending ? 1 : 0);
    if(imr_lsm_meta.zone_compaction_auto_pending_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "zone_compaction_auto_pending_zone: %u\n",
                   imr_lsm_meta.zone_compaction_auto_pending_zone);
    }else{
        seq_puts(seq, "zone_compaction_auto_pending_zone: none\n");
    }
    seq_printf(seq, "zone_compaction_auto_pending_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_auto_pending_count);
    if(imr_lsm_meta.stats.last_zone_compaction_auto_pending_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_auto_pending_zone: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_auto_pending_zone);
    }else{
        seq_puts(seq, "last_zone_compaction_auto_pending_zone: none\n");
    }
    seq_printf(seq, "zone_compaction_auto_run_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_auto_run_count);
    seq_printf(seq, "zone_compaction_auto_busy_retry_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_auto_busy_retry_count);
    seq_printf(seq, "zone_compaction_auto_run_failed_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_auto_run_failed_count);
    if(imr_lsm_meta.stats.last_zone_compaction_auto_run_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_auto_run_zone: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_auto_run_zone);
    }else{
        seq_puts(seq, "last_zone_compaction_auto_run_zone: none\n");
    }
    seq_printf(seq, "last_zone_compaction_auto_run_error: %d\n",
               imr_lsm_meta.stats.last_zone_compaction_auto_run_error);
    seq_printf(seq, "zone_compaction_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_count);
    seq_printf(seq, "zone_compaction_failed_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_failed_count);
    seq_printf(seq, "zone_compaction_input_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_input_entries_total);
    seq_printf(seq, "zone_compaction_live_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_live_entries_total);
    seq_printf(seq, "zone_compaction_skipped_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_skipped_entries_total);
    seq_printf(seq, "zone_compaction_copied_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_copied_entries_total);
    seq_printf(seq, "zone_compaction_committed_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_committed_entries_total);
    seq_printf(seq, "zone_compaction_failed_entries_total: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_failed_entries_total);
    if(imr_lsm_meta.stats.last_zone_compaction_source_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_source_zone: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_source_zone);
    }else{
        seq_puts(seq, "last_zone_compaction_source_zone: none\n");
    }
    if(imr_lsm_meta.stats.last_zone_compaction_dest_zone0 !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_dest_zone0: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_dest_zone0);
    }else{
        seq_puts(seq, "last_zone_compaction_dest_zone0: none\n");
    }
    if(imr_lsm_meta.stats.last_zone_compaction_dest_zone1 !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        seq_printf(seq, "last_zone_compaction_dest_zone1: %u\n",
                   imr_lsm_meta.stats.last_zone_compaction_dest_zone1);
    }else{
        seq_puts(seq, "last_zone_compaction_dest_zone1: none\n");
    }
    seq_printf(seq, "last_zone_compaction_input_entries: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_input_entries);
    seq_printf(seq, "last_zone_compaction_live_entries: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_live_entries);
    seq_printf(seq, "last_zone_compaction_skipped_entries: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_skipped_entries);
    seq_printf(seq, "last_zone_compaction_copied_entries: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_copied_entries);
    seq_printf(seq, "last_zone_compaction_failed_entries: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_failed_entries);
    seq_printf(seq, "last_zone_compaction_error: %d\n",
               imr_lsm_meta.stats.last_zone_compaction_error);
    seq_printf(seq, "last_zone_compaction_output_pba_start: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_zone_compaction_output_pba_start);
    seq_printf(seq, "last_zone_compaction_output_pba_end: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.last_zone_compaction_output_pba_end);
    seq_printf(seq, "tree_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.tree_hit_count);
    seq_printf(seq, "unsorted_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.unsorted_hit_count);
    seq_printf(seq, "segment_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.segment_hit_count);
    seq_printf(seq, "sorted_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.sorted_hit_count);
    seq_printf(seq, "tombstone_hit_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.tombstone_hit_count);
    seq_printf(seq, "fallback_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.fallback_count);
    seq_printf(seq, "compaction_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.compaction_count);
    seq_printf(seq, "level_compaction_pending: %u\n",
               imr_lsm_meta.level_compaction_pending ? 1 : 0);
    seq_printf(seq, "level_compaction_running: %u\n",
               imr_lsm_meta.level_compaction_running ? 1 : 0);
    seq_printf(seq, "level_compaction_work_schedule_count: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.level_compaction_work_schedule_count);
    seq_printf(seq, "level_compaction_work_run_count: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.level_compaction_work_run_count);
    seq_printf(seq, "level_compaction_work_round_count: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.level_compaction_work_round_count);
    seq_printf(seq, "level_compaction_work_requeue_count: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.level_compaction_work_requeue_count);
    seq_printf(seq, "level_compaction_work_error_count: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.level_compaction_work_error_count);
    seq_printf(seq, "last_level_compaction_work_error: %d\n",
               imr_lsm_meta.stats.last_level_compaction_work_error);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_coalesced_schedule_count",
        &imrsim_diag.level_compaction_coalesced_schedule_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_no_work_run_count",
        &imrsim_diag.level_compaction_no_work_run_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_score_max",
        &imrsim_diag.level_compaction_score_max);
    imrsim_diag_seq_atomic64(seq,
        "last_level_compaction_evaluated_score",
        &imrsim_diag.last_level_compaction_evaluated_score);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_schedule_score",
        &imrsim_diag.last_level_compaction_schedule_score);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_requeue_score",
        &imrsim_diag.last_level_compaction_requeue_score);
    seq_printf(seq, "metadata_compaction_input_bytes: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.metadata_compaction_input_bytes);
    seq_printf(seq, "metadata_compaction_output_bytes: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.metadata_compaction_output_bytes);
    seq_printf(seq, "last_compaction_input_bytes: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.last_compaction_input_bytes);
    seq_printf(seq, "last_compaction_output_bytes: %llu\n",
               (unsigned long long)
               imr_lsm_meta.stats.last_compaction_output_bytes);
    seq_printf(seq, "last_compaction_from: L%u\n",
               imr_lsm_meta.stats.last_compaction_from);
    seq_printf(seq, "last_compaction_to: L%u\n",
               imr_lsm_meta.stats.last_compaction_to);
    seq_printf(seq, "last_compaction_input: %u\n",
               imr_lsm_meta.stats.last_compaction_input);
    seq_printf(seq, "last_compaction_output_total: %u\n",
               imr_lsm_meta.stats.last_compaction_output_total);
    imrsim_diag_seq_atomic64(seq, "compaction_total_ns",
        &imrsim_diag.level_compaction_total_ns);
    imrsim_diag_seq_atomic64(seq, "compaction_max_ns",
        &imrsim_diag.level_compaction_max_ns);
    imrsim_diag_seq_atomic64(seq, "compaction_queue_depth",
        &imrsim_diag.level_compaction_queue_depth);
    imrsim_diag_seq_atomic64(seq, "compaction_queue_depth_max",
        &imrsim_diag.level_compaction_queue_depth_max);
    imrsim_diag_seq_atomic64(seq, "level_compaction_queue_wait_count",
        &imrsim_diag.level_compaction_queue_wait_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_queue_wait_total_ns",
        &imrsim_diag.level_compaction_queue_wait_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_queue_wait_max_ns",
        &imrsim_diag.level_compaction_queue_wait_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_queue_wait_ns",
        &imrsim_diag.last_level_compaction_queue_wait_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_work_time_count",
        &imrsim_diag.level_compaction_work_time_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_work_total_ns",
        &imrsim_diag.level_compaction_work_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_work_max_ns",
        &imrsim_diag.level_compaction_work_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_work_ns",
        &imrsim_diag.last_level_compaction_work_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_prepare_time_count",
        &imrsim_diag.level_compaction_prepare_time_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_prepare_total_ns",
        &imrsim_diag.level_compaction_prepare_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_prepare_max_ns",
        &imrsim_diag.level_compaction_prepare_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_prepare_ns",
        &imrsim_diag.last_level_compaction_prepare_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_build_time_count",
        &imrsim_diag.level_compaction_build_time_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_build_total_ns",
        &imrsim_diag.level_compaction_build_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_build_max_ns",
        &imrsim_diag.level_compaction_build_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_build_ns",
        &imrsim_diag.last_level_compaction_build_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_publish_time_count",
        &imrsim_diag.level_compaction_publish_time_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_publish_total_ns",
        &imrsim_diag.level_compaction_publish_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_publish_max_ns",
        &imrsim_diag.level_compaction_publish_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_publish_ns",
        &imrsim_diag.last_level_compaction_publish_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_publish_conflict_count",
        &imrsim_diag.level_compaction_publish_conflict_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_time_count",
        &imrsim_diag.level_compaction_time_count);
    imrsim_diag_seq_atomic64(seq, "level_compaction_total_ns",
        &imrsim_diag.level_compaction_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_max_ns",
        &imrsim_diag.level_compaction_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_ns",
        &imrsim_diag.last_level_compaction_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_input_entries_total",
        &imrsim_diag.level_compaction_input_entries_total);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_input_entries_max",
        &imrsim_diag.level_compaction_input_entries_max);
    imrsim_diag_seq_atomic64(seq,
        "last_level_compaction_input_entries",
        &imrsim_diag.last_level_compaction_input_entries);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_output_entries_total",
        &imrsim_diag.level_compaction_output_entries_total);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_output_entries_max",
        &imrsim_diag.level_compaction_output_entries_max);
    imrsim_diag_seq_atomic64(seq,
        "last_level_compaction_output_entries",
        &imrsim_diag.last_level_compaction_output_entries);
    for(level = 0; level < IMR_LSM_LEVELS; level++){
        seq_printf(seq, "level%u_compaction_time_count: %llu\n", level,
                   (unsigned long long)atomic64_read(
                       &imrsim_diag.level_compaction_time_count_by_level[
                           level]));
        seq_printf(seq, "level%u_compaction_total_ns: %llu\n", level,
                   (unsigned long long)atomic64_read(
                       &imrsim_diag.level_compaction_total_ns_by_level[
                           level]));
        seq_printf(seq, "level%u_compaction_max_ns: %llu\n", level,
                   (unsigned long long)atomic64_read(
                       &imrsim_diag.level_compaction_max_ns_by_level[
                           level]));
        seq_printf(seq,
                   "level%u_compaction_input_entries_total: %llu\n",
                   level, (unsigned long long)atomic64_read(
                       &imrsim_diag
                            .level_compaction_input_entries_total_by_level[
                                level]));
        seq_printf(seq,
                   "level%u_compaction_input_entries_max: %llu\n",
                   level, (unsigned long long)atomic64_read(
                       &imrsim_diag
                            .level_compaction_input_entries_max_by_level[
                                level]));
        seq_printf(seq,
                   "level%u_compaction_output_entries_total: %llu\n",
                   level, (unsigned long long)atomic64_read(
                       &imrsim_diag
                            .level_compaction_output_entries_total_by_level[
                                level]));
        seq_printf(seq,
                   "level%u_compaction_output_entries_max: %llu\n",
                   level, (unsigned long long)atomic64_read(
                       &imrsim_diag
                            .level_compaction_output_entries_max_by_level[
                                level]));
    }
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_wait_count",
        &imrsim_diag.level_compaction_zone_lock_wait_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_wait_total_ns",
        &imrsim_diag.level_compaction_zone_lock_wait_total_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_wait_max_ns",
        &imrsim_diag.level_compaction_zone_lock_wait_max_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_wait_count",
        &imrsim_diag.level_compaction_lsm_lock_wait_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_wait_total_ns",
        &imrsim_diag.level_compaction_lsm_lock_wait_total_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_wait_max_ns",
        &imrsim_diag.level_compaction_lsm_lock_wait_max_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_hold_count",
        &imrsim_diag.level_compaction_zone_lock_hold_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_hold_total_ns",
        &imrsim_diag.level_compaction_zone_lock_hold_total_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_zone_lock_hold_max_ns",
        &imrsim_diag.level_compaction_zone_lock_hold_max_ns);
    imrsim_diag_seq_atomic64(seq,
        "last_level_compaction_zone_lock_hold_ns",
        &imrsim_diag.last_level_compaction_zone_lock_hold_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_hold_count",
        &imrsim_diag.level_compaction_lsm_lock_hold_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_hold_total_ns",
        &imrsim_diag.level_compaction_lsm_lock_hold_total_ns);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_lsm_lock_hold_max_ns",
        &imrsim_diag.level_compaction_lsm_lock_hold_max_ns);
    imrsim_diag_seq_atomic64(seq,
        "last_level_compaction_lsm_lock_hold_ns",
        &imrsim_diag.last_level_compaction_lsm_lock_hold_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_post_round_count",
        &imrsim_diag.level_compaction_post_round_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_post_round_total_ns",
        &imrsim_diag.level_compaction_post_round_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_post_round_max_ns",
        &imrsim_diag.level_compaction_post_round_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_post_round_ns",
        &imrsim_diag.last_level_compaction_post_round_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_post_recalc_count",
        &imrsim_diag.level_compaction_post_recalc_count);
    imrsim_diag_seq_atomic64(seq,
        "level_compaction_post_recalc_total_ns",
        &imrsim_diag.level_compaction_post_recalc_total_ns);
    imrsim_diag_seq_atomic64(seq, "level_compaction_post_recalc_max_ns",
        &imrsim_diag.level_compaction_post_recalc_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_level_compaction_post_recalc_ns",
        &imrsim_diag.last_level_compaction_post_recalc_ns);
    imrsim_diag_seq_atomic64(seq, "foreground_zone_lock_wait_count",
        &imrsim_diag.foreground_zone_lock_wait_count);
    imrsim_diag_seq_atomic64(seq, "foreground_zone_lock_wait_total_ns",
        &imrsim_diag.foreground_zone_lock_wait_total_ns);
    imrsim_diag_seq_atomic64(seq, "foreground_zone_lock_wait_max_ns",
        &imrsim_diag.foreground_zone_lock_wait_max_ns);
    imrsim_diag_seq_atomic64(seq, "foreground_lsm_lock_wait_count",
        &imrsim_diag.foreground_lsm_lock_wait_count);
    imrsim_diag_seq_atomic64(seq, "foreground_lsm_lock_wait_total_ns",
        &imrsim_diag.foreground_lsm_lock_wait_total_ns);
    imrsim_diag_seq_atomic64(seq, "foreground_lsm_lock_wait_max_ns",
        &imrsim_diag.foreground_lsm_lock_wait_max_ns);
    seq_printf(seq, "imr_lsm_lock_wait_count: %llu\n",
               (unsigned long long)(atomic64_read(
                   &imrsim_diag.foreground_lsm_lock_wait_count) +
                   atomic64_read(
                   &imrsim_diag.level_compaction_lsm_lock_wait_count)));
    seq_printf(seq, "imr_lsm_lock_wait_total_ns: %llu\n",
               (unsigned long long)(atomic64_read(
                   &imrsim_diag.foreground_lsm_lock_wait_total_ns) +
                   atomic64_read(
                   &imrsim_diag.level_compaction_lsm_lock_wait_total_ns)));
    seq_printf(seq, "imr_lsm_lock_wait_max_ns: %llu\n",
               (unsigned long long)max(atomic64_read(
                   &imrsim_diag.foreground_lsm_lock_wait_max_ns),
                   atomic64_read(
                   &imrsim_diag.level_compaction_lsm_lock_wait_max_ns)));
    imrsim_diag_seq_atomic64(seq, "flush_bio_count",
        &imrsim_diag.flush_bio_count);
    imrsim_diag_seq_atomic64(seq, "incoming_fua_write_count",
        &imrsim_diag.incoming_fua_write_count);
    imrsim_diag_seq_atomic64(seq, "fua_write_count",
        &imrsim_diag.fua_write_count);
    imrsim_diag_seq_atomic64(seq, "internal_flush_fua_write_count",
        &imrsim_diag.internal_flush_fua_write_count);
    imrsim_diag_seq_atomic64(seq, "internal_flush_fua_write_total_ns",
        &imrsim_diag.internal_flush_fua_write_total_ns);
    imrsim_diag_seq_atomic64(seq, "internal_flush_fua_write_max_ns",
        &imrsim_diag.internal_flush_fua_write_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_internal_flush_fua_write_ns",
        &imrsim_diag.last_internal_flush_fua_write_ns);
    imrsim_diag_seq_atomic64(seq, "internal_rmw_fua_write_count",
        &imrsim_diag.internal_rmw_fua_write_count);
    imrsim_diag_seq_atomic64(seq, "partial_io_count",
        &imrsim_diag.partial_io_count);
    imrsim_diag_seq_atomic64(seq, "partial_read_count",
        &imrsim_diag.partial_read_count);
    imrsim_diag_seq_atomic64(seq, "partial_rmw_count",
        &imrsim_diag.partial_rmw_count);
    imrsim_diag_seq_atomic64(seq, "partial_io_queue_wait_count",
        &imrsim_diag.partial_io_queue_wait_count);
    imrsim_diag_seq_atomic64(seq, "partial_io_queue_wait_total_ns",
        &imrsim_diag.partial_io_queue_wait_total_ns);
    imrsim_diag_seq_atomic64(seq, "partial_io_queue_wait_max_ns",
        &imrsim_diag.partial_io_queue_wait_max_ns);
    imrsim_diag_seq_atomic64(seq, "partial_io_total_ns",
        &imrsim_diag.partial_io_total_ns);
    imrsim_diag_seq_atomic64(seq, "partial_io_max_ns",
        &imrsim_diag.partial_io_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_partial_io_ns",
        &imrsim_diag.last_partial_io_ns);
    imrsim_diag_seq_atomic64(seq, "partial_rmw_total_ns",
        &imrsim_diag.partial_rmw_total_ns);
    imrsim_diag_seq_atomic64(seq, "partial_rmw_max_ns",
        &imrsim_diag.partial_rmw_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_partial_rmw_ns",
        &imrsim_diag.last_partial_rmw_ns);
    imrsim_diag_seq_atomic64(seq, "legacy_rmw_count",
        &imrsim_diag.legacy_rmw_count);
    imrsim_diag_seq_atomic64(seq, "legacy_rmw_total_ns",
        &imrsim_diag.legacy_rmw_total_ns);
    imrsim_diag_seq_atomic64(seq, "legacy_rmw_max_ns",
        &imrsim_diag.legacy_rmw_max_ns);
    imrsim_diag_seq_atomic64(seq, "last_legacy_rmw_ns",
        &imrsim_diag.last_legacy_rmw_ns);
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, imr_lsm_debugfs_stats_show, inode->i_private);
}

static const struct file_operations imr_lsm_debugfs_stats_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_stats_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static ssize_t imr_lsm_debugfs_delete_key_write(struct file *file,
                                                const char __user *ubuf,
                                                size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u64 key;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtoull(buf, 0, &key);
    if(ret){
        return ret;
    }

    ret = imrsim_lsm_delete_key(key);
    if(ret < 0){
        return ret;
    }

    return count;
}

static const struct file_operations imr_lsm_debugfs_delete_key_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_delete_key_write,
    .llseek = no_llseek,
};

static ssize_t imr_lsm_debugfs_compact_write(struct file *file,
                                             const char __user *ubuf,
                                             size_t count, loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 level;
    __u32 compacted_level;
    __u32 input_entries;
    __u32 output_entries;
    bool did_work;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &level);
    if(ret){
        return ret;
    }
    if(level >= IMR_LSM_LEVELS){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    ret = imr_lsm_execute_level_compaction(
        false, level, &did_work, &compacted_level,
        &input_entries, &output_entries);
    mutex_unlock(&imr_lsm_compaction_lock);
    if(ret){
        return ret;
    }

    printk(KERN_INFO "imrsim: IMR-LSM manual compact L%u\n", level);
    return count;
}

static const struct file_operations imr_lsm_debugfs_compact_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_compact_write,
    .llseek = no_llseek,
};

static ssize_t imr_lsm_debugfs_compact_segment_write(struct file *file,
                                                     const char __user *ubuf,
                                                     size_t count,
                                                     loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 run;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &run);
    if(ret){
        return ret;
    }
    if(!run){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_compact_selected_segment_locked();
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    if(ret){
        return ret;
    }

    printk(KERN_INFO "imrsim: IMR-LSM manual selected segment compact\n");
    return count;
}

static const struct file_operations imr_lsm_debugfs_compact_segment_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_compact_segment_write,
    .llseek = no_llseek,
};

static ssize_t imr_lsm_debugfs_compact_zone_write(struct file *file,
                                                  const char __user *ubuf,
                                                  size_t count,
                                                  loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 zone_idx;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &zone_idx);
    if(ret){
        return ret;
    }

    ret = imr_lsm_compact_zone(zone_idx);
    if(ret){
        return ret;
    }

    printk(KERN_INFO "imrsim: IMR-LSM manual zone compact zone=%u\n",
           zone_idx);
    return count;
}

static const struct file_operations imr_lsm_debugfs_compact_zone_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_compact_zone_write,
    .llseek = no_llseek,
};

static ssize_t imr_lsm_debugfs_seed_full_zone_write(struct file *file,
                                                    const char __user *ubuf,
                                                    size_t count,
                                                    loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 zone_idx;
    bool queue_auto_run = false;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &zone_idx);
    if(ret){
        return ret;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_seed_full_zone_locked(zone_idx);
    if(!ret){
        queue_auto_run =
            imr_lsm_defer_zone_compaction_auto_run_locked(zone_idx);
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    if(ret){
        return ret;
    }
    if(queue_auto_run){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    printk(KERN_INFO "imrsim: IMR-LSM manual seed full zone=%u\n",
           zone_idx);
    return count;
}

static const struct file_operations imr_lsm_debugfs_seed_full_zone_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_seed_full_zone_write,
    .llseek = no_llseek,
};

enum imr_lsm_zone_gc_policy_knob {
    IMR_LSM_ZONE_GC_POLICY_MIN_INVALID_RATIO = 0,
    IMR_LSM_ZONE_GC_POLICY_FREE_LOW_WATERMARK,
};

static ssize_t imr_lsm_debugfs_zone_gc_policy_write(
    const char __user *ubuf, size_t count,
    enum imr_lsm_zone_gc_policy_knob knob)
{
    char buf[32];
    size_t len;
    __u32 value;
    __u32 candidate_zone;
    bool queue_auto_run = false;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &value);
    if(ret){
        return ret;
    }
    if((knob == IMR_LSM_ZONE_GC_POLICY_MIN_INVALID_RATIO &&
        value > IMR_LSM_ZONE_GC_MIN_INVALID_RATIO_PERMILLE_MAX) ||
       (knob == IMR_LSM_ZONE_GC_POLICY_FREE_LOW_WATERMARK &&
        value > IMR_LSM_ZONE_GC_FREE_LOW_WATERMARK_MAX)){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    if(knob == IMR_LSM_ZONE_GC_POLICY_MIN_INVALID_RATIO){
        imr_lsm_zone_gc_min_invalid_ratio_permille = value;
    }else{
        imr_lsm_zone_gc_free_low_watermark = value;
    }
    imr_lsm_record_zone_compaction_candidate_locked(0);
    candidate_zone =
        imr_lsm_meta.stats.zone_compaction_candidate_zone;
    if(candidate_zone != IMR_LSM_ZONE_COMPACTION_NONE){
        queue_auto_run =
            imr_lsm_defer_zone_compaction_auto_run_locked(candidate_zone);
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);

    if(queue_auto_run){
        imr_lsm_queue_zone_compaction_auto_work();
    }
    if(knob == IMR_LSM_ZONE_GC_POLICY_MIN_INVALID_RATIO){
        printk(KERN_INFO "imrsim: IMR-LSM zone GC min invalid ratio permille=%u\n",
               value);
    }else{
        printk(KERN_INFO "imrsim: IMR-LSM zone GC free low watermark=%u\n",
               value);
    }
    return count;
}

static int imr_lsm_debugfs_zone_gc_min_invalid_ratio_show(
    struct seq_file *seq, void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n",
               imr_lsm_zone_gc_min_invalid_ratio_permille);
    mutex_unlock(&imr_lsm_lock);
    return 0;
}

static int imr_lsm_debugfs_zone_gc_min_invalid_ratio_open(
    struct inode *inode, struct file *file)
{
    return single_open(
        file, imr_lsm_debugfs_zone_gc_min_invalid_ratio_show,
        inode->i_private);
}

static ssize_t imr_lsm_debugfs_zone_gc_min_invalid_ratio_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    return imr_lsm_debugfs_zone_gc_policy_write(
        ubuf, count, IMR_LSM_ZONE_GC_POLICY_MIN_INVALID_RATIO);
}

static const struct file_operations
imr_lsm_debugfs_zone_gc_min_invalid_ratio_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_zone_gc_min_invalid_ratio_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_zone_gc_min_invalid_ratio_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_zone_gc_free_low_watermark_show(
    struct seq_file *seq, void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n", imr_lsm_zone_gc_free_low_watermark);
    mutex_unlock(&imr_lsm_lock);
    return 0;
}

static int imr_lsm_debugfs_zone_gc_free_low_watermark_open(
    struct inode *inode, struct file *file)
{
    return single_open(
        file, imr_lsm_debugfs_zone_gc_free_low_watermark_show,
        inode->i_private);
}

static ssize_t imr_lsm_debugfs_zone_gc_free_low_watermark_write(
    struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    return imr_lsm_debugfs_zone_gc_policy_write(
        ubuf, count, IMR_LSM_ZONE_GC_POLICY_FREE_LOW_WATERMARK);
}

static const struct file_operations
imr_lsm_debugfs_zone_gc_free_low_watermark_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_zone_gc_free_low_watermark_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_zone_gc_free_low_watermark_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static int imr_lsm_debugfs_zone_compaction_auto_run_show(struct seq_file *seq,
                                                         void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "%u\n", imr_lsm_meta.zone_compaction_auto_run ? 1 : 0);
    mutex_unlock(&imr_lsm_lock);

    return 0;
}

static int imr_lsm_debugfs_zone_compaction_auto_run_open(struct inode *inode,
                                                         struct file *file)
{
    return single_open(file, imr_lsm_debugfs_zone_compaction_auto_run_show,
                       inode->i_private);
}

static ssize_t imr_lsm_debugfs_zone_compaction_auto_run_write(struct file *file,
                                                              const char __user *ubuf,
                                                              size_t count,
                                                              loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 enabled;
    __u32 candidate_zone;
    bool queue_auto_run = false;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &enabled);
    if(ret){
        return ret;
    }
    if(enabled > 1){
        return -EINVAL;
    }

    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    imr_lsm_meta.zone_compaction_auto_run = enabled ? 1 : 0;
    if(!enabled){
        imr_lsm_clear_zone_compaction_auto_pending_locked(
            IMR_LSM_ZONE_COMPACTION_NONE);
    }else{
        imr_lsm_record_zone_compaction_candidate_locked(0);
        if(imr_lsm_meta.stats.zone_compaction_candidate_zone !=
           IMR_LSM_ZONE_COMPACTION_NONE){
            candidate_zone =
                imr_lsm_meta.stats.zone_compaction_candidate_zone;
            queue_auto_run =
                imr_lsm_defer_zone_compaction_auto_run_locked(
                    candidate_zone);
        }
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    if(queue_auto_run){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    printk(KERN_INFO "imrsim: IMR-LSM zone compaction auto_run=%u\n",
           enabled ? 1 : 0);
    return count;
}

static const struct file_operations imr_lsm_debugfs_zone_compaction_auto_run_fops = {
    .owner = THIS_MODULE,
    .open = imr_lsm_debugfs_zone_compaction_auto_run_open,
    .read = seq_read,
    .write = imr_lsm_debugfs_zone_compaction_auto_run_write,
    .llseek = seq_lseek,
    .release = single_release,
};

static ssize_t imr_lsm_debugfs_commit_output_write(struct file *file,
                                                   const char __user *ubuf,
                                                   size_t count,
                                                   loff_t *ppos)
{
    char buf[32];
    size_t len;
    __u32 run;
    int ret;

    if(count >= sizeof(buf)){
        return -E2BIG;
    }
    len = count;
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &run);
    if(ret){
        return ret;
    }
    if(!run){
        return -EINVAL;
    }
    if(run > 3){
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_commit_output_locked(run);
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    if(ret){
        return ret;
    }

    printk(KERN_INFO "imrsim: IMR-LSM output commit mode=%u\n", run);
    return count;
}

static const struct file_operations imr_lsm_debugfs_commit_output_fops = {
    .owner = THIS_MODULE,
    .write = imr_lsm_debugfs_commit_output_write,
    .llseek = no_llseek,
};

static void imr_lsm_debugfs_init(void)
{
    imr_lsm_debugfs_dir = debugfs_create_dir("imrsim_lsm", NULL);
    if(IS_ERR_OR_NULL(imr_lsm_debugfs_dir)){
        printk(KERN_ERR "imrsim: IMR-LSM debugfs dir create failed\n");
        imr_lsm_debugfs_dir = NULL;
        return;
    }

    debugfs_create_file("unsorted", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_unsorted_fops);
    debugfs_create_file("sorted", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_sorted_fops);
    debugfs_create_file("segments", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_segments_fops);
    debugfs_create_file("placement", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_placement_fops);
    debugfs_create_file("obsolete", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_obsolete_fops);
    debugfs_create_file("compaction_policy", 0444,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_compaction_policy_fops);
    debugfs_create_file("block_table", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_block_table_fops);
    debugfs_create_file("read_tree", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_read_tree_fops);
    debugfs_create_file("clear_read_tree", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_clear_read_tree_fops);
    debugfs_create_file("read_tree_limit", 0600, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_read_tree_limit_fops);
    /*
     * VM/debug validation overrides only.  These files are deliberately not
     * a stable production policy interface and are reset with the dm target.
     */
    debugfs_create_file("compaction_threshold", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_compaction_threshold_fops);
    debugfs_create_file("max_bytes_for_level_base", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_max_bytes_for_level_base_fops);
    debugfs_create_file("max_bytes_for_level_multiplier", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_max_bytes_for_level_multiplier_fops);
    debugfs_create_file("bloom_bits_per_key", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_bloom_bits_per_key_fops);
    debugfs_create_file("level_compaction_build_delay_ms", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_level_compaction_build_delay_ms_fops);
    debugfs_create_file("stats", 0444, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_stats_fops);
    debugfs_create_file("delete_key", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_delete_key_fops);
    debugfs_create_file("compact", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_compact_fops);
    debugfs_create_file("compact_segment", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_compact_segment_fops);
    debugfs_create_file("compact_zone", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_compact_zone_fops);
    debugfs_create_file("zone_gc_min_invalid_ratio_permille", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_zone_gc_min_invalid_ratio_fops);
    debugfs_create_file("zone_gc_free_low_watermark", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_zone_gc_free_low_watermark_fops);
    debugfs_create_file("zone_compaction_auto_run", 0600,
                        imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_zone_compaction_auto_run_fops);
    /* VM/debug validation only; not a production compaction data path. */
    debugfs_create_file("seed_full_zone", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_seed_full_zone_fops);
    debugfs_create_file("commit_output", 0200, imr_lsm_debugfs_dir, NULL,
                        &imr_lsm_debugfs_commit_output_fops);
}

static void imr_lsm_debugfs_exit(void)
{
    debugfs_remove_recursive(imr_lsm_debugfs_dir);
    imr_lsm_debugfs_dir = NULL;
}

/* Basic information for initializing zone. */
static void imrsim_init_zone_default(__u64 sizedev)   /* sizedev: in sectors */
{
    IMR_CAPACITY = sizedev;
    IMR_ZONE_SIZE_SHIFT = IMR_ZONE_SIZE_SHIFT_DEFAULT;
    IMR_BLOCK_SIZE_SHIFT = IMR_BLOCK_SIZE_SHIFT_DEFAULT;
    IMR_NUMZONES = (IMR_CAPACITY >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT);
    IMR_NUMZONES_DEFAULT = IMR_NUMZONES;
    printk(KERN_INFO "imrsim_init_zone_state: numzones=%d sizedev=%llu\n",
        IMR_NUMZONES, sizedev); 
}

/* Caller holds imrsim_zone_lock, or is initializing unpublished state. */
static void imrsim_reset_stats_locked(void)
{
    memset(&zone_state->stats.dev_stats.idle_stats, 0,
           sizeof(struct imrsim_idle_stats));
    memset(&zone_state->stats.extra_write_total, 0, sizeof(__u64));
    memset(&zone_state->stats.write_total, 0, sizeof(__u64));
    memset(zone_state->stats.zone_stats, 0,
           zone_state->stats.num_zones *
           sizeof(struct imrsim_zone_stats));
}

/* Basic information for initializing the device state (zone_state) */
static void imrsim_init_zone_state_default(__u32 state_size)
{
    __u32 i;
    __u32 j;
    __u32 *magic;   /* magic number to identify the device (equipment identity) */

    /* head info. */
    zone_state->header.magic = 0xBEEFBEEF;
    zone_state->header.length = state_size;
    zone_state->header.version = VERSION;
    zone_state->header.crc32 = 0;

    /* config info. */
    zone_state->config.dev_config.out_of_policy_read_flag = 0;
    zone_state->config.dev_config.out_of_policy_write_flag = 0;
    zone_state->config.dev_config.r_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    zone_state->config.dev_config.w_time_to_rmw_zone = IMR_TRANSFER_PENALTY;

    zone_state->stats.num_zones = IMR_NUMZONES;
    zone_state->stats.extra_write_total = 0;
    zone_state->stats.write_total = 0;
    imrsim_reset_stats_locked();
    /* To allocate space for the zone_status array and initialize it. */
    zone_status = imrsim_zone_status_ptr(zone_state, IMR_NUMZONES);
    imr_lsm_allocator_reset_locked();
    for(i=0; i<IMR_NUMZONES; i++){
        zone_status[i].z_start = i;
        zone_status[i].z_length = num_sectors_zone();
        zone_status[i].z_type = Z_TYPE_SEQUENTIAL;
        zone_status[i].z_conds = Z_COND_EMPTY;
        zone_status[i].z_flag = 0;
        for(j=0;j<TOP_TRACK_NUM_TOTAL;j++){
            memset(zone_status[i].z_tracks[j].isUsedBlock,0,IMR_TOP_TRACK_SIZE*sizeof(__u8));
        }
        zone_status[i].z_map_size = 0;
        zone_status[i].z_live_count = 0;
        zone_status[i].z_generation = 0;
        memset(zone_status[i].z_pba_map,-1,TOP_TRACK_NUM_TOTAL*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)*sizeof(int));
        memset(zone_status[i].z_key_map, 0xff,
               sizeof(zone_status[i].z_key_map));
    }
    printk(KERN_INFO "imrsim: %s zone_status init!\n", __FUNCTION__);
    magic = (__u32 *)&zone_status[IMR_NUMZONES];
    *magic = 0xBEEFBEEF;
}

/* To initial a device. */
int imrsim_init_zone_state(__u64 sizedev)
{
    __u32 state_size;
    int ret;

    if(!sizedev){
        printk(KERN_ERR "imrsim: zero capacity detected\n");
        return -EINVAL;
    }
    imrsim_init_zone_default(sizedev);     /* Initialize the basic information of the zone. */
    ret = imrsim_state_size(&state_size);
    if(ret){
        printk(KERN_ERR "imrsim: zone state size overflow: %d\n", ret);
        return ret;
    }
    /* zone_state should not have allocated space, if it already exists, reclaim the space. */
    if(zone_state){
        vfree(zone_state);
        zone_state = NULL;
        zone_status = NULL;
    }
    zone_state = vzalloc((unsigned long)
                         imrsim_state_persistence_bytes(state_size));
    if(!zone_state){
        printk(KERN_ERR "imrsim: memory alloc failed for zone state\n");
        return -ENOMEM;
    }
    imrsim_init_zone_state_default(state_size);   
    imrsim_dev_idle_init();      
    imr_lsm_init_metadata();
    return 0;
}

struct imrsim_bio_wait_context
{
    struct completion event;
    int error;
};

static void imrsim_internal_io_completion(struct bio *bio, int err)
{
    struct imrsim_bio_wait_context *ctx;

    if(err){
        printk(KERN_ERR "imrsim: internal bio err:%d\n", err);
    }
    if(bio){
        ctx = (struct imrsim_bio_wait_context *)bio->bi_private;
        if(ctx){
            ctx->error = err;
            complete(&ctx->event);
        }
    }
}

static bool imrsim_bio_is_internal_io(const struct bio *bio)
{
    return bio && bio->bi_end_io == imrsim_internal_io_completion;
}

/* Each submitted bio owns an independent stack context.  The function does
 * not return (and the context cannot go out of scope) before end_io runs. */
static int imrsim_submit_internal_page_io(struct block_device *dev,
                                          sector_t lba,
                                          unsigned int size,
                                          struct page *page, int rw)
{
    struct imrsim_bio_wait_context ctx;
    struct bio *bio;
    int ret;

    bio = bio_alloc(GFP_NOIO, 1);
    if(!bio){
        printk(KERN_ERR "imrsim: %s bio_alloc failed\n", __FUNCTION__);
        return -ENOMEM;
    }
    bio->bi_bdev = dev;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
    bio->bi_sector = lba;
    #else
    bio->bi_iter.bi_sector = lba;
    #endif
    if(bio_add_page(bio, page, size, 0) != size){
        bio_put(bio);
        return -EIO;
    }

    init_completion(&ctx.event);
    ctx.error = 0;
    bio->bi_private = &ctx;
    bio->bi_end_io = imrsim_internal_io_completion;
    submit_bio(rw, bio);
    wait_for_completion(&ctx.event);

    ret = ctx.error;
    if(!ret && !test_bit(BIO_UPTODATE, &bio->bi_flags)){
        ret = -EIO;
    }
    bio_put(bio);
    return ret;
}

/* To get device mapping offset. */
static sector_t imrsim_map_sector(struct dm_target *ti,
                                  sector_t bi_sector)
{
    struct imrsim_c *c = ti->private;
    return c->start + dm_target_offset(ti, bi_sector);
}

/* read page (for meta-data) */
static int imrsim_read_page(struct block_device *dev, sector_t lba,
                            int size, struct page *page)
{
    int ret;

    ret = imrsim_submit_internal_page_io(dev, lba, size, page,
                                         READ | REQ_SYNC);
    if(ret){
        printk(KERN_ERR "imrsim: pstore bio read failed\n");
    }
    return ret;
}

/* write page (for meta-data) */
static int imrsim_write_page(struct block_device *dev, sector_t lba,
                            __u32 size, struct page *page)
{
    int ret;
    __u64 fua_start_ns;

    atomic64_inc(&imrsim_diag.fua_write_count);
    atomic64_inc(&imrsim_diag.internal_flush_fua_write_count);
    fua_start_ns = imrsim_diag_now_ns();
    ret = imrsim_submit_internal_page_io(dev, lba, size, page,
                                         WRITE_FLUSH_FUA);
    imrsim_diag_record_duration(
        NULL, &imrsim_diag.internal_flush_fua_write_total_ns,
        &imrsim_diag.internal_flush_fua_write_max_ns,
        &imrsim_diag.last_internal_flush_fua_write_ns,
        imrsim_diag_elapsed_ns(fua_start_ns));
    if(ret){
        printk(KERN_ERR "imrsim: pstore bio write failed\n");
    }
    return ret;
}

static int imrsim_read_data_page(struct block_device *dev, sector_t lba,
                                 unsigned int size, struct page *page)
{
    int ret;

    ret = imrsim_submit_internal_page_io(dev, lba, size, page,
                                         READ | REQ_SYNC);
    if(ret){
        printk(KERN_ERR "imrsim: data RMW bio read failed\n");
    }
    return ret;
}

static int imrsim_write_data_page(struct block_device *dev, sector_t lba,
                                  unsigned int size, struct page *page)
{
    int ret;

    ret = imrsim_submit_internal_page_io(dev, lba, size, page,
                                         WRITE | REQ_SYNC);
    if(ret){
        printk(KERN_ERR "imrsim: data RMW bio write failed\n");
    }
    return ret;
}

/* rmw task - sub thread*/
int read_modify_write_task(void *arg)
{
    
    struct dm_target * ti = (struct dm_target *)arg;
    struct imrsim_c *c = ti->private;
    __u8 i;
    __u8 n = imrsim_rmw_task.lba_num;
    struct page *pages[2] = {NULL, NULL};
    void  *page_addrs[2];
    __u64 rmw_start_ns = imrsim_diag_now_ns();
    bool data_bio_submitted = false;
    int ret = 0;

    if(imrsim_rmw_task.bio)
    {
        IMRSIM_DATA_LOG("imrsim: enter rmw process and back up\n");
        // read the blocks needed to back up
        for(i=0; i<n; i++)
        {
            pages[i] = alloc_page(GFP_KERNEL);
            if(!pages[i]){
                printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
                ret = -ENOMEM;
                goto out;
            }
            page_addrs[i] = page_address(pages[i]);
            if(!page_addrs[i]){
                printk(KERN_ERR "imrsim: read page vm addr null\n");
                __free_page(pages[i]);
                pages[i] = NULL;
                ret = -ENOMEM;
                goto out;
            }
            memset(page_addrs[i], 0, PAGE_SIZE);
            ret = imrsim_submit_internal_page_io(
                c->dev->bdev,
                imrsim_map_sector(ti, imrsim_rmw_task.lba[i]),
                PAGE_SIZE, pages[i], READ | REQ_SYNC);
            if(ret){
                printk(KERN_ERR "imrsim: legacy RMW backup read failed ret=%d\n",
                       ret);
                goto out;
            }
            cond_resched();
        }

        IMRSIM_DATA_LOG("imrsim: write bio.\n");
        // write current bio
        atomic64_inc(&imrsim_diag.fua_write_count);
        atomic64_inc(&imrsim_diag.internal_rmw_fua_write_count);
        submit_bio(WRITE_FUA, imrsim_rmw_task.bio);
        data_bio_submitted = true;
        cond_resched();

        IMRSIM_DATA_LOG("imrsim: write back.\n");
        // write back
        for(i=0; i<n; i++)
        {
            atomic64_inc(&imrsim_diag.fua_write_count);
            atomic64_inc(&imrsim_diag.internal_rmw_fua_write_count);
            ret = imrsim_submit_internal_page_io(
                c->dev->bdev,
                imrsim_map_sector(ti, imrsim_rmw_task.lba[i]),
                PAGE_SIZE, pages[i], WRITE_FUA);
            if(ret){
                printk(KERN_ERR "imrsim: legacy RMW writeback failed ret=%d\n",
                       ret);
                goto out;
            }
            cond_resched();
        }

out:
        if(ret && !data_bio_submitted && imrsim_rmw_task.bio){
            imrsim_complete_bio(imrsim_rmw_task.bio, ret);
        }
        for(i=0; i<n; i++)
        {
            if(pages[i]){
                __free_page(pages[i]);
            }
        }
        IMRSIM_DATA_LOG("imrsim: release pages.\n");
        imrsim_rmw_task.lba_num = 0;
        imrsim_rmw_task.bio = NULL;
        imrsim_diag_record_duration(&imrsim_diag.legacy_rmw_count,
                                    &imrsim_diag.legacy_rmw_total_ns,
                                    &imrsim_diag.legacy_rmw_max_ns,
                                    &imrsim_diag.last_legacy_rmw_ns,
                                    imrsim_diag_elapsed_ns(rmw_start_ns));
        complete(&imrsim_rmw_event);
    }
    return ret;
}

/* RMW event caused by update to bottom track */
void imrsim_rmw_thread(struct dm_target *ti)
{
    imrsim_rmw_task.task = kthread_create(read_modify_write_task, ti, "rmw thread");
    if(!IS_ERR(imrsim_rmw_task.task)){
        IMRSIM_DATA_LOG("imrsim: rmw thread created : %d.\n",
                        imrsim_rmw_task.task->pid);
        init_completion(&imrsim_rmw_event);
        wake_up_process(imrsim_rmw_task.task);
        wait_for_completion(&imrsim_rmw_event);
        //kthread_stop(imrsim_rmw_task.task);
        IMRSIM_DATA_LOG("imrsim: rmw task end.\n");
    }else{
        int ret = PTR_ERR(imrsim_rmw_task.task);

        imrsim_rmw_task.task = NULL;
        if(imrsim_rmw_task.bio){
            imrsim_complete_bio(imrsim_rmw_task.bio, ret);
            imrsim_rmw_task.bio = NULL;
        }
        imrsim_rmw_task.lba_num = 0;
        printk(KERN_ERR "imrsim: cannot create RMW thread ret=%d\n", ret);
    }
}


static sector_t imrsim_pstore_page_offset(__u32 page_idx)
{
    return (sector_t)page_idx *
           (sector_t)(PAGE_SIZE >> IMR_SECTOR_SIZE_SHIFT_DEFAULT);
}

/* To persist meta-data. */
static int imrsim_save_persistence(struct dm_target *ti)
{
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            num_pages;
    __u32            part_page;
    __u32            idx;
    __u32            crc;
    int              ret = 0;

    zdev = ti->private;
    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: write page vm addr null\n");
        __free_pages(page, 0);
        return -EINVAL;
    }
    num_pages = div_u64_rem(zone_state->header.length, PAGE_SIZE, &part_page);
    crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header),
                zone_state->header.length - sizeof(struct imrsim_state_header));
    zone_state->header.crc32 = crc;
    for(idx = 0; idx < num_pages; idx++){
        memcpy(page_addr, ((unsigned char *)zone_state + 
               idx * PAGE_SIZE), PAGE_SIZE);
        ret = imrsim_write_page(zdev->dev->bdev,
                                imrsim_ptask.pstore_lba +
                                imrsim_pstore_page_offset(idx),
                                PAGE_SIZE, page);
        if(ret < 0){
            goto out;
        }
    }
    if(part_page){
        memset(page_addr, 0, PAGE_SIZE);
        memcpy(page_addr, ((unsigned char *)zone_state + 
              num_pages * PAGE_SIZE), part_page);
        ret = imrsim_write_page(zdev->dev->bdev,
                                imrsim_ptask.pstore_lba +
                                imrsim_pstore_page_offset(num_pages),
                                PAGE_SIZE, page);
        if(ret < 0){
            goto out;
        }
    }
    if(imrsim_dbg_log_enabled && printk_ratelimit()){
        printk(KERN_INFO "imrsim: save persist success\n");
    }
out:
    __free_pages(page, 0);
    return ret < 0 ? ret : 0;
}

/* To load metadata from persistent storage. */
static int imrsim_load_persistence(struct dm_target *ti)
{
    __u64            sizedev;
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            num_pages;
    __u32            part_page;     
    __u32            idx;
    __u32            crc;
    __u32            expected_state_size;
    __u32            persisted_num_zones;
    __u32            persisted_zone_blocks;
    int              ret;
    struct imrsim_state_header header;

    printk(KERN_INFO "imrsim: load persistence\n");

    zdev = ti->private;
    sizedev = ti->len;
    imrsim_init_zone_default(sizedev);
    /* The starting address for persistent storage. */
    /*
     * The persistence tail starts immediately after the mapped target.  Using
     * ti->len also avoids 32-bit shift wrap for targets above 8191 zones.
     */
    imrsim_ptask.pstore_lba = ti->len;
    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: read page vm addr null\n");
        __free_pages(page, 0);
        return -ENOMEM;
    }
    memset(page_addr, 0, PAGE_SIZE);
    ret = imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba,
                           PAGE_SIZE, page);
    if(ret < 0){
        __free_pages(page, 0);
        return ret;
    }
    memcpy(&header, page_addr, sizeof(struct imrsim_state_header));
    if(header.magic == 0xBEEFBEEF &&
       header.version == VERSION){
        persisted_num_zones =
            ((struct imrsim_state *)page_addr)->stats.num_zones;
        ret = imrsim_state_size_for_zones(persisted_num_zones,
                                          &expected_state_size);
        if(!persisted_num_zones || ret ||
           header.length != expected_state_size ||
           imrsim_state_persistence_bytes(header.length) >
           imrsim_persistence_capacity_bytes){
            printk(KERN_ERR "imrsim: invalid persisted zone-state dimensions\n");
            goto corrupt_state;
        }
        zone_state = vzalloc((unsigned long)
                             imrsim_state_persistence_bytes(header.length));
        if(!zone_state){
            printk(KERN_ERR "imrsim: zone_state error: no enough memory\n");
            ret = -ENOMEM;
            goto load_error;
        }
        num_pages = div_u64_rem(header.length, PAGE_SIZE, &part_page);
        if(num_pages){
            memcpy((unsigned char *)zone_state, page_addr, PAGE_SIZE);  // load header
        }
        for(idx = 1; idx < num_pages; idx++){
            memset(page_addr, 0, PAGE_SIZE);
            ret = imrsim_read_page(
                zdev->dev->bdev,
                imrsim_ptask.pstore_lba +
                imrsim_pstore_page_offset(idx),
                PAGE_SIZE, page);
            if(ret < 0){
                goto load_error;
            }
            memcpy(((unsigned char *)zone_state + 
                  idx * PAGE_SIZE), page_addr, PAGE_SIZE);
        }
        if(part_page){
            if(num_pages){
                memset(page_addr, 0, PAGE_SIZE);
                ret = imrsim_read_page(
                    zdev->dev->bdev,
                    imrsim_ptask.pstore_lba +
                    imrsim_pstore_page_offset(num_pages),
                    PAGE_SIZE, page);
                if(ret < 0){
                    goto load_error;
                }
            }
            memcpy(((unsigned char *)zone_state + 
                   num_pages * PAGE_SIZE), page_addr, part_page);
        }
        crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header), 
                   zone_state->header.length - sizeof(struct imrsim_state_header));
        if(crc != zone_state->header.crc32){
            printk(KERN_ERR "imrsim: persisted state CRC mismatch\n");
            goto corrupt_state;
        }
        if(zone_state->stats.num_zones != persisted_num_zones){
            printk(KERN_ERR "imrsim: persisted zone count mismatch\n");
            goto corrupt_state;
        }
        zone_status = imrsim_zone_status_ptr(zone_state,
                                             persisted_num_zones);
        persisted_zone_blocks =
            zone_status[0].z_length >> IMR_BLOCK_SIZE_SHIFT_DEFAULT;
        if(!persisted_zone_blocks ||
           persisted_zone_blocks != TOTAL_ITEMS ||
           (zone_status[0].z_length &
            (((__u32)1 << IMR_BLOCK_SIZE_SHIFT_DEFAULT) - 1)) ||
           !is_power_of_2(persisted_zone_blocks) ||
           (__u64)persisted_num_zones * zone_status[0].z_length !=
           sizedev){
            printk(KERN_ERR "imrsim: invalid persisted zone geometry\n");
            goto corrupt_state;
        }
        for(idx = 0; idx < persisted_num_zones; idx++){
            if(zone_status[idx].z_start != idx ||
               zone_status[idx].z_length != zone_status[0].z_length ||
               zone_status[idx].z_map_size > TOTAL_ITEMS ||
               zone_status[idx].z_live_count >
                   zone_status[idx].z_map_size){
                printk(KERN_ERR "imrsim: inconsistent persisted zone geometry at %u\n",
                       idx);
                goto corrupt_state;
            }
        }
        IMR_NUMZONES = persisted_num_zones;
        IMR_ZONE_SIZE_SHIFT = index_power_of_2(persisted_zone_blocks);
        ret = imr_lsm_allocator_recover_locked();
        if(ret){
            printk(KERN_ERR "imrsim: invalid persisted allocator state\n");
            goto corrupt_state;
        }
        printk(KERN_INFO "imrsim: load persist success\n");
    }else if((!header.magic && !header.length &&
              !header.version && !header.crc32) ||
             (header.magic == 0xBEEFBEEF &&
              header.version != VERSION)){
        printk(KERN_INFO "imrsim: uninitialized/old persistence layout; using defaults\n");
        goto invalid_state;
    }else{
        printk(KERN_ERR "imrsim: corrupt persistence header\n");
        ret = -EUCLEAN;
        goto load_error;
    }
    __free_pages(page, 0);
    return 0;

invalid_state:
    __free_pages(page, 0);
    ret = imrsim_init_zone_state(sizedev);
    return ret ? ret : -EINVAL;

corrupt_state:
    ret = -EUCLEAN;
load_error:
    __free_pages(page, 0);
    vfree(zone_state);
    zone_state = NULL;
    zone_status = NULL;
    return ret;
}

struct imrsim_pstore_snapshot
{
    struct imrsim_state *state;
    __u32                length;
    __u32                zone_idx[IMR_PSTORE_QDEPTH];
    __u8                 zone_idx_cnt;
    unsigned char        flag;
    bool                 full_save;
};

static int imrsim_write_snapshot_page(struct imrsim_c *zdev,
                                      struct imrsim_pstore_snapshot *snapshot,
                                      __u32 page_idx,
                                      struct page *page,
                                      void *page_addr)
{
    __u64 offset = (__u64)page_idx * PAGE_SIZE;
    __u32 bytes;

    if(offset >= snapshot->length){
        return -EINVAL;
    }
    bytes = min_t(__u32, PAGE_SIZE, snapshot->length - (__u32)offset);
    memset(page_addr, 0, PAGE_SIZE);
    memcpy(page_addr, (unsigned char *)snapshot->state + offset, bytes);
    return imrsim_write_page(
        zdev->dev->bdev,
        imrsim_ptask.pstore_lba + imrsim_pstore_page_offset(page_idx),
        PAGE_SIZE, page);
}

static int imrsim_write_persistence_snapshot(
    struct dm_target *ti,
    struct imrsim_pstore_snapshot *snapshot)
{
    struct imrsim_c *zdev = ti->private;
    struct imrsim_zone_status *snapshot_zones;
    struct page *page;
    void *page_addr;
    __u32 crc;
    __u32 page_count;
    __u32 page_idx;
    __u32 pg_cur;
    __u32 pg_nxt;
    __u32 qidx;
    __u64 start;
    __u64 end;
    int ret = 0;

    if(snapshot->length < sizeof(struct imrsim_state_header)){
        return -EINVAL;
    }
    crc = crc32(0,
                (unsigned char *)snapshot->state +
                    sizeof(struct imrsim_state_header),
                snapshot->length - sizeof(struct imrsim_state_header));
    snapshot->state->header.crc32 = crc;
    page_count = DIV_ROUND_UP(snapshot->length, PAGE_SIZE);

    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        __free_pages(page, 0);
        return -EINVAL;
    }

    if(snapshot->full_save){
        for(page_idx = 1; page_idx < page_count; page_idx++){
            ret = imrsim_write_snapshot_page(zdev, snapshot, page_idx,
                                              page, page_addr);
            if(ret < 0){
                goto out;
            }
        }
    }else{
        if(snapshot->flag & IMR_STATS_CHANGE){
            pg_nxt = (__u32)((IMR_PSTORE_PG_OFF +
                              (__u64)sizeof(struct imrsim_zone_stats) *
                                  snapshot->state->stats.num_zones - 1) /
                             PAGE_SIZE);
            for(page_idx = 1;
                page_idx <= pg_nxt && page_idx < page_count;
                page_idx++){
                ret = imrsim_write_snapshot_page(zdev, snapshot, page_idx,
                                                  page, page_addr);
                if(ret < 0){
                    goto out;
                }
            }
        }

        if(snapshot->flag & IMR_STATUS_CHANGE){
            snapshot_zones = imrsim_zone_status_ptr(
                snapshot->state, snapshot->state->stats.num_zones);
            for(qidx = 0; qidx < snapshot->zone_idx_cnt; qidx++){
                if(snapshot->zone_idx[qidx] >=
                   snapshot->state->stats.num_zones){
                    ret = -EINVAL;
                    goto out;
                }
                start = (unsigned char *)&snapshot_zones[
                            snapshot->zone_idx[qidx]] -
                        (unsigned char *)snapshot->state;
                end = start + sizeof(struct imrsim_zone_status) - 1;
                pg_cur = (__u32)(start / PAGE_SIZE);
                pg_nxt = (__u32)(end / PAGE_SIZE);
                for(page_idx = pg_cur;
                    page_idx <= pg_nxt && page_idx < page_count;
                    page_idx++){
                    ret = imrsim_write_snapshot_page(
                        zdev, snapshot, page_idx, page, page_addr);
                    if(ret < 0){
                        goto out;
                    }
                }
            }
        }
    }

    /* Commit the CRC-bearing header only after all snapshot data pages. */
    ret = imrsim_write_snapshot_page(zdev, snapshot, 0, page, page_addr);
out:
    __free_pages(page, 0);
    return ret < 0 ? ret : 0;
}

/* Caller must hold imrsim_zone_lock. */
static bool imrsim_capture_persistence_snapshot_locked(
    struct imrsim_pstore_snapshot *snapshot,
    __u32 capacity)
{
    if(!imrsim_ptask.flag || !zone_state){
        return false;
    }
    if(zone_state->header.length > capacity){
        return false;
    }

    snapshot->length = zone_state->header.length;
    snapshot->flag = imrsim_ptask.flag;
    snapshot->zone_idx_cnt = imrsim_ptask.stu_zone_idx_cnt;
    snapshot->full_save =
        (snapshot->flag & IMR_CONFIG_CHANGE) ||
        imrsim_ptask.stu_zone_idx_gap >= IMR_PSTORE_PG_GAP;
    memcpy(snapshot->state, zone_state, snapshot->length);
    memcpy(snapshot->zone_idx, imrsim_ptask.stu_zone_idx,
           sizeof(snapshot->zone_idx));

    imrsim_ptask.flag = IMR_NO_CHANGE;
    memset(imrsim_ptask.stu_zone_idx, 0,
           sizeof(imrsim_ptask.stu_zone_idx));
    imrsim_ptask.stu_zone_idx_cnt = 0;
    imrsim_ptask.stu_zone_idx_gap = 0;
    return true;
}

/* Caller must hold imrsim_zone_lock. */
static void imrsim_restore_persistence_snapshot_locked(
    struct imrsim_pstore_snapshot *snapshot)
{
    __u32 qidx;

    imrsim_ptask.flag |= snapshot->flag;
    for(qidx = 0; qidx < snapshot->zone_idx_cnt; qidx++){
        imrsim_ptask_queue_zone_status_locked(snapshot->zone_idx[qidx]);
    }
    if(snapshot->full_save){
        imrsim_ptask.stu_zone_idx_gap = IMR_PSTORE_PG_GAP;
    }
}

/* persistent storage task */
static int imrsim_persistence_task(void *arg)
{
    struct dm_target *ti = (struct dm_target *)arg;
    struct imrsim_pstore_snapshot snapshot;
    __u32 capacity;
    bool captured;
    int ret;

    memset(&snapshot, 0, sizeof(snapshot));
    mutex_lock(&imrsim_zone_lock);
    capacity = zone_state ? zone_state->header.length : 0;
    mutex_unlock(&imrsim_zone_lock);
    if(!capacity){
        return -ENODEV;
    }
    snapshot.state = vzalloc(capacity);
    if(!snapshot.state){
        printk(KERN_ERR "imrsim: persistence snapshot allocation failed bytes=%u\n",
               capacity);
        return -ENOMEM;
    }

    while(!kthread_should_stop()){
        captured = false;
        if(READ_ONCE(imrsim_ptask.flag)){
            mutex_lock(&imrsim_zone_lock);
            captured = imrsim_capture_persistence_snapshot_locked(
                &snapshot, capacity);
            mutex_unlock(&imrsim_zone_lock);

            ret = captured ?
                imrsim_write_persistence_snapshot(ti, &snapshot) : 0;
            if(ret && printk_ratelimit()){
                printk(KERN_ERR "imrsim: persistence flush failed: %d\n",
                       ret);
            }
            if(ret && captured){
                mutex_lock(&imrsim_zone_lock);
                imrsim_restore_persistence_snapshot_locked(&snapshot);
                mutex_unlock(&imrsim_zone_lock);
            }
        }
        msleep_interruptible(IMR_PSTORE_CHECK);
    }
    vfree(snapshot.state);
    return 0;
}

/* persistent storage thread */
static int imrsim_persistence_thread(struct dm_target *ti)
{
    int ret = 0;

    if(!ti){
        printk(KERN_ERR "imrsim: warning: null device target. Improper usage\n");
        return -EINVAL;
    }
    imrsim_ptask.flag = 0;
    imrsim_ptask.stu_zone_idx_cnt = 0;
    imrsim_ptask.stu_zone_idx_gap = 0;
    memset(imrsim_ptask.stu_zone_idx, 0, sizeof(__u32) * IMR_PSTORE_QDEPTH);
    ret = imrsim_load_persistence(ti);
    if(ret == -EINVAL){
        ret = imrsim_save_persistence(ti);
        if(ret){
            return ret;
        }
    }else if(ret){
        return ret;
    }
    // create thread
    imrsim_ptask.pstore_thread = kthread_create(imrsim_persistence_task, 
                                                ti, "imrsim pthread");
    if(IS_ERR(imrsim_ptask.pstore_thread)){
        ret = PTR_ERR(imrsim_ptask.pstore_thread);
        imrsim_ptask.pstore_thread = NULL;
        printk(KERN_ERR "imrsim persistence thread create failed: %d\n",
               ret);
        return ret;
    }
    printk(KERN_INFO "imrsim persistence thread created\n");
    // After a thread is created with kthread_create, the thread will not start immediately,
    // but needs to be started after calling the wake_up_process function.
    wake_up_process(imrsim_ptask.pstore_thread);
    return 0;
}

/* To update device idle time. */
static void imrsim_dev_idle_update(void)
{
    __u32 dt = 0;
    if(jiffies > imrsim_dev_idle_checkpoint){
        dt = (jiffies - imrsim_dev_idle_checkpoint) / HZ;
    }else{
        dt = (~(__u32)0 - imrsim_dev_idle_checkpoint + jiffies) / HZ;
    }
    if (dt > zone_state->stats.dev_stats.idle_stats.dev_idle_time_max) {
      zone_state->stats.dev_stats.idle_stats.dev_idle_time_max = dt;
   } else if (dt && (dt < zone_state->stats.dev_stats.idle_stats.dev_idle_time_min)) {
      zone_state->stats.dev_stats.idle_stats.dev_idle_time_min = dt;
   }
}

/* status report */
static void imrsim_report_stats(struct imrsim_stats *stats)
{
    __u32 i;
    __u32 num32 = stats->num_zones;

    if (!stats) {
       printk(KERN_ERR "imrsim: NULL pointer passed through\n");
       return;
    }
    printk("Device idle time max: %u\n",
            stats->dev_stats.idle_stats.dev_idle_time_max);
    printk("Device idle time min: %u\n",
            stats->dev_stats.idle_stats.dev_idle_time_min);
    for (i = 0; i < num32; i++) {
        printk("zone[%u] imrsim out of policy read stats: span zones count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_read_stats.span_zones_count);
        printk("zone[%u] imrsim out of policy write stats: span zones count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_write_stats.span_zones_count);
        printk("zone[%u] imrsim out of policy write stats: unaligned count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_write_stats.unaligned_count);
        printk("zone[%u] extra write count: %u\n",
                    i, stats->zone_stats[i].z_extra_write_total);    
        printk("zone[%u] write total count: %u\n",
                    i, stats->zone_stats[i].z_write_total); 
    }

    printk("imrsim extra write total count: %llu\n", stats->extra_write_total);
    printk("imrsim write total count: %llu\n", stats->write_total);
}

/* The following are interface methods with EXPORT_SYMBOL. */

/* To get the last read error. */
int imrsim_get_last_rd_error(__u32 *last_error)
{
    __u32 tmperr = imrsim_dbg_rerr;

    imrsim_dbg_rerr = 0;
    if(last_error){
        *last_error = tmperr;
    }
    return 0;
}
EXPORT_SYMBOL(imrsim_get_last_rd_error);

/* To get the last write error. */
int imrsim_get_last_wd_error(__u32 *last_error)
{
   __u32 tmperr = imrsim_dbg_werr;

   imrsim_dbg_werr  = 0;
   if(last_error)
      *last_error = tmperr;
   return 0;
}
EXPORT_SYMBOL(imrsim_get_last_wd_error);

/* Enable logging. */
int imrsim_set_log_enable(__u32 zero_is_disable)
{
   imrsim_dbg_log_enabled = zero_is_disable;
   return 0;
}
EXPORT_SYMBOL(imrsim_set_log_enable);

/* Disable logging. */
int imrsim_get_num_zones(__u32* num_zones)
{
   printk(KERN_INFO "imrsim: %s: called.\n", __FUNCTION__);
   if (!num_zones) {
      printk(KERN_ERR "imrsim: NULL pointer passed through\n");
      return -EINVAL;
   }
   mutex_lock(&imrsim_zone_lock);
   if(!imrsim_target_ready_locked()){
      mutex_unlock(&imrsim_zone_lock);
      return -ENODEV;
   }
   *num_zones = IMR_NUMZONES;
   mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_get_num_zones);

/* To get the number of sectors in a zone. */
int imrsim_get_size_zone_default(__u32 *size_zone)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!size_zone){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    *size_zone = num_sectors_zone();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_size_zone_default);

/* To set the default zone size. */
int imrsim_set_size_zone_default(__u32 size_zone)
{
    struct imrsim_state *old_state;
    struct imrsim_state *sta_tmp;
    __u64 new_numzones;
    __u32 old_numzones;
    __u32 old_zone_size_shift;
    __u32 state_size;
    int ret;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if((size_zone % ((__u32)1 << IMR_BLOCK_SIZE_SHIFT_DEFAULT)) ||
       !(is_power_of_2(size_zone))){
        printk(KERN_ERR "imrsim: Wrong zone size specified\n");
        return -EINVAL;
    }
    if((size_zone >> IMR_BLOCK_SIZE_SHIFT_DEFAULT) != TOTAL_ITEMS){
        printk(KERN_ERR "imrsim: non-default zone geometry is unsupported by fixed track/map arrays\n");
        return -EOPNOTSUPP;
    }
    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    if((__u64)size_zone > IMR_CAPACITY ||
       IMR_CAPACITY % size_zone){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        printk(KERN_ERR "imrsim: zone size must divide target capacity exactly\n");
        return -EINVAL;
    }
    new_numzones = div64_u64(IMR_CAPACITY, size_zone);
    if(!new_numzones || new_numzones > (__u64)((__u32)~0U)){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -EOVERFLOW;
    }
    old_numzones = IMR_NUMZONES;
    old_zone_size_shift = IMR_ZONE_SIZE_SHIFT;
    IMR_ZONE_SIZE_SHIFT = index_power_of_2((size_zone) >> IMR_BLOCK_SIZE_SHIFT);
    IMR_NUMZONES = (__u32)new_numzones;
    ret = imrsim_state_size(&state_size);
    if(ret ||
       imrsim_state_persistence_bytes(state_size) >
       imrsim_persistence_capacity_bytes){
        IMR_NUMZONES = old_numzones;
        IMR_ZONE_SIZE_SHIFT = old_zone_size_shift;
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return ret ? ret : -ENOSPC;
    }
    sta_tmp = vzalloc((unsigned long)
                      imrsim_state_persistence_bytes(state_size));
    if(!sta_tmp){
        IMR_NUMZONES = old_numzones;
        IMR_ZONE_SIZE_SHIFT = old_zone_size_shift;
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -ENOMEM;
    }
    old_state = zone_state;
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(state_size);
    mutex_lock(&imr_lsm_lock);
    imr_lsm_release_metadata_locked();
    mutex_unlock(&imr_lsm_lock);
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    vfree(old_state);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_size_zone_default);

/* To reset default config. */
int imrsim_reset_default_config(void)
{
    int ret;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    ret = imrsim_reset_default_zone_config();
    if(ret){
        return ret;
    }
    return imrsim_reset_default_device_config();
}
EXPORT_SYMBOL(imrsim_reset_default_config);

/* To reset default device config. */
int imrsim_reset_default_device_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_state->config.dev_config.out_of_policy_read_flag = 0;
    zone_state->config.dev_config.out_of_policy_write_flag = 0;
    zone_state->config.dev_config.r_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    zone_state->config.dev_config.w_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_device_config);

/* To get device config. */
int imrsim_get_device_config(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    memcpy(device_config, &(zone_state->config.dev_config), 
           sizeof(struct imrsim_dev_config));
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_device_config);

/* To set device read config. */
int imrsim_set_device_rconfig(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_state->config.dev_config.out_of_policy_read_flag = 
        device_config->out_of_policy_read_flag;
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_rconfig);

/* To set device write config. */
int imrsim_set_device_wconfig(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_state->config.dev_config.out_of_policy_write_flag = 
        device_config->out_of_policy_write_flag;
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_wconfig);

/* To set read delay. */
int imrsim_set_device_rconfig_delay(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(device_config->r_time_to_rmw_zone >= IMR_TRANSFER_PENALTY_MAX){
        printk(KERN_ERR "time delay exceeds default maximum\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_state->config.dev_config.r_time_to_rmw_zone = 
        device_config->r_time_to_rmw_zone;
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_rconfig_delay);

/* To set write delay. */
int imrsim_set_device_wconfig_delay(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(device_config->w_time_to_rmw_zone >= IMR_TRANSFER_PENALTY_MAX){
        printk(KERN_ERR "time delay exceeds default maximum\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_state->config.dev_config.w_time_to_rmw_zone = 
        device_config->w_time_to_rmw_zone;
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_wconfig_delay);

/* To reset default zone config. */
int imrsim_reset_default_zone_config(void)
{
    struct imrsim_state *old_state;
    struct imrsim_state *sta_tmp;
    __u32 old_numzones;
    __u32 old_zone_size_shift;
    __u32 state_size;
    int ret;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return -ENODEV;
    }
    old_numzones = IMR_NUMZONES;
    old_zone_size_shift = IMR_ZONE_SIZE_SHIFT;
    IMR_NUMZONES = IMR_NUMZONES_DEFAULT;
    IMR_ZONE_SIZE_SHIFT = IMR_ZONE_SIZE_SHIFT_DEFAULT;
    ret = imrsim_state_size(&state_size);
    if(ret ||
       imrsim_state_persistence_bytes(state_size) >
       imrsim_persistence_capacity_bytes){
        IMR_NUMZONES = old_numzones;
        IMR_ZONE_SIZE_SHIFT = old_zone_size_shift;
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        return ret ? ret : -ENOSPC;
    }
    sta_tmp = vzalloc((unsigned long)
                      imrsim_state_persistence_bytes(state_size));
    if(!sta_tmp){
        IMR_NUMZONES = old_numzones;
        IMR_ZONE_SIZE_SHIFT = old_zone_size_shift;
        mutex_unlock(&imrsim_zone_lock);
        mutex_unlock(&imr_lsm_compaction_lock);
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -ENOMEM;
    }
    old_state = zone_state;
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(state_size);
    /*
     * A fresh zone map cannot safely retain LSM entries that refer to the old
     * physical map.  Reset both under the documented zone -> LSM lock order.
     */
    mutex_lock(&imr_lsm_lock);
    imr_lsm_release_metadata_locked();
    mutex_unlock(&imr_lsm_lock);
    imrsim_ptask_mark_config_change_locked();
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    vfree(old_state);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_zone_config);

/* To clear config of a zone. */
int imrsim_clear_zone_config(void)
{
    printk(KERN_ERR "imrsim: deprecated incremental zone config is unsupported\n");
    return -EOPNOTSUPP;
}
EXPORT_SYMBOL(imrsim_clear_zone_config);

/* Count the number of Z_TYPE_SEQUENTIAL type zones. @Deprecated */
static int imrsim_zone_seq_count(void)
{
    __u32 count = 0;
    __u32 index;

    for(index = 0; index < IMR_NUMZONES; index++){
        if(zone_status[index].z_type == Z_TYPE_SEQUENTIAL){
            count++;
        }
    }
    return count;
}

/* To check if the zone status is correct. */
static int imrsim_zone_cond_check(__u16 cond)
{
    switch(cond){
        case Z_COND_NO_WP:
        case Z_COND_EMPTY:
        case Z_COND_CLOSED:
        case Z_COND_OPEN:
        case Z_COND_RO:
        case Z_COND_FULL:
        case Z_COND_OFFLINE:
            return 1;
        default:
            return 0;
    }
    return 0;
}

/* To modify zone configuration. @Deprecated */
int imrsim_modify_zone_config(struct imrsim_zone_status *z_status)
{
    __u32 count;
    int ret = 0;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__); 
    if(!z_status){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        ret = -ENODEV;
        goto out;
    }
    count = imrsim_zone_seq_count();
    if(IMR_NUMZONES <= z_status->z_start){
        printk(KERN_ERR "imrsim: config does not exist\n");
        ret = -EINVAL;
        goto out;
    }
    if(1 >= count && (Z_TYPE_SEQUENTIAL == z_status->z_type) &&
      (Z_TYPE_SEQUENTIAL == zone_status[z_status->z_start].z_type))
    {
        printk(KERN_ERR "imrsim: zone type is not allowed to modify\n");
        ret = -EINVAL;
        goto out;
    }
    if(z_status->z_length != num_sectors_zone()){
        printk(KERN_ERR "imrsim: zone size is not allowed to change individually\n");
        ret = -EINVAL;
        goto out;
    }
    if(!imrsim_zone_cond_check(z_status->z_conds)){
        printk(KERN_ERR "imrsim: wrong zone condition\n");
        ret = -EINVAL;
        goto out;
    }
    if((z_status->z_conds == Z_COND_NO_WP) && 
        (z_status->z_type != Z_TYPE_CONVENTIONAL))
    {
        printk(KERN_ERR "imrsim: condition and type mismatch\n");
        ret = -EINVAL;
        goto out;
    }
    if ((Z_COND_EMPTY == z_status->z_conds) && 
       (Z_TYPE_SEQUENTIAL == z_status->z_type) ) {
        printk(KERN_ERR "imrsim: empty zone isn't empty\n");
        ret = -EINVAL;
        goto out;
    }

    zone_status[z_status->z_start].z_conds = 
      (enum imrsim_zone_conditions)z_status->z_conds;
    zone_status[z_status->z_start].z_type = 
      (enum imrsim_zone_type)z_status->z_type;
    zone_status[z_status->z_start].z_flag = 0;
    imrsim_ptask_queue_zone_status_locked((__u32)z_status->z_start);
    printk(KERN_DEBUG "imrsim: zone[%lu] modified. type:0x%x conds:0x%x\n",
      zone_status[z_status->z_start].z_start,
      zone_status[z_status->z_start].z_type, 
      zone_status[z_status->z_start].z_conds);
out:
    mutex_unlock(&imrsim_zone_lock);
    return ret;
}
EXPORT_SYMBOL(imrsim_modify_zone_config);

/* To add zone configuration. @Deprecated */
int imrsim_add_zone_config(struct imrsim_zone_status *zone_sts)
{
    (void)zone_sts;
    printk(KERN_ERR "imrsim: deprecated incremental zone config is unsupported\n");
    return -EOPNOTSUPP;
}
EXPORT_SYMBOL(imrsim_add_zone_config);

/* To reset statistics for a zone. */
int imrsim_reset_zone_stats(sector_t start_sector)
{
    __u32 zone_idx;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_idx = start_sector >> IMR_BLOCK_SIZE_SHIFT >>
               IMR_ZONE_SIZE_SHIFT;
    if(IMR_NUMZONES <= zone_idx){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: %s start sector is out of range\n", __FUNCTION__);
        return -EINVAL;
    }
    memset(&(zone_state->stats.zone_stats[zone_idx].out_of_policy_read_stats),
          0, sizeof(struct imrsim_out_of_policy_read_stats));
    memset(&(zone_state->stats.zone_stats[zone_idx].out_of_policy_write_stats),
          0, sizeof(struct imrsim_out_of_policy_write_stats));
    memset(&(zone_state->stats.zone_stats[zone_idx].z_extra_write_total),
          0, sizeof(__u32));
    memset(&(zone_state->stats.zone_stats[zone_idx].z_write_total),
          0, sizeof(__u32));
    imrsim_ptask.flag |= IMR_STATS_CHANGE;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_zone_stats);

/* To reset zone_stats. */
int imrsim_reset_stats(void)
{
    printk(KERN_INFO "imrsim: %s: called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    imrsim_reset_stats_locked();
    imrsim_ptask.flag |= IMR_STATS_CHANGE;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_stats);

/* To get zone_stats. */
int imrsim_get_stats(struct imrsim_stats *stats)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!stats){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    memcpy(stats, &(zone_state->stats), imrsim_stats_size());
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_stats);

/* @Deprecated */
int imrsim_blkdev_reset_zone_ptr(sector_t start_sector)
{
    //__u32 rem;
    __u32 zone_idx;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    zone_idx = start_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if(IMR_NUMZONES <= zone_idx){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: %s start_sector is out of range\n", __FUNCTION__);
        return -EINVAL;
    }
    if (zone_status[zone_idx].z_type == Z_TYPE_CONVENTIONAL) {
      mutex_unlock(&imrsim_zone_lock);
      printk(KERN_ERR "imrsim:error: CMR zone dosen't have a write pointer.\n");
      return -EINVAL;
    }
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_blkdev_reset_zone_ptr);

/* error log */
void imrsim_log_error(struct bio* bio, __u32 uerr)
{
    __u64 lba;

    if (!bio) {
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return;
    }
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    lba = bio->bi_sector;
    #else
    lba = bio->bi_iter.bi_sector;
    #endif
    if (imrsim_dbg_log_enabled) {
        switch(uerr)
        {
            case IMR_ERR_READ_BORDER:
                printk(KERN_DEBUG "%s: lba:%llu IMR_ERR_READ_BORDER\n", __FUNCTION__, lba);
                imrsim_dbg_rerr = uerr;
                break;
            case IMR_ERR_READ_POINTER: 
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_READ_POINTER\n",__FUNCTION__, lba);
                imrsim_dbg_rerr = uerr;
                break;
            case IMR_ERR_READ_ALIGN:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_READ_ALIGN\n",
                       __FUNCTION__, lba);
                imrsim_dbg_rerr = uerr;
                break;
            case IMR_ERR_WRITE_RO:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_RO\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_POINTER :
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_POINTER\n",__FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_ALIGN :
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_ALIGN\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_BORDER:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_BORDER\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_FULL:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_FULL\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            default:
                printk(KERN_DEBUG "%s: lba:%llu: UNKNOWN ERR=%u\n", __FUNCTION__, lba, uerr);
        }
    }
}

/* The following is the relevant method to build the target_type structure. */
/* device creation */
static int imrsim_ctr(struct dm_target *ti,
                      unsigned int argc,
                      char **argv)
{
    unsigned long long tmp;
    int ret;
    char dummy;
    struct imrsim_c *c = NULL;
    __u64 num;
    __u64 backing_bytes;
    __u64 target_bytes;
    __u64 persistence_bytes;
    __u32 state_size;
    __u32 candidate_zone;
    bool queue_zone_gc = false;

    printk(KERN_INFO "imrsim: %s called\n", __FUNCTION__);
    if(!ti){
        printk(KERN_ERR "imrsim: error: invalid device\n");
        return -EINVAL;
    }
    if(2 != argc){
        ti->error = "dm-imrsim: error: invalid argument count; !=2";
        return -EINVAL;
    }
    if(ti->begin != 0){
        ti->error = "dm-imrsim: target must begin at sector 0";
        return -EINVAL;
    }
    if(1 != sscanf(argv[1], "%llu%c", &tmp, &dummy)){
        ti->error = "dm-imrsim: error: invalid argument device sector";
        return -EINVAL;
    }
    /*
     * Zone keys and the reserved persistence area are currently addressed
     * from the beginning of the backing device.
     */
    if(tmp != 0){
        ti->error = "dm-imrsim: backing start must be sector 0";
        return -EINVAL;
    }

    mutex_lock(&imrsim_zone_lock);
    if(imrsim_single != IMRSIM_TARGET_INACTIVE){
        printk(KERN_ERR "imrsim: No multiple device support currently\n");
        ret = -EBUSY;
        goto out_unlock;
    }
    mutex_lock(&imr_lsm_lock);
    imrsim_diag_reset();
    mutex_unlock(&imr_lsm_lock);

    c = kmalloc(sizeof(*c), GFP_KERNEL);    // To allocate physically contiguous memory.
    if(!c){
        ti->error = "dm-imrsim: error: no enough memory";
        ret = -ENOMEM;
        goto out_unlock;
    }
    c->start = tmp;
    // Fill in the bdev of the device specified by path and the corresponding interval, permission, mode, etc. into ti->table.
    ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
    if(ret){
        ti->error = "dm-imrsim: error: device lookup failed";
        goto out_free_c;
    }
    if(ti->len > IMR_MAX_CAPACITY){
        printk(KERN_ERR "imrsim: capacity %llu exceeds the maximum 10TB\n", (__u64)ti->len);
        ret = -EINVAL;
        goto out_put_device;
    }
    num = ti->len >> IMR_BLOCK_SIZE_SHIFT_DEFAULT >>
          IMR_ZONE_SIZE_SHIFT_DEFAULT;
    if((num << IMR_BLOCK_SIZE_SHIFT_DEFAULT <<
        IMR_ZONE_SIZE_SHIFT_DEFAULT) != ti->len){
        printk(KERN_ERR "imrsim:error: total size must be zone size (256MB) aligned\n");
        ret = -EINVAL;
        goto out_put_device;
    }
    if(ti->len < ((sector_t)2 << IMR_BLOCK_SIZE_SHIFT_DEFAULT <<
                  IMR_ZONE_SIZE_SHIFT_DEFAULT)){
      printk(KERN_INFO "imrsim: capacity: %llu sectors\n", (__u64)ti->len);
      printk(KERN_ERR "imrsim:error: active-zone allocator requires at least two 256MB zones\n");
      ret = -EINVAL;
      goto out_put_device;
    }
    if(num > (__u64)((__u32)~0U)){
        ti->error = "dm-imrsim: zone count exceeds persistent format";
        ret = -EOVERFLOW;
        goto out_put_device;
    }
    ret = imrsim_state_size_for_zones((__u32)num, &state_size);
    if(ret){
        ti->error = "dm-imrsim: zone state exceeds persistent format";
        goto out_put_device;
    }
    target_bytes = (__u64)ti->len << IMR_SECTOR_SIZE_SHIFT_DEFAULT;
    backing_bytes = (__u64)i_size_read(c->dev->bdev->bd_inode);
    persistence_bytes = imrsim_state_persistence_bytes(state_size);
    if(backing_bytes < target_bytes ||
       backing_bytes - target_bytes < persistence_bytes){
        printk(KERN_ERR "imrsim: backing tail too small: have=%llu need=%llu bytes for %llu zones\n",
               (unsigned long long)(backing_bytes < target_bytes ?
                                    0 : backing_bytes - target_bytes),
               (unsigned long long)persistence_bytes,
               (unsigned long long)num);
        ti->error = "dm-imrsim: insufficient backing tail for persistence";
        ret = -ENOSPC;
        goto out_put_device;
    }

    /*
     * The mapping and track bookkeeping are 4K-key granular.  Keep normal
     * reads/writes bounded to one 4K key where possible; unaligned head/tail
     * bios are completed by the target's worker-based partial-block RMW path.
     * Discard bios use their separate queue limit and stay range-based.
     */
    ret = dm_set_target_max_io_len(
        ti, (sector_t)1 << IMR_BLOCK_SIZE_SHIFT_DEFAULT);
    if(ret){
        goto out_put_device;
    }
    imrsim_persistence_capacity_bytes = backing_bytes - target_bytes;

    ti->num_flush_bios = 1;
    ti->num_discard_bios = 1;
    IMRSIM_DM_PER_BIO_DATA_SIZE(ti) =
        sizeof(struct imrsim_io_context);
    /*
     * WRITE SAME has separate dm splitting rules and this target does not
     * implement it.  Do not advertise a path that could bypass max_io_len.
     */
    ti->num_write_same_bios = 0;
    /*
     * IMR-LSM handles discard as a metadata-only tombstone update and completes
     * the bio without forwarding it to the backing disk. Request discard bios
     * even when the underlying device does not advertise native discard support.
     */
    ti->discards_supported = 1;
    ti->private = c;
    imrsim_ptask.pstore_thread = NULL;
    imrsim_dbg_rerr = imrsim_dbg_werr = imrsim_dbg_log_enabled = 0;

    mutex_lock(&imr_lsm_lock);
    imr_lsm_reset_validation_overrides_locked();
    imr_lsm_output_bdev = c->dev->bdev;
    imr_lsm_output_bdev_start = c->start;
    mutex_unlock(&imr_lsm_lock);

    ret = imrsim_persistence_thread(ti);
    if(ret){
        printk(KERN_ERR "imrsim: error: metadata persistence setup failed: %d\n",
               ret);
        ti->error = "dm-imrsim: metadata persistence setup failed";
        goto out_clear_state;
    }

    imrsim_single = IMRSIM_TARGET_ACTIVE;
    mutex_lock(&imr_lsm_lock);
    imr_lsm_record_zone_compaction_candidate_locked(0);
    candidate_zone =
        imr_lsm_meta.stats.zone_compaction_candidate_zone;
    if(candidate_zone != IMR_LSM_ZONE_COMPACTION_NONE){
        queue_zone_gc =
            imr_lsm_defer_zone_compaction_auto_run_locked(candidate_zone);
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    if(queue_zone_gc){
        imr_lsm_queue_zone_compaction_auto_work();
    }
    return 0;

out_clear_state:
    mutex_lock(&imr_lsm_lock);
    imr_lsm_output_bdev = NULL;
    imr_lsm_output_bdev_start = 0;
    imr_lsm_release_metadata_locked();
    imr_lsm_reset_validation_overrides_locked();
    mutex_unlock(&imr_lsm_lock);
    vfree(zone_state);
    zone_state = NULL;
    zone_status = NULL;
    IMR_NUMZONES = 0;
    imrsim_persistence_capacity_bytes = 0;
    ti->private = NULL;
out_put_device:
    dm_put_device(ti, c->dev);
out_free_c:
    kfree(c);
out_unlock:
    mutex_unlock(&imrsim_zone_lock);
    return ret;
}

/* device destory */
static void imrsim_dtr(struct dm_target *ti)
{
    struct imrsim_c *c = (struct imrsim_c *) ti->private;
    struct imrsim_state *old_state;
    int persistence_ret;

    /*
     * Close the module-lifetime debugfs gate first.  Taking/releasing the zone
     * lock also waits for any already-running zone/LSM debugfs operation.
     */
    mutex_lock(&imrsim_ioctl_lock);
    mutex_lock(&imrsim_zone_lock);
    imrsim_single = IMRSIM_TARGET_TEARDOWN;
    mutex_unlock(&imrsim_zone_lock);

    if(imrsim_ptask.pstore_thread){
        kthread_stop(imrsim_ptask.pstore_thread);
        imrsim_ptask.pstore_thread = NULL;
    }
    cancel_work_sync(&imr_lsm_level_compaction_work);
    cancel_work_sync(&imr_lsm_zone_compaction_work);

    /* A manual zone compaction is not owned by either workqueue.  Wait for
     * its unlocked physical-copy phase before releasing the backing device. */
    mutex_lock(&imr_lsm_compaction_lock);
    mutex_lock(&imrsim_zone_lock);
    /*
     * kthread_stop() does not flush its pending flags.  Save the complete
     * snapshot while the backing device and zone state are still alive so an
     * orderly dm remove cannot lose the final second of metadata updates.
     */
    persistence_ret = zone_state ? imrsim_save_persistence(ti) : 0;
    if(persistence_ret){
        printk(KERN_ERR "imrsim: final persistence save failed: %d\n",
               persistence_ret);
    }
    mutex_lock(&imr_lsm_lock);
    imr_lsm_output_bdev = NULL;
    imr_lsm_output_bdev_start = 0;
    imr_lsm_release_metadata_locked();
    imr_lsm_reset_validation_overrides_locked();
    imr_lsm_allocator_reset_locked();
    old_state = zone_state;
    zone_state = NULL;
    zone_status = NULL;
    IMR_NUMZONES = 0;
    imrsim_persistence_capacity_bytes = 0;
    imrsim_single = IMRSIM_TARGET_INACTIVE;
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    mutex_unlock(&imr_lsm_compaction_lock);
    mutex_unlock(&imrsim_ioctl_lock);

    dm_put_device(ti, c->dev);
    kfree(c);
    vfree(old_state);
    printk(KERN_INFO "imrsim target destructed\n");
}

/*
 * Active-zone foreground write path.  The logical key is never used to pick
 * a physical zone, and every version receives a new append slot.
 */
static int imrsim_active_write_rule_check(struct bio *bio,
                                          __u32 logical_zone_idx,
                                          sector_t bio_sectors,
                                          int policy_flag)
{
    sector_t logical_lba;
    sector_t physical_lba;
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    __u32 physical_zone;
    int ret;

    (void)logical_zone_idx;
    (void)policy_flag;
    if(!bio || imrsim_bio_is_internal_io(bio)){
        return -EINVAL;
    }
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    logical_lba = bio->bi_sector;
#else
    logical_lba = bio->bi_iter.bi_sector;
#endif
    if(bio_sectors != block_sectors ||
       (logical_lba & (block_sectors - 1))){
        return IMR_ERR_WRITE_ALIGN;
    }

    imr_lsm_record_logical_write();
    ret = imr_lsm_append_logical_block_locked(logical_lba,
                                              &physical_lba,
                                              &physical_zone);
    if(ret){
        return ret;
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    bio->bi_sector = physical_lba;
#else
    bio->bi_iter.bi_sector = physical_lba;
#endif
    zone_state->stats.zone_stats[physical_zone].z_write_total++;
    zone_state->stats.write_total++;
    IMRSIM_DATA_LOG("imrsim: append key=%llu logical_lba=%llu physical_zone=%u pba=%llu\n",
                    (unsigned long long)
                        (logical_lba >> IMR_BLOCK_SIZE_SHIFT),
                    (unsigned long long)logical_lba,
                    physical_zone,
                    (unsigned long long)physical_lba);
    return 0;
}

/* Legacy in-zone allocator retained only as a reference for the IMR track
 * penalty model; it is not selected by the data path. */
/* Device Write Rules */
static int __maybe_unused
imrsim_legacy_write_rule_check(struct bio *bio, __u32 zone_idx,
                               sector_t bio_sectors, int policy_flag)
{
    __u64  lba;
    __u64  lba_offset;    // The offset of lba in the zone
    __u64  block_offset;  // The offset of the block in the zone
    __u64  boundary;
    __u64  elba;
    __u64  zlba;
    __u32  relocateTrackno;   // In a stage allocation, how many tracks are the relocated lba on?
    __u32  rv;       // rule violation
    __u32  z_size;
    __u32  trackno;  // on the top-bottom track group
    __u32  blockno;  // The number of the block corresponding to lba on the track
    __u32  physical_zone_idx;
    __u32  trackrate;  // Track ratio, p.s. linux kernel does not support floating point calculation.
    __u16  wa_penalty;
    __u8   isTopTrack;
    __u8   rewriteSign;
    __u8   ret=1;       // Determine whether the block requested by lba is in the mapping table.
    int    lsm_ret;

    zlba = zone_idx_lba(zone_idx);
    physical_zone_idx = zone_idx;

    /* Relocate bio according to phase. */
    if(!imrsim_bio_is_internal_io(bio))
    {
        imr_lsm_record_logical_write();
        /* 根据phase来重定位bio */
        #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        switch(IMR_ALLOCATION_PHASE)
        {
            case 1:
				lba = bio->bi_sector;
				break;
			case 2:
				lba = bio->bi_sector;
				//printk(KERN_INFO "imrsim: request- lba(sectors) is %llu\n", lba);
				lba_offset = bio->bi_sector - zlba;
				block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
				//Check the mapping table, ret indicates whether the block where lba is located is in the mapping table
				ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
				if(!ret){         // lba is not in the mapping table, indicating a new write operation
					// Fill the mapping table, allocate tracks according to the stage, and write data
					// Note: Multiply TOP_TRACK_NUM_TOTAL because the number of top and bottom tracks in the zone is equal
					boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
					if(zone_status[zone_idx].z_map_size < boundary){
						// Indicates that the relocated lba should be on the bottom track, in the first stage allocation
						isTopTrack = 0;
						// Judgment should be redirected to the first few bottom tracks
						relocateTrackno = zone_status[zone_idx].z_map_size / IMR_BOTTOM_TRACK_SIZE;
						// Get the pba corresponding to the bio starting lba
						bio->bi_sector = zlba  
							+ (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
							+ ((zone_status[zone_idx].z_map_size % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						IMRSIM_DATA_LOG("imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size +=
							imrsim_write_block_count(lba, bio_sectors);
						lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}else{
						// Indicates that the relocated lba should be on the top track, in a second stage allocation
						isTopTrack = 1;
						relocateTrackno = (zone_status[zone_idx].z_map_size - boundary) / IMR_TOP_TRACK_SIZE;
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((zone_status[zone_idx].z_map_size - boundary) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
						IMRSIM_DATA_LOG("imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size +=
							imrsim_write_block_count(lba, bio_sectors);
						lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}
				}else{            // lba is in the mapping table, indicating an update operation
					// Get pba from the mapping table, modify lba in bio
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					IMRSIM_DATA_LOG("imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
					lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
					if(lsm_ret){
						return lsm_ret;
					}
					lba = bio->bi_sector;
				}
				break;
			case 3:
				lba = bio->bi_sector;
				lba_offset = bio->bi_sector - zlba;
				block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
				ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
				if(!ret){         // a new write operation
					boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
					__u32 mapSize = zone_status[zone_idx].z_map_size;
					if(mapSize < boundary){
						// first stage allocation
						isTopTrack = 0;
						relocateTrackno = mapSize / IMR_BOTTOM_TRACK_SIZE;
						bio->bi_sector = zlba  
							+ (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
							+ ((mapSize % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						IMRSIM_DATA_LOG("imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size +=
							imrsim_write_block_count(lba, bio_sectors);
						lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}else if(mapSize >= boundary 
						&& mapSize < boundary + IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2){
						// In second stage allocation Top(0,2,4,...)
						isTopTrack = 1;
						relocateTrackno = 2*((mapSize - boundary) / IMR_TOP_TRACK_SIZE);
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((mapSize - boundary) % IMR_TOP_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						IMRSIM_DATA_LOG("imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size +=
							imrsim_write_block_count(lba, bio_sectors);
						lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}else{
						// In the third stage allocation Top(1,3,5,...)
						isTopTrack = 1;
						relocateTrackno = 2*((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) 
										/ IMR_TOP_TRACK_SIZE) + 1;
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
						IMRSIM_DATA_LOG("imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size +=
							imrsim_write_block_count(lba, bio_sectors);
						lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}
				}else{            // an update operation
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					IMRSIM_DATA_LOG("imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_sector);
					lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_sector, bio_sectors);
					if(lsm_ret){
						return lsm_ret;
					}
					lba = bio->bi_sector;
				}
				break;
			default:
				printk(KERN_ERR "imrsim: error: Allocation of more phases is not currently supported!\n");
        }
        #else
        switch(IMR_ALLOCATION_PHASE)
        {
            case 1:
                lba = bio->bi_iter.bi_sector;
                break;
            case 2:
                lba = bio->bi_iter.bi_sector;
                //printk(KERN_INFO "imrsim: request- lba(sectors) is %llu\n", lba);
                lba_offset = bio->bi_iter.bi_sector - zlba;
                block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
                //Check the mapping table, ret indicates whether the block where lba is located is in the mapping table
                ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
                if(!ret){          // lba is not in the mapping table, indicating a new write operation
                    // Fill the mapping table, allocate tracks according to the stage, and write data
					// Note: Multiply TOP_TRACK_NUM_TOTAL because the number of top and bottom tracks in the zone is equal
                    boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
                    if(zone_status[zone_idx].z_map_size < boundary){
                        // Indicates that the relocated lba should be on the bottom track, in the first stage allocation
                        isTopTrack = 0;
                        // Judgment should be redirected to the first few bottom tracks
                        relocateTrackno = zone_status[zone_idx].z_map_size / IMR_BOTTOM_TRACK_SIZE;
                        // Get the pba corresponding to the bio starting lba
                        bio->bi_iter.bi_sector = zlba  
                            + (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
                            + ((zone_status[zone_idx].z_map_size % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        IMRSIM_DATA_LOG("imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size +=
                            imrsim_write_block_count(lba, bio_sectors);
                        lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }else{
                        // Indicates that the relocated lba should be on the top track, in a second stage allocation
                        isTopTrack = 1;
                        relocateTrackno = (zone_status[zone_idx].z_map_size - boundary) / IMR_TOP_TRACK_SIZE;
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((zone_status[zone_idx].z_map_size - boundary) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
                        IMRSIM_DATA_LOG("imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size +=
                            imrsim_write_block_count(lba, bio_sectors);
                        lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // lba is in the mapping table, indicating an update operation
                    // Get pba from the mapping table, modify lba in bio
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    IMRSIM_DATA_LOG("imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                    lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                    if(lsm_ret){
                        return lsm_ret;
                    }
                    lba = bio->bi_iter.bi_sector;
                }
                break;
            case 3:
                lba = bio->bi_iter.bi_sector;
                lba_offset = bio->bi_iter.bi_sector - zlba;
                block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
                ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
                if(!ret){         // a new write operation
                    // 填充映射表，按照阶段情况分配磁道，写入数据
                    boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
                    __u32 mapSize = zone_status[zone_idx].z_map_size;
                    if(mapSize < boundary){
                        // first stage allocation
                        isTopTrack = 0;
                        relocateTrackno = mapSize / IMR_BOTTOM_TRACK_SIZE;
                        bio->bi_iter.bi_sector = zlba  
                            + (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
                            + ((mapSize % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        IMRSIM_DATA_LOG("imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size +=
                            imrsim_write_block_count(lba, bio_sectors);
                        lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }else if(mapSize >= boundary 
                        && mapSize < boundary + IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2){
                        // In second stage allocation Top(0,2,4,...)
                        isTopTrack = 1;
                        relocateTrackno = 2*((mapSize - boundary) / IMR_TOP_TRACK_SIZE);
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((mapSize - boundary) % IMR_TOP_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        IMRSIM_DATA_LOG("imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size +=
                            imrsim_write_block_count(lba, bio_sectors);
                        lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }else{
                        // In the third stage allocation Top(1,3,5,...)
                        isTopTrack = 1;
                        relocateTrackno = 2*((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) 
                                        / IMR_TOP_TRACK_SIZE) + 1;
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
                        IMRSIM_DATA_LOG("imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size +=
                            imrsim_write_block_count(lba, bio_sectors);
                        lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // an update operation
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    IMRSIM_DATA_LOG("imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_iter.bi_sector);
                    lsm_ret = imrsim_record_write_mapping_range(zone_idx, lba, bio->bi_iter.bi_sector, bio_sectors);
                    if(lsm_ret){
                        return lsm_ret;
                    }
                    lba = bio->bi_iter.bi_sector;
                }
                break;
            default:
                printk(KERN_ERR "imrsim: error: Allocation of more phases is not currently supported!\n");
        }
        #endif
        /* relocate bio end */
    }else{
        #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        lba = bio->bi_sector;
        #else
        lba = bio->bi_iter.bi_sector;
        #endif
        IMRSIM_DATA_LOG("imrsim DIRECT write option.\n");
    }
    
    physical_zone_idx = imrsim_lba_zone_idx((sector_t)lba);
    if(physical_zone_idx >= IMR_NUMZONES){
        printk(KERN_ERR "imrsim: remapped write lba is out of range. zone_idx: %u\n",
               physical_zone_idx);
        return IMR_ERR_OUT_RANGE;
    }
    imrsim_ptask_queue_zone_status_locked(physical_zone_idx);
    zlba = zone_idx_lba(physical_zone_idx);

    rv = 0;
    elba = lba + bio_sectors;
    z_size = num_sectors_zone();

    if ((policy_flag == 1) &&
        (zone_status[physical_zone_idx].z_conds == Z_COND_FULL)) {
        zone_status[physical_zone_idx].z_conds = Z_COND_CLOSED;
    } 
    if(elba > (zlba + z_size)){
        printk(KERN_ERR "imrsim: error: write across physical zone: %u.%012llx.%08lx\n",
               physical_zone_idx, lba, bio_sectors);
        rv++;
        zone_state->stats.zone_stats[physical_zone_idx]
            .out_of_policy_write_stats.span_zones_count++;
        imrsim_log_error(bio, IMR_ERR_WRITE_BORDER);
        if(!policy_flag){
            return IMR_ERR_WRITE_BORDER;
        }
        printk(KERN_ERR "imrsim:error: out of policy write allowed pass\n");
    }
    if (imrsim_dbg_log_enabled && printk_ratelimit()) {
        printk(KERN_INFO "imrsim write PASS\n");
    }
    if (rv && (policy_flag ==1)) {
        printk(KERN_ERR "imrsim: out of policy passed rule violation: %u\n", rv); 
        return IMR_ERR_OUT_OF_POLICY;
    }
    //printk(KERN_INFO "imrsim: %s called! lba: %llu, zlba: %llu ~~\n", __FUNCTION__, lba, zlba);

    trackno = (lba - zlba) / ((IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) << 
                IMR_BLOCK_SIZE_SHIFT);
    if(trackno >= TOP_TRACK_NUM_TOTAL){
        printk(KERN_ERR "imrsim: error: remapped write track out of range: zone=%u track=%u\n",
               physical_zone_idx, trackno);
        imrsim_log_error(bio, IMR_ERR_WRITE_BORDER);
        return IMR_ERR_WRITE_BORDER;
    }
    // If it is a new write operation, there is no need to judge isTopTrack
    if(ret){
        isTopTrack = (lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                    IMR_BLOCK_SIZE_SHIFT))) < (IMR_TOP_TRACK_SIZE << IMR_BLOCK_SIZE_SHIFT) ? 1 : 0;
    }
    IMRSIM_DATA_LOG("imrsim: %s trackno: %u, isTopTrack: %u.\n",
                    __FUNCTION__, trackno, isTopTrack);

    // record this write operation
    zone_state->stats.zone_stats[physical_zone_idx].z_write_total++;
    zone_state->stats.write_total++;

    // If lba is on the top track, mark the top track with data, and on the bottom track, determine whether to rewrite
    if(isTopTrack){
        blockno = (lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                IMR_BLOCK_SIZE_SHIFT))) >> IMR_BLOCK_SIZE_SHIFT;
        zone_status[physical_zone_idx].z_tracks[trackno]
            .isUsedBlock[blockno]=1;
        //printk(KERN_INFO "imrsim: SIGN - block is remember\n");
    }else{
        wa_penalty=0;
        rewriteSign=0;
        blockno = ((lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                IMR_BLOCK_SIZE_SHIFT))) >> IMR_BLOCK_SIZE_SHIFT) - IMR_TOP_TRACK_SIZE;
        trackrate = IMR_BOTTOM_TRACK_SIZE * 10000 / IMR_TOP_TRACK_SIZE;
        int wa_pba1=-1,wa_pba2=-1;
        imrsim_rmw_task.lba_num=0;
        if(trackno>=0 && zone_status[physical_zone_idx].z_tracks[trackno].isUsedBlock[(__u32)(blockno*10000/trackrate)]==1){
            IMRSIM_DATA_LOG("imrsim: write amplification(zone_idx[%u]trackno), block: %u .\n",
                            physical_zone_idx,
                            (__u32)(blockno * 10000 / trackrate));
            // record write amplification
            zone_state->stats.zone_stats[physical_zone_idx].z_extra_write_total++;
            zone_state->stats.zone_stats[physical_zone_idx].z_write_total++;
            zone_state->stats.extra_write_total++;
            zone_state->stats.write_total++;
            rewriteSign++;
            lba = zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<IMR_BLOCK_SIZE_SHIFT) 
                    + ((__u32)(blockno*10000/trackrate) <<IMR_BLOCK_SIZE_SHIFT);
            imrsim_rmw_task.lba[imrsim_rmw_task.lba_num] = (sector_t)lba;
            imrsim_rmw_task.lba_num++;
            wa_pba1=lba>>IMR_BLOCK_SIZE_SHIFT;
        }
        if(trackno+1<TOP_TRACK_NUM_TOTAL && zone_status[physical_zone_idx].z_tracks[trackno+1].isUsedBlock[(__u32)(blockno*10000/trackrate)]==1){
            IMRSIM_DATA_LOG("imrsim: write amplification(trackno+1), block: %u .\n",
                            (__u32)(blockno * 10000 / trackrate));
            zone_state->stats.zone_stats[physical_zone_idx].z_extra_write_total++;
            zone_state->stats.zone_stats[physical_zone_idx].z_write_total++;
            zone_state->stats.extra_write_total++;
            zone_state->stats.write_total++;
            rewriteSign++;
            lba = zlba + ((trackno+1) * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<IMR_BLOCK_SIZE_SHIFT) 
                    + ((__u32)(blockno*10000/trackrate) <<IMR_BLOCK_SIZE_SHIFT);
            imrsim_rmw_task.lba[imrsim_rmw_task.lba_num] = (sector_t)lba;
            imrsim_rmw_task.lba_num++;
            wa_pba2=lba>>IMR_BLOCK_SIZE_SHIFT;
        }
        if(1 <= rewriteSign){
            IMRSIM_DATA_LOG("imrsim: WA, wa_pba_1:%d,wa_pba_2:%d.\n",
                            wa_pba1, wa_pba2);
            return 1;
        }
    }
    return 0;
}

/* Device Read Rules */
int imrsim_read_rule_check(struct bio *bio, __u32 zone_idx, 
                           sector_t bio_sectors, int policy_flag)
{
    __u64 lba;
    __u64 zlba;
    __u64 elba;
    __u32 rv = 0;
    __u32 block_offset;
    __u32 check_zone_idx;
    __u64 check_zlba;
    sector_t lsm_pba;
    enum imr_lsm_lookup_result lsm_lookup;
    bool zero_fill = false;

    zlba = zone_idx_lba(zone_idx);

    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    lba = bio->bi_sector;
    block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT;
    lsm_lookup = imr_lsm_read(lba >> IMR_BLOCK_SIZE_SHIFT, &lsm_pba);
    if(lsm_lookup == IMR_LSM_LOOKUP_VALID){
        bio->bi_sector = lsm_pba + ((lba-zlba) % (1 << IMR_BLOCK_SIZE_SHIFT));
        IMRSIM_DATA_LOG("imrsim: IMR-LSM read hit zone %u key=%llu pba=%llu\n",
                        zone_idx,
                        (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT),
                        (unsigned long long)bio->bi_sector);
        lba = bio->bi_sector;
    }else if(lsm_lookup == IMR_LSM_LOOKUP_DELETED){
        IMRSIM_DATA_LOG("imrsim: IMR-LSM read deleted zone %u key=%llu\n",
                        zone_idx,
                        (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT));
        zero_fill = true;
    }else if(zone_status[zone_idx].z_pba_map[block_offset] != -1){
        imr_lsm_record_fallback();
        bio->bi_sector =
            (sector_t)(__u32)zone_status[zone_idx]
                .z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT;
        IMRSIM_DATA_LOG("imrsim: read_ops on zone %u - start lba is %llu, pba is %llu\n",
                        zone_idx, lba, bio->bi_sector);
        lba = bio->bi_sector;
    }else{
        zero_fill = true;
    }
    #else
    lba = bio->bi_iter.bi_sector;
    if(!imrsim_bio_is_internal_io(bio))
    {
        block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT;
        lsm_lookup = imr_lsm_read(lba >> IMR_BLOCK_SIZE_SHIFT, &lsm_pba);
        if(lsm_lookup == IMR_LSM_LOOKUP_VALID){
            bio->bi_iter.bi_sector = lsm_pba
                + ((lba-zlba) % (1 << IMR_BLOCK_SIZE_SHIFT));
            IMRSIM_DATA_LOG("imrsim: IMR-LSM read hit zone %u key=%llu pba=%llu\n",
                            zone_idx,
                            (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT),
                            (unsigned long long)bio->bi_iter.bi_sector);
            lba = bio->bi_iter.bi_sector;
        }else if(lsm_lookup == IMR_LSM_LOOKUP_DELETED){
            IMRSIM_DATA_LOG("imrsim: IMR-LSM read deleted zone %u key=%llu\n",
                            zone_idx,
                            (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT));
            zero_fill = true;
        }else if(zone_status[zone_idx].z_pba_map[block_offset] != -1){
            imr_lsm_record_fallback();
            bio->bi_iter.bi_sector =
                ((sector_t)(__u32)zone_status[zone_idx]
                    .z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT)
                + (lba-zlba)%(1<<IMR_BLOCK_SIZE_SHIFT);
            IMRSIM_DATA_LOG("imrsim: read_ops on zone %u - start LBA is %llu, PBA is %llu\n",
                            zone_idx, lba >> IMR_BLOCK_SIZE_SHIFT,
                            bio->bi_iter.bi_sector >> IMR_BLOCK_SIZE_SHIFT);
            lba = bio->bi_iter.bi_sector;
        }else{
            zero_fill = true;
        }
    }else{
        IMRSIM_DATA_LOG("imrsim DIRECT read option.\n");
    }
    
    #endif
    check_zone_idx = imrsim_lba_zone_idx((sector_t)lba);
    if(check_zone_idx >= IMR_NUMZONES){
        printk(KERN_ERR "imrsim: remapped read lba is out of range. zone_idx: %u\n",
               check_zone_idx);
        imrsim_log_error(bio, IMR_ERR_OUT_RANGE);
        return IMR_ERR_OUT_RANGE;
    }
    check_zlba = zone_idx_lba(check_zone_idx);
    elba = lba + bio_sectors;

    if(elba > (check_zlba + num_sectors_zone())){
        printk(KERN_ERR "imrsim: error: read across zone: %u.%012llx.%08lx\n",
               check_zone_idx, lba, bio_sectors);
        rv++;
        zone_state->stats.zone_stats[check_zone_idx]
            .out_of_policy_read_stats.span_zones_count++;
        imrsim_log_error(bio, IMR_ERR_READ_BORDER);
        if(!policy_flag){
            return IMR_ERR_READ_BORDER;
        }
        printk(KERN_ERR "imrsim:error: out of policy allowed pass\n");
    }
  
    if (imrsim_dbg_log_enabled && printk_ratelimit()) {
        printk(KERN_INFO "imrsim read PASS\n");
    }
    if (rv) {
        printk(KERN_ERR "imrsim: out of policy passed rule violation: %u\n", rv); 
        return IMR_ERR_OUT_OF_POLICY;
    }
    if(zero_fill){
        return IMRSIM_READ_ZERO_FILL;
    }
    return 0;
}

static bool imrsim_ptask_zone_is_queued(__u32 idx)
{
    __u32 qidx;

    for(qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++){
        if(imrsim_ptask.stu_zone_idx[qidx] == idx){
            return true;
        }
    }

    return false;
}

/* Caller must hold imrsim_zone_lock. */
static void imrsim_ptask_queue_zone_status_locked(__u32 idx)
{
    if(WARN_ON_ONCE(!zone_status || idx >= IMR_NUMZONES)){
        return;
    }
    imrsim_ptask.flag |= IMR_STATUS_CHANGE;
    if(imrsim_ptask.stu_zone_idx_cnt == IMR_PSTORE_QDEPTH){
        /* Force a full save rather than lose a distinct dirty zone. */
        imrsim_ptask.stu_zone_idx_gap = IMR_PSTORE_PG_GAP;
    }else if(!imrsim_ptask_zone_is_queued(idx)){
        imrsim_ptask.stu_zone_idx[imrsim_ptask.stu_zone_idx_cnt] = idx;
        imrsim_ptask.stu_zone_idx_cnt++;
    }
}

static bool imrsim_bio_is_discard(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    return bio_op(bio) == REQ_OP_DISCARD;
#else
    return bio->bi_rw & REQ_DISCARD;
#endif
}

static void imrsim_complete_bio(struct bio *bio, int error)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    bio->bi_status = error ? errno_to_blk_status(error) : BLK_STS_OK;
    bio_endio(bio);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
    bio->bi_error = error;
    bio_endio(bio);
#else
    bio_endio(bio, error);
#endif
}

static bool imrsim_bio_has_fua(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    return bio_op(bio) == REQ_OP_WRITE && (bio->bi_opf & REQ_FUA);
#else
    return bio_data_dir(bio) == WRITE && (bio->bi_rw & REQ_FUA);
#endif
}

static int imrsim_bio_copy_from_buffer(struct bio *bio,
                                       unsigned int bio_offset,
                                       const void *buffer,
                                       unsigned int bytes)
{
    unsigned int copied = 0;
    unsigned int skip = bio_offset;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    struct bio_vec *bvec;
    int idx;

    bio_for_each_segment(bvec, bio, idx) {
        unsigned int seg_skip;
        unsigned int seg_len;
        void *addr;

        if(skip >= bvec->bv_len){
            skip -= bvec->bv_len;
            continue;
        }

        seg_skip = skip;
        seg_len = bvec->bv_len - seg_skip;
        if(seg_len > bytes - copied){
            seg_len = bytes - copied;
        }

        addr = kmap_atomic(bvec->bv_page);
        memcpy((char *)addr + bvec->bv_offset + seg_skip,
               (const char *)buffer + copied, seg_len);
        kunmap_atomic(addr);

        copied += seg_len;
        skip = 0;
        if(copied == bytes){
            return 0;
        }
    }
#else
    struct bio_vec bvec;
    struct bvec_iter iter;

    bio_for_each_segment(bvec, bio, iter) {
        unsigned int seg_skip;
        unsigned int seg_len;
        void *addr;

        if(skip >= bvec.bv_len){
            skip -= bvec.bv_len;
            continue;
        }

        seg_skip = skip;
        seg_len = bvec.bv_len - seg_skip;
        if(seg_len > bytes - copied){
            seg_len = bytes - copied;
        }

        addr = kmap_atomic(bvec.bv_page);
        memcpy((char *)addr + bvec.bv_offset + seg_skip,
               (const char *)buffer + copied, seg_len);
        kunmap_atomic(addr);

        copied += seg_len;
        skip = 0;
        if(copied == bytes){
            return 0;
        }
    }
#endif

    return copied == bytes ? 0 : -EIO;
}

static int imrsim_bio_copy_to_buffer(struct bio *bio,
                                     unsigned int bio_offset,
                                     void *buffer,
                                     unsigned int bytes)
{
    unsigned int copied = 0;
    unsigned int skip = bio_offset;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    struct bio_vec *bvec;
    int idx;

    bio_for_each_segment(bvec, bio, idx) {
        unsigned int seg_skip;
        unsigned int seg_len;
        void *addr;

        if(skip >= bvec->bv_len){
            skip -= bvec->bv_len;
            continue;
        }

        seg_skip = skip;
        seg_len = bvec->bv_len - seg_skip;
        if(seg_len > bytes - copied){
            seg_len = bytes - copied;
        }

        addr = kmap_atomic(bvec->bv_page);
        memcpy((char *)buffer + copied,
               (char *)addr + bvec->bv_offset + seg_skip, seg_len);
        kunmap_atomic(addr);

        copied += seg_len;
        skip = 0;
        if(copied == bytes){
            return 0;
        }
    }
#else
    struct bio_vec bvec;
    struct bvec_iter iter;

    bio_for_each_segment(bvec, bio, iter) {
        unsigned int seg_skip;
        unsigned int seg_len;
        void *addr;

        if(skip >= bvec.bv_len){
            skip -= bvec.bv_len;
            continue;
        }

        seg_skip = skip;
        seg_len = bvec.bv_len - seg_skip;
        if(seg_len > bytes - copied){
            seg_len = bytes - copied;
        }

        addr = kmap_atomic(bvec.bv_page);
        memcpy((char *)buffer + copied,
               (char *)addr + bvec.bv_offset + seg_skip, seg_len);
        kunmap_atomic(addr);

        copied += seg_len;
        skip = 0;
        if(copied == bytes){
            return 0;
        }
    }
#endif

    return copied == bytes ? 0 : -EIO;
}

static int imrsim_read_logical_block_page_locked(struct dm_target *ti,
                                                 struct imrsim_c *c,
                                                 sector_t logical_lba,
                                                 struct page *page)
{
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    __u32 zone_idx = logical_lba >> IMR_BLOCK_SIZE_SHIFT >>
                     IMR_ZONE_SIZE_SHIFT;
    __u64 zlba;
    __u32 block_offset;
    sector_t pba = 0;
    sector_t lsm_pba;
    enum imr_lsm_lookup_result lsm_lookup;
    void *page_addr;

    if(zone_idx >= IMR_NUMZONES){
        return IMR_ERR_OUT_RANGE;
    }
    page_addr = page_address(page);
    if(!page_addr){
        return -ENOMEM;
    }
    memset(page_addr, 0, block_sectors << IMR_SECTOR_SIZE_SHIFT_DEFAULT);

    zlba = zone_idx_lba(zone_idx);
    block_offset = (logical_lba - zlba) >> IMR_BLOCK_SIZE_SHIFT;
    if(block_offset >= TOTAL_ITEMS){
        return IMR_ERR_READ_BORDER;
    }

    lsm_lookup = imr_lsm_read(logical_lba >> IMR_BLOCK_SIZE_SHIFT, &lsm_pba);
    if(lsm_lookup == IMR_LSM_LOOKUP_VALID){
        pba = lsm_pba;
    }else if(lsm_lookup == IMR_LSM_LOOKUP_DELETED){
        return 0;
    }else if(zone_status[zone_idx].z_pba_map[block_offset] != -1){
        imr_lsm_record_fallback();
        pba = (sector_t)(__u32)zone_status[zone_idx]
                  .z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT;
    }else{
        return 0;
    }

    return imrsim_read_data_page(
        c->dev->bdev, imrsim_map_sector(ti, pba),
        block_sectors << IMR_SECTOR_SIZE_SHIFT_DEFAULT, page);
}

static int imrsim_write_back_overlap_pages_locked(struct dm_target *ti,
                                                  struct imrsim_c *c,
                                                  struct page **pages,
                                                  __u8 page_count)
{
    __u8 idx;
    int ret = 0;

    for(idx = 0; idx < page_count; idx++){
        ret = imrsim_write_data_page(c->dev->bdev,
                                     imrsim_map_sector(ti,
                                         imrsim_rmw_task.lba[idx]),
                                     PAGE_SIZE, pages[idx]);
        if(ret){
            break;
        }
    }

    return ret;
}

static int imrsim_write_full_block_page_locked(struct dm_target *ti,
                                               struct imrsim_c *c,
                                               sector_t logical_lba,
                                               struct page *page,
                                               int policy_wflag)
{
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    unsigned int block_bytes =
        block_sectors << IMR_SECTOR_SIZE_SHIFT_DEFAULT;
    __u32 zone_idx = logical_lba >> IMR_BLOCK_SIZE_SHIFT >>
                     IMR_ZONE_SIZE_SHIFT;
    struct bio *wbio;
    struct page *overlap_pages[2] = { NULL, NULL };
    __u8 overlap_count = 0;
    bool append_reserved = false;
    int ret;
    __u8 idx;
    sector_t remapped_lba;

    if(zone_idx >= IMR_NUMZONES){
        return IMR_ERR_OUT_RANGE;
    }
    wbio = bio_alloc(GFP_NOIO, 1);
    if(!wbio){
        return -ENOMEM;
    }
    wbio->bi_bdev = c->dev->bdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    wbio->bi_sector = logical_lba;
#else
    wbio->bi_iter.bi_sector = logical_lba;
#endif
    if(bio_add_page(wbio, page, block_bytes, 0) != block_bytes){
        printk(KERN_ERR "imrsim: partial RMW full-block bio_add_page failed lba=%llu bytes=%u\n",
               (unsigned long long)logical_lba, block_bytes);
        bio_put(wbio);
        return -EIO;
    }

    ret = imrsim_active_write_rule_check(wbio, zone_idx, block_sectors,
                                         policy_wflag);
    if(ret < 0){
        printk(KERN_ERR "imrsim: partial RMW write rule failed lba=%llu ret=%d\n",
               (unsigned long long)logical_lba, ret);
        bio_put(wbio);
        return ret;
    }
    append_reserved = true;
    atomic_inc(&imr_lsm_append_writes_inflight);

    if(ret > 0){
        overlap_count = imrsim_rmw_task.lba_num;
        if(overlap_count > 2){
            ret = -EIO;
            goto out_free_overlap;
        }
        for(idx = 0; idx < overlap_count; idx++){
            overlap_pages[idx] = alloc_page(GFP_NOIO);
            if(!overlap_pages[idx]){
                ret = -ENOMEM;
                goto out_free_overlap;
            }
            ret = imrsim_read_data_page(c->dev->bdev,
                                        imrsim_map_sector(ti,
                                            imrsim_rmw_task.lba[idx]),
                                        PAGE_SIZE, overlap_pages[idx]);
            if(ret){
                printk(KERN_ERR "imrsim: partial RMW overlap read failed lba=%llu ret=%d\n",
                       (unsigned long long)imrsim_rmw_task.lba[idx],
                       ret);
                goto out_free_overlap;
            }
        }
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    remapped_lba = wbio->bi_sector;
#else
    remapped_lba = wbio->bi_iter.bi_sector;
#endif

    ret = imrsim_write_data_page(c->dev->bdev,
                                 imrsim_map_sector(ti, remapped_lba),
                                 block_bytes, page);
    if(ret){
        printk(KERN_ERR "imrsim: partial RMW full-block write failed logical_lba=%llu pba=%llu ret=%d\n",
               (unsigned long long)logical_lba,
               (unsigned long long)remapped_lba,
               ret);
    }
    if(!ret && overlap_count){
        ret = imrsim_write_back_overlap_pages_locked(ti, c, overlap_pages,
                                                     overlap_count);
    }
    if(!ret){
        imrsim_ptask_queue_zone_status_locked(zone_idx);
    }

out_free_overlap:
    for(idx = 0; idx < overlap_count; idx++){
        if(overlap_pages[idx]){
            __free_page(overlap_pages[idx]);
        }
    }
    imrsim_rmw_task.lba_num = 0;
    if(append_reserved &&
       atomic_dec_and_test(&imr_lsm_append_writes_inflight) &&
       READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
        imr_lsm_queue_zone_compaction_auto_work();
    }
    bio_put(wbio);
    return ret;
}

static int imrsim_partial_data_io_locked(struct dm_target *ti,
                                         struct bio *bio,
                                         sector_t lba,
                                         sector_t bio_sectors,
                                         int cdir,
                                         int policy_wflag,
                                         bool zero_fill)
{
    struct imrsim_c *c = ti->private;
    sector_t block_sectors = (sector_t)1 << IMR_BLOCK_SIZE_SHIFT;
    unsigned int sector_bytes = 1U << IMR_SECTOR_SIZE_SHIFT_DEFAULT;
    sector_t done = 0;
    int ret = 0;

    if(cdir != READ && cdir != WRITE){
        return -EOPNOTSUPP;
    }

    while(done < bio_sectors){
        sector_t current_lba = lba + done;
        sector_t block_lba = current_lba & ~(block_sectors - 1);
        sector_t block_offset = current_lba - block_lba;
        sector_t span = block_sectors - block_offset;
        unsigned int bio_byte_offset;
        unsigned int block_byte_offset;
        unsigned int bytes;
        struct page *page;
        void *page_addr;

        if(span > bio_sectors - done){
            span = bio_sectors - done;
        }

        page = alloc_page(GFP_NOIO);
        if(!page){
            ret = -ENOMEM;
            break;
        }
        page_addr = page_address(page);
        if(!page_addr){
            __free_page(page);
            ret = -ENOMEM;
            break;
        }

        if(zero_fill){
            memset(page_addr, 0, PAGE_SIZE);
        }else{
            ret = imrsim_read_logical_block_page_locked(ti, c, block_lba,
                                                        page);
            if(ret){
                printk(KERN_ERR "imrsim: partial RMW read old block failed block_lba=%llu ret=%d\n",
                       (unsigned long long)block_lba, ret);
                __free_page(page);
                break;
            }
        }

        bio_byte_offset = (unsigned int)(done * sector_bytes);
        block_byte_offset = (unsigned int)(block_offset * sector_bytes);
        bytes = (unsigned int)(span * sector_bytes);

        if(cdir == READ){
            ret = imrsim_bio_copy_from_buffer(
                bio, bio_byte_offset,
                (char *)page_addr + block_byte_offset, bytes);
            if(ret){
                printk(KERN_ERR "imrsim: partial RMW copy to read bio failed lba=%llu bytes=%u ret=%d\n",
                       (unsigned long long)current_lba, bytes, ret);
            }
        }else{
            ret = imrsim_bio_copy_to_buffer(
                bio, bio_byte_offset,
                (char *)page_addr + block_byte_offset, bytes);
            if(!ret){
                ret = imrsim_write_full_block_page_locked(
                    ti, c, block_lba, page, policy_wflag);
                if(ret){
                    printk(KERN_ERR "imrsim: partial RMW write merged block failed block_lba=%llu ret=%d\n",
                           (unsigned long long)block_lba, ret);
                }
            }else{
                printk(KERN_ERR "imrsim: partial RMW copy from write bio failed lba=%llu bytes=%u ret=%d\n",
                       (unsigned long long)current_lba, bytes, ret);
            }
        }

        __free_page(page);
        if(ret){
            break;
        }
        done += span;
    }

    return ret;
}

static void imrsim_partial_data_io_work(struct work_struct *work)
{
    struct imrsim_partial_io_task *task =
        container_of(work, struct imrsim_partial_io_task, work);
    __u64 work_start_ns = imrsim_diag_now_ns();
    __u64 work_duration_ns;
    int ret;

    if(task->queued_at_ns && work_start_ns >= task->queued_at_ns){
        imrsim_diag_record_duration(
            &imrsim_diag.partial_io_queue_wait_count,
            &imrsim_diag.partial_io_queue_wait_total_ns,
            &imrsim_diag.partial_io_queue_wait_max_ns,
            NULL, work_start_ns - task->queued_at_ns);
    }

    IMRSIM_DATA_LOG("imrsim: partial RMW worker start lba=%llu sectors=%llu\n",
                    (unsigned long long)task->lba,
                    (unsigned long long)task->bio_sectors);

retry_zone_lock:
    imrsim_diag_timed_mutex_lock(
        &imrsim_zone_lock,
        &imrsim_diag.foreground_zone_lock_wait_count,
        &imrsim_diag.foreground_zone_lock_wait_total_ns,
        &imrsim_diag.foreground_zone_lock_wait_max_ns);
    if(!imrsim_target_ready_locked()){
        ret = -ENODEV;
    }else if(imr_lsm_zone_copy_overlaps_locked(task->lba,
                                               task->bio_sectors)){
        mutex_unlock(&imrsim_zone_lock);
        wait_event(imr_lsm_zone_copy_wait,
                   imr_lsm_zone_copy_wait_done(task->lba,
                                               task->bio_sectors));
        goto retry_zone_lock;
    }else{
        ret = imrsim_partial_data_io_locked(task->ti, task->bio,
                                            task->lba,
                                            task->bio_sectors,
                                            task->cdir,
                                            task->policy_wflag,
                                            task->zero_fill);
    }
    mutex_unlock(&imrsim_zone_lock);

    work_duration_ns = imrsim_diag_elapsed_ns(work_start_ns);
    imrsim_diag_record_duration(&imrsim_diag.partial_io_count,
                                &imrsim_diag.partial_io_total_ns,
                                &imrsim_diag.partial_io_max_ns,
                                &imrsim_diag.last_partial_io_ns,
                                work_duration_ns);
    if(task->cdir == READ){
        atomic64_inc(&imrsim_diag.partial_read_count);
    }else{
        imrsim_diag_record_duration(&imrsim_diag.partial_rmw_count,
                                    &imrsim_diag.partial_rmw_total_ns,
                                    &imrsim_diag.partial_rmw_max_ns,
                                    &imrsim_diag.last_partial_rmw_ns,
                                    work_duration_ns);
    }

    if(ret){
        printk(KERN_ERR "imrsim: partial RMW worker failed lba=%llu sectors=%llu ret=%d\n",
               (unsigned long long)task->lba,
               (unsigned long long)task->bio_sectors,
               ret);
    }else{
        IMRSIM_DATA_LOG("imrsim: partial RMW worker done lba=%llu sectors=%llu\n",
                        (unsigned long long)task->lba,
                        (unsigned long long)task->bio_sectors);
    }

    imrsim_complete_bio(task->bio, ret);
    kfree(task);
}

static int imrsim_queue_partial_data_io(struct dm_target *ti,
                                        struct bio *bio,
                                        sector_t lba,
                                        sector_t bio_sectors,
                                        int cdir,
                                        int policy_wflag,
                                        bool zero_fill)
{
    struct imrsim_partial_io_task *task;

    if(!imrsim_partial_io_wq){
        return -ENODEV;
    }

    task = kzalloc(sizeof(*task), GFP_NOIO);
    if(!task){
        return -ENOMEM;
    }

    INIT_WORK(&task->work, imrsim_partial_data_io_work);
    task->ti = ti;
    task->bio = bio;
    task->queued_at_ns = imrsim_diag_now_ns();
    task->lba = lba;
    task->bio_sectors = bio_sectors;
    task->cdir = cdir;
    task->policy_wflag = policy_wflag;
    task->zero_fill = zero_fill;

    queue_work(imrsim_partial_io_wq, &task->work);
    return 0;
}

/* I/O mapping */
int imrsim_map(struct dm_target *ti, struct bio *bio)
{
    struct imrsim_c *c = ti->private;
    struct imrsim_io_context *io =
        dm_per_bio_data(bio, IMRSIM_DM_PER_BIO_DATA_SIZE(ti));
    int cdir = bio_data_dir(bio);     
    bool is_discard = imrsim_bio_is_discard(bio);
    bool incoming_fua = imrsim_bio_has_fua(bio);

    if(bio){
        IMRSIM_DATA_LOG("imrsim_map: the bio has %u sectors.\n",
                        bio_sectors(bio));
    }

    sector_t bio_sectors = bio_sectors(bio);
    int policy_rflag = 0;
    int policy_wflag = 0;
    int ret = 0;
    unsigned int penalty;
    __u32 zone_idx;
    __u64 lba;

    io->append_reserved = 0;

    if(incoming_fua){
        atomic64_inc(&imrsim_diag.incoming_fua_write_count);
    }
retry_zone_lock:
    imrsim_diag_timed_mutex_lock(
        &imrsim_zone_lock,
        &imrsim_diag.foreground_zone_lock_wait_count,
        &imrsim_diag.foreground_zone_lock_wait_total_ns,
        &imrsim_diag.foreground_zone_lock_wait_max_ns);
    //printk(KERN_INFO "zone_lock.\n");
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    zone_idx = bio->bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_sector;
    #else
    zone_idx = bio->bi_iter.bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_iter.bi_sector;
    #endif

    //printk(KERN_INFO "imrsim: map- lba is %llu\n", lba);

    if(!imrsim_target_ready_locked()){
        ret = -ENODEV;
        goto nomap;
    }
    if((bio_sectors || is_discard) &&
       imr_lsm_zone_copy_overlaps_locked((sector_t)lba, bio_sectors)){
        mutex_unlock(&imrsim_zone_lock);
        wait_event(imr_lsm_zone_copy_wait,
                   imr_lsm_zone_copy_wait_done((sector_t)lba,
                                               bio_sectors));
        goto retry_zone_lock;
    }
    /*
     * Flush bios carry no data and therefore have no 4K mapping record.
     * Forward them directly instead of sending them through the write rule.
     */
    if(!bio_sectors && !is_discard){
        atomic64_inc(&imrsim_diag.flush_bio_count);
        bio->bi_bdev = c->dev->bdev;
        goto mapped;
    }
    imrsim_dev_idle_update();
    imrsim_ptask.flag |= IMR_STATS_CHANGE;

    if(IMR_NUMZONES <= zone_idx){
        printk(KERN_ERR "imrsim: lba is out of range. zone_idx: %u\n", zone_idx);
        imrsim_log_error(bio, IMR_ERR_OUT_RANGE);
        goto nomap;
    }
    if(imrsim_dbg_log_enabled){
        printk(KERN_DEBUG "imrsim: %s bio_sectors=%llu\n", __FUNCTION__, 
                (unsigned long long)bio_sectors);
    }
    if(lba < ti->begin || bio_sectors > ti->len ||
       lba - ti->begin > ti->len - bio_sectors){
        printk(KERN_ERR "imrsim: error: %s bio range is outside target\n",
               __FUNCTION__);
        imrsim_log_error(bio, IMR_ERR_OUT_OF_POLICY);
        goto nomap;
    }
    bio->bi_bdev = c->dev->bdev;
    policy_rflag = zone_state->config.dev_config.out_of_policy_read_flag;
    policy_wflag = zone_state->config.dev_config.out_of_policy_write_flag;

    if(is_discard){
        ret = imrsim_lsm_discard_lba_range_locked((sector_t)lba,
                                                  bio_sectors);
        if(ret){
            printk(KERN_ERR "imrsim: discard delete failed lba=%llu sectors=%llu ret=%d\n",
                   lba, (unsigned long long)bio_sectors, ret);
            imrsim_log_error(bio, IMR_ERR_OUT_OF_POLICY);
            goto nomap;
        }

        mutex_unlock(&imrsim_zone_lock);
        imrsim_complete_bio(bio, 0);
        return DM_MAPIO_SUBMITTED;
    }

    if(bio_sectors != ((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) ||
       (lba & (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1))){
        if(cdir == WRITE){
            zone_state->stats.zone_stats[zone_idx]
                .out_of_policy_write_stats.unaligned_count++;
        }
        IMRSIM_DATA_LOG("imrsim: partial data bio queued for 4K RMW lba=%llu sectors=%llu\n",
                        lba, (unsigned long long)bio_sectors);
        ret = imrsim_queue_partial_data_io(ti, bio, (sector_t)lba,
                                           bio_sectors, cdir,
                                           policy_wflag, false);
        mutex_unlock(&imrsim_zone_lock);
        if(ret){
            imrsim_complete_bio(bio, ret);
        }
        return DM_MAPIO_SUBMITTED;
    }
    
    // read or write ?
    if(cdir == WRITE){
        if(imrsim_dbg_log_enabled){
            printk(KERN_DEBUG "imrsim: %s WRITE %u.%012llx:%08lx.\n", __FUNCTION__,
                zone_idx, lba, bio_sectors);
        }
        ret = imrsim_active_write_rule_check(bio, zone_idx, bio_sectors,
                                             policy_wflag);
        if(ret<0){
            /* Allocator failures cannot safely fall through to logical-LBA
             * passthrough: that would violate append-only placement. */
            goto nomap;
        }
        if(ret>0){
            goto submitted;
        }
        io->append_reserved = 1;
        atomic_inc(&imr_lsm_append_writes_inflight);
        imrsim_ptask_queue_zone_status_locked(zone_idx);
    }
    else if(cdir == READ){
        if (imrsim_dbg_log_enabled) {
            printk(KERN_DEBUG "imrsim: %s READ %u.%012llx:%08lx.\n", __FUNCTION__,
                    zone_idx, lba, bio_sectors);
        }
        ret = imrsim_read_rule_check(bio, zone_idx, bio_sectors, policy_rflag);
        /* Buffered partial writes may first read a complete, unmapped page. */
        if(ret == IMRSIM_READ_ZERO_FILL){
            ret = imrsim_queue_partial_data_io(ti, bio, (sector_t)lba,
                                               bio_sectors, READ,
                                               policy_wflag, true);
            mutex_unlock(&imrsim_zone_lock);
            if(ret){
                imrsim_complete_bio(bio, ret);
            }
            return DM_MAPIO_SUBMITTED;
        }
        if(ret){
            if(policy_wflag == 1 && policy_rflag == 1){
                printk(KERN_ERR "imrsim: out of policy read passthrough applied\n");
                goto mapped;
            }
            penalty = 0;
            if(policy_rflag == 1){
                penalty = zone_state->config.dev_config.r_time_to_rmw_zone;
                if(printk_ratelimit()){
                    printk(KERN_ERR "imrsim:%s: read error passed: out of policy read flagged on\n", 
                  __FUNCTION__);
                }
                udelay(penalty);
            }else{
                goto nomap;
            }
        }
    }
    mapped:
    if(incoming_fua && bio_sectors){
        atomic64_inc(&imrsim_diag.fua_write_count);
    }
    if (bio_sectors(bio))
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        bio->bi_sector =  imrsim_map_sector(ti,bio->bi_sector);
    #else
        bio->bi_iter.bi_sector =  imrsim_map_sector(ti, bio->bi_iter.bi_sector);
    #endif
    mutex_unlock(&imrsim_zone_lock);
    //printk(KERN_INFO "zone_unlock.\n");
    return DM_MAPIO_REMAPPED;

    submitted:
    IMRSIM_DATA_LOG("imrsim_map: submitted and conduct rmw!\n");
    imrsim_rmw_task.bio = bio;
    imrsim_rmw_thread(ti);
    mutex_unlock(&imrsim_zone_lock);
    IMRSIM_DATA_LOG("imrsim_map: end rmw!\n");
    return DM_MAPIO_SUBMITTED;

nomap:
    mutex_unlock(&imrsim_zone_lock);
    //printk(KERN_INFO "zone_unlock.\n");
    return IMR_DM_IO_ERR;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static int imrsim_end_io(struct dm_target *ti, struct bio *bio,
                         blk_status_t *error)
{
    struct imrsim_io_context *io =
        dm_per_bio_data(bio, IMRSIM_DM_PER_BIO_DATA_SIZE(ti));
    bool append_idle = false;

    (void)error;

    if(io->append_reserved){
        io->append_reserved = 0;
        append_idle = atomic_dec_and_test(
            &imr_lsm_append_writes_inflight);
    }
    if(append_idle &&
       READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    return 0;
}
#else
static int imrsim_end_io(struct dm_target *ti, struct bio *bio, int error)
{
    struct imrsim_io_context *io =
        dm_per_bio_data(bio, IMRSIM_DM_PER_BIO_DATA_SIZE(ti));
    bool append_idle = false;

    (void)error;

    if(io->append_reserved){
        io->append_reserved = 0;
        append_idle = atomic_dec_and_test(
            &imr_lsm_append_writes_inflight);
    }
    if(append_idle &&
       READ_ONCE(imr_lsm_meta.zone_compaction_auto_pending)){
        imr_lsm_queue_zone_compaction_auto_work();
    }

    return 0;
}
#endif

/* Device status query */
static void imrsim_status(struct dm_target* ti, 
                          status_type_t type,
                          unsigned status_flags,
                          char* result,
                          unsigned maxlen)
{
   struct imrsim_c* c   = ti->private;

   switch(type)
   {
      case STATUSTYPE_INFO:
         result[0] = '\0';
         break;

      case STATUSTYPE_TABLE:
         snprintf(result, maxlen, "%s %llu", c->dev->name,
	    (unsigned long long)c->start);
         break;
   }
}

/* Present zone status information */
static void imrsim_list_zone_status(struct imrsim_zone_status *ptr, 
                                    __u32 num_zones, int criteria)
{
   __u32 i = 0;
   printk(KERN_DEBUG "\nQuery ceiteria: %d\n", criteria);
   printk(KERN_DEBUG "List zone status of %u zones:\n\n", num_zones);
   for (i = 0; i < num_zones; i++) {
       printk(KERN_DEBUG "zone index        : %lu\n", (long unsigned)ptr[i].z_start);
       printk(KERN_DEBUG "zone length       : %u\n",  ptr[i].z_length);
       printk(KERN_DEBUG "zone type         : 0x%x\n", ptr[i].z_type);
       printk(KERN_DEBUG "zone condition    : 0x%x\n", ptr[i].z_conds);
       printk(KERN_DEBUG "\n");
   }
}

/* Query zone status information and record the result in ptr */
int imrsim_query_zones(sector_t lba, int criteria,
                       __u32 *num_zones, struct imrsim_zone_status *ptr)
{
    int idx32;
    __u32 num32;
    __u32 zone_idx;
    __u64 zone_idx64;

    if(!num_zones || !ptr){
        printk(KERN_ERR "imrsim: NULL pointer passed through.\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        return -ENODEV;
    }
    zone_idx64 = (__u64)lba >> IMR_BLOCK_SIZE_SHIFT >>
                 IMR_ZONE_SIZE_SHIFT;
    if(zone_idx64 >= IMR_NUMZONES || !*num_zones ||
       *num_zones > IMR_NUMZONES - (__u32)zone_idx64){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: number of zone out of range\n");
        return -EINVAL;
    }
    zone_idx = (__u32)zone_idx64;
    if (imrsim_dbg_log_enabled) {   
        imrsim_list_zone_status(zone_status, *num_zones, criteria);
    }
    if(criteria > 0){
        idx32 = 0; 
        for (num32 = 0; num32 < *num_zones; num32++) {
            memcpy((ptr + idx32), &zone_status[zone_idx + num32], 
                sizeof(struct imrsim_zone_status));
            idx32++;
        }
        *num_zones = idx32;
        mutex_unlock(&imrsim_zone_lock);
        return 0;  
    }
    switch(criteria){
        case ZONE_MATCH_ALL:
            memcpy(ptr, &zone_status[zone_idx], *num_zones * 
                sizeof(struct imrsim_zone_status));
            break;
        case ZONE_MATCH_FULL:
            idx32 = 0; 
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_FULL == zone_status[num32].z_conds) {
                    memcpy((ptr + idx32), &zone_status[num32], 
                            sizeof(struct imrsim_zone_status));
                    idx32++;
                    if (idx32 == *num_zones) {
                        break;
                    }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_NFULL:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_FREE:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if ((Z_COND_EMPTY == zone_status[num32].z_conds)) {
                    memcpy((ptr + idx32), &zone_status[num32], 
                            sizeof(struct imrsim_zone_status));
                    idx32++;
                    if (idx32 == *num_zones) {
                        break;
                    }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_RNLY:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_RO == zone_status[num32].z_conds) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_OFFL:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_OFFLINE == zone_status[num32].z_conds) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
                }
            }
            *num_zones = idx32;
            break;
        default:
            printk("imrsim: wrong query parameter\n");
    }
    mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_query_zones);

/* The ioctl interface method implements specific interface functions. */
int imrsim_ioctl(struct dm_target *ti,
                 unsigned int cmd,
                 unsigned long arg)
{
    imrsim_zbc_query          *zbc_query;
    struct imrsim_dev_config   pconf;
    //struct imrsim_zone_status  pstatus;
    struct imrsim_stats       *pstats;
    int                        ret = 0;
    __u32                      size  = 0;
    __u64                      num64;
    __u32                      param = 0;
#ifdef BLKDISCARD
    __u64                      discard_range[2];
    sector_t                   discard_lba;
    sector_t                   discard_sectors;
#endif
    
    mutex_lock(&imrsim_ioctl_lock);
    mutex_lock(&imrsim_zone_lock);
    if(!imrsim_target_ready_locked()){
        mutex_unlock(&imrsim_zone_lock);
        goto ioerr;
    }
    imrsim_dev_idle_update();
    imrsim_ptask.flag |= IMR_STATS_CHANGE;
    mutex_unlock(&imrsim_zone_lock);
    switch(cmd)
    {
        case IOCTL_IMRSIM_GET_LAST_RERROR:
            if(imrsim_get_last_rd_error(&param)){
                printk(KERN_ERR "imrsim: get last rd error failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy last rd error to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_LAST_WERROR:
            if(imrsim_get_last_wd_error(&param)){
                printk(KERN_ERR "imrsim: get last wd error failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy last wd error to user memory failed\n");
                goto ioerr;
            }
            break;
        /* zone ioctl */
        case IOCTL_IMRSIM_SET_LOGENABLE:
            if(imrsim_set_log_enable(1)){
                printk(KERN_ERR "imrsim: enable log failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_LOGDISABLE:
            if(imrsim_set_log_enable(0)){
                printk(KERN_ERR "imrsim: disable log failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_LSM_DELETE_KEY:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&num64, (__u64 *)arg, sizeof(__u64))){
                printk(KERN_ERR "imrsim: delete key copy from user failed\n");
                goto ioerr;
            }
            if(imrsim_lsm_delete_key(num64)){
                printk(KERN_ERR "imrsim: delete key failed key=%llu\n",
                       (unsigned long long)num64);
                goto ioerr;
            }
            break;
#ifdef BLKDISCARD
        case BLKDISCARD:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad discard parameter\n");
                goto ioerr;
            }
            if(copy_from_user(discard_range, (__u64 *)arg,
                              sizeof(discard_range))){
                printk(KERN_ERR "imrsim: discard range copy from user failed\n");
                goto ioerr;
            }
            if((discard_range[0] &
                ((1ULL << (IMR_SECTOR_SIZE_SHIFT_DEFAULT +
                           IMR_BLOCK_SIZE_SHIFT_DEFAULT)) - 1)) ||
               (discard_range[1] &
                ((1ULL << (IMR_SECTOR_SIZE_SHIFT_DEFAULT +
                           IMR_BLOCK_SIZE_SHIFT_DEFAULT)) - 1))){
                printk(KERN_ERR "imrsim: discard range is not 4K aligned offset=%llu length=%llu\n",
                       (unsigned long long)discard_range[0],
                       (unsigned long long)discard_range[1]);
                goto ioerr;
            }

            discard_lba = (sector_t)(discard_range[0] >>
                                      IMR_SECTOR_SIZE_SHIFT_DEFAULT);
            discard_sectors = (sector_t)(discard_range[1] >>
                                         IMR_SECTOR_SIZE_SHIFT_DEFAULT);
            mutex_lock(&imrsim_zone_lock);
            ret = imrsim_lsm_discard_lba_range_locked(discard_lba,
                                                      discard_sectors);
            if(ret){
                mutex_unlock(&imrsim_zone_lock);
                printk(KERN_ERR "imrsim: discard ioctl delete failed lba=%llu sectors=%llu ret=%d\n",
                       (unsigned long long)discard_lba,
                       (unsigned long long)discard_sectors,
                       ret);
                goto ioerr;
            }
            mutex_unlock(&imrsim_zone_lock);
            break;
#endif
        case IOCTL_IMRSIM_GET_NUMZONES:
            if(imrsim_get_num_zones(&param)){
                printk(KERN_ERR "imrsim: get number of zones failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy num of zones to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_SIZZONEDEFAULT:
            if(imrsim_get_size_zone_default(&param)){
                printk(KERN_ERR "imrsim: get zone size failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy zone size to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_SIZZONEDEFAULT:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&param, (__u32 *)arg, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: set zone size copy from user failed\n");
                goto ioerr;
            }
            if(imrsim_set_size_zone_default(param)){
                printk(KERN_ERR "imrsim: set default zone size failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_RESET_ZONE:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&num64, (__u64 *)arg, sizeof(__u64) )){
                printk(KERN_ERR "imrsim: reset zone write pointer copy from user memory failed\n");
                goto ioerr;
            }
            if(imrsim_blkdev_reset_zone_ptr(num64)){
                printk(KERN_ERR "imrsim: reset zone write pointer failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_QUERY:
            zbc_query = kzalloc(sizeof(imrsim_zbc_query), GFP_KERNEL);
            if(!zbc_query){
                printk(KERN_ERR "imrsim: %s no enough memory for zbc query\n", __FUNCTION__);
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto zfail;
            }
            ret = copy_from_user(zbc_query, (imrsim_zbc_query *)arg, sizeof(imrsim_zbc_query));
            if(ret){
                printk(KERN_ERR "imrsim: %s copy from user for zbc query failed\n", __FUNCTION__);
                goto zfail;
            }
            if (zbc_query->num_zones == 0 || zbc_query->num_zones > IMR_NUMZONES) {
                printk(KERN_ERR "imrsim: Wrong parameter for the number of zones\n");
                goto zfail;
            }
            size = sizeof(imrsim_zbc_query) + sizeof(struct imrsim_zone_status) *
                  (zbc_query->num_zones - 1);
            zbc_query = krealloc(zbc_query, size, GFP_KERNEL);
            if (!zbc_query) {
                printk(KERN_ERR "imrsim: %s no enough emeory for zbc query\n", __FUNCTION__);
                goto zfail;
            } 
            if (imrsim_query_zones(zbc_query->lba, zbc_query->criteria, 
                &zbc_query->num_zones, zbc_query->ptr)) {
                printk(KERN_ERR "imrsim: %s query zone status failed\n", __FUNCTION__);
                goto zfail;            
            }
            if(copy_to_user((__u32 *)arg, zbc_query, size)){
                    printk(KERN_ERR "imrsim: %s copy to user for zbc query failed\n", __FUNCTION__);
                    goto zfail;
            }
            kfree(zbc_query);
            break;
        zfail:
            kfree(zbc_query);
            break;
        /* IMRSIM stats IOCTLs */
        case IOCTL_IMRSIM_GET_STATS:
            size = imrsim_stats_size();
            pstats = (struct imrsim_stats *)kzalloc(size, GFP_ATOMIC);
            if(!pstats){
                printk(KERN_ERR "imrsim: no enough memory to hold stats\n");
                goto ioerr;
            }
            if(imrsim_get_stats(pstats)){
                printk(KERN_ERR "imrsim: get stats failed\n");
                kfree(pstats);
                goto sfail;
            }
            if(imrsim_dbg_log_enabled){
                imrsim_report_stats(pstats);
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto sfail;
            }
            if(copy_to_user((struct imrsim_stats *)arg, pstats, size)){
                printk(KERN_ERR "imrsim: get stats failed as insufficient user memory\n");
                kfree(pstats);
                goto sfail;
            }
            kfree(pstats);
            break;
        sfail:
            kfree(pstats);
            break;
        case IOCTL_IMRSIM_RESET_STATS:
            if(imrsim_reset_stats()){
                printk(KERN_ERR "imrsim: reset stats failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_RESET_ZONESTATS:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&num64, (__u64 *)arg, sizeof(__u64) )){
                printk(KERN_ERR "imrsim: copy reset zone lba from user memory failed\n");
                goto ioerr;
            }
            if(imrsim_reset_zone_stats(num64)){
                printk(KERN_ERR "imrsim: reset zone stats on lba failed");
                goto ioerr;
            }
            break;
        /* IMRSIM config IOCTLs */
        case IOCTL_IMRSIM_RESET_DEFAULTCONFIG:
            if(imrsim_reset_default_config()){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_RESET_ZONECONFIG:
            if(imrsim_reset_default_zone_config()){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_RESET_DEVCONFIG:
            if(imrsim_reset_default_device_config()){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_DEVCONFIG:
            if(imrsim_get_device_config(&pconf)){
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((struct imrsim_dev_config*)arg, &pconf, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_DEVRCONFIG_DELAY:
            if ((__u64)arg == 0) {
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr; 
            }
            if(copy_from_user(&pconf, (struct imrsim_dev_config *)arg, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            if(imrsim_set_device_rconfig_delay(&pconf)){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_DEVWCONFIG_DELAY:
            if ((__u64)arg == 0) {
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr; 
            }
            if(copy_from_user(&pconf, (struct imrsim_dev_config *)arg, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            if(imrsim_set_device_wconfig_delay(&pconf)){
                goto ioerr;
            }
            break;
        default:
            break;
    }
    mutex_unlock(&imrsim_ioctl_lock);
    return 0;
    ioerr:
    mutex_unlock(&imrsim_ioctl_lock);
    return -EFAULT;
}

/* To merge requests. */
static int imrsim_merge(struct dm_target* ti, 
                        struct bvec_merge_data* bvm,
                        struct bio_vec* biovec, 
                        int max_size)
{
   struct imrsim_c*      c = ti->private;
   struct request_queue* q = bdev_get_queue(c->dev->bdev);

   if (!q->merge_bvec_fn)
      return max_size;

   bvm->bi_bdev   = c->dev->bdev;
   bvm->bi_sector = imrsim_map_sector(ti, bvm->bi_sector);

   return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

/* iterate devices */
static int imrsim_iterate_devices(struct dm_target *ti,
                                  iterate_devices_callout_fn fn,
                                  void *data)
{
   struct imrsim_c* c = ti->private;

   return fn(ti, c->dev, c->start, ti->len, data);
}

static void imrsim_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
   unsigned int block_bytes =
       1U << (IMR_BLOCK_SIZE_SHIFT_DEFAULT +
              IMR_SECTOR_SIZE_SHIFT_DEFAULT);

   (void)ti;

   /*
    * IMR-LSM still stores metadata at 4 KiB key granularity, but the target
    * handles sub-4 KiB head/tail data I/O with synchronous RMW.  Keep the
    * logical block at the sector size while advertising the preferred physical
    * and minimum I/O size.
    */
   limits->logical_block_size =
       max(limits->logical_block_size,
           1U << IMR_SECTOR_SIZE_SHIFT_DEFAULT);
   limits->physical_block_size =
       max(limits->physical_block_size, block_bytes);
   limits->io_min = max(limits->io_min, block_bytes);

   /*
    * IMR-LSM consumes discard as logical metadata updates. Advertise virtual
    * discard limits so blkdiscard/fstrim can reach the target even if the
    * backing device has no native discard support. Keep ranges bounded because
    * tombstones are appended synchronously while the global zone lock is held.
    */
   limits->max_discard_sectors =
       IMR_MAX_DISCARD_BLOCKS << IMR_BLOCK_SIZE_SHIFT_DEFAULT;
   limits->discard_granularity = block_bytes;
   limits->discard_alignment = 0;
   limits->discard_zeroes_data = 0;
}

/* Core structure - represents the target-driven plug-in, 
and the structure collects the function entry for the functions implemented by the driver plug-in */
static struct target_type imrsim_target = 
{
    .name            = "imrsim",
    .version         = {1, 1, 1},
    .module          = THIS_MODULE,
    .ctr             = imrsim_ctr,
    .dtr             = imrsim_dtr,
    .map             = imrsim_map,
    .end_io          = imrsim_end_io,
    .status          = imrsim_status,
    .ioctl           = imrsim_ioctl,
    .merge           = imrsim_merge,
    .iterate_devices = imrsim_iterate_devices,
    .io_hints        = imrsim_io_hints
};

/* init IMRSim module */
static int __init dm_imrsim_init(void)
{
    int ret = 0;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    imr_lsm_zone_compaction_wq =
        alloc_workqueue("imrsim_lsm_auto", WQ_MEM_RECLAIM, 1);
    if(!imr_lsm_zone_compaction_wq){
        return -ENOMEM;
    }
    imr_lsm_level_compaction_wq =
        alloc_workqueue("imrsim_lsm_level", WQ_MEM_RECLAIM, 1);
    if(!imr_lsm_level_compaction_wq){
        destroy_workqueue(imr_lsm_zone_compaction_wq);
        imr_lsm_zone_compaction_wq = NULL;
        return -ENOMEM;
    }
    imrsim_partial_io_wq =
        alloc_workqueue("imrsim_partial_io", WQ_MEM_RECLAIM, 1);
    if(!imrsim_partial_io_wq){
        destroy_workqueue(imr_lsm_level_compaction_wq);
        imr_lsm_level_compaction_wq = NULL;
        destroy_workqueue(imr_lsm_zone_compaction_wq);
        imr_lsm_zone_compaction_wq = NULL;
        return -ENOMEM;
    }

    ret = dm_register_target(&imrsim_target);
    if(ret < 0){
        printk(KERN_ERR "imrsim: register failed\n");
        destroy_workqueue(imrsim_partial_io_wq);
        imrsim_partial_io_wq = NULL;
        destroy_workqueue(imr_lsm_level_compaction_wq);
        imr_lsm_level_compaction_wq = NULL;
        destroy_workqueue(imr_lsm_zone_compaction_wq);
        imr_lsm_zone_compaction_wq = NULL;
        return ret;
    }
    imr_lsm_debugfs_init();
    return 0;
}

/* kill IMRSim module */
static void dm_imrsim_exit(void)
{
    imr_lsm_debugfs_exit();
    dm_unregister_target(&imrsim_target);
    if(imrsim_partial_io_wq){
        flush_workqueue(imrsim_partial_io_wq);
        destroy_workqueue(imrsim_partial_io_wq);
        imrsim_partial_io_wq = NULL;
    }
    if(imr_lsm_level_compaction_wq){
        flush_workqueue(imr_lsm_level_compaction_wq);
        destroy_workqueue(imr_lsm_level_compaction_wq);
        imr_lsm_level_compaction_wq = NULL;
    }
    if(imr_lsm_zone_compaction_wq){
        flush_workqueue(imr_lsm_zone_compaction_wq);
        destroy_workqueue(imr_lsm_zone_compaction_wq);
        imr_lsm_zone_compaction_wq = NULL;
    }
}

module_init(dm_imrsim_init);    
module_exit(dm_imrsim_exit);    

/* Module related signature information */
MODULE_DESCRIPTION(DM_NAME "IMR Simulator");
MODULE_AUTHOR("Zhimin Zeng <im_zzm@126.com>");
MODULE_LICENSE("GPL");
