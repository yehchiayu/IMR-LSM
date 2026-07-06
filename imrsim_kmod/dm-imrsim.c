#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/crc32.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include "imrsim_types.h"
#include "imrsim_ioctl.h"
#include "imrsim_kapi.h"
#include "imrsim_zerror.h"

/*
 The kernel module in the Device Mapper framework is mainly responsible for 
 building the target driver to build the disk structure and function.
*/

/* Some basic disk information */
#define IMR_ZONE_SIZE_SHIFT_DEFAULT      16      /* number of blocks/zone, e.g. 2^16=65536 */
#define IMR_BLOCK_SIZE_SHIFT_DEFAULT     3       /* number of sectors/block, 8   */
#define IMR_PAGE_SIZE_SHIFT_DEFAULT      3       /* number of sectors/page, 8    */
#define IMR_SECTOR_SIZE_SHIFT_DEFAULT    9       /* number of bytes/sector, 512  */
#define IMR_TRANSFER_PENALTY             60      /* usec */
#define IMR_TRANSFER_PENALTY_MAX         1000    /* usec */
#define IMR_ROTATE_PENALTY               11000   /* usec ,  5400rpm->  rotate time: 11ms*/


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
#define IMR_LSM_LEVEL_RATIO              2
#define IMR_LSM_SCORE_SCALE              1000
#define IMR_LSM_SCORE_BOOST              10
#define IMR_LSM_SEGMENT_ZONE_MIXED       ((__u32)~0U)
#define IMR_LSM_TRACK_BOTTOM             1
#define IMR_LSM_TRACK_TOP                2
#define IMR_LSM_PLACEMENT_NONE           0
#define IMR_LSM_PLACEMENT_BOTTOM_TO_TOP  1
#define IMR_LSM_ZONE_COMPACTION_NONE     ((__u32)~0U)
#define IMR_LSM_BLOOM_MIN_BITS           256
#define IMR_LSM_BLOOM_MAX_BITS           16384
#define IMR_LSM_BLOOM_BITS_PER_KEY       10
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

__u32 VERSION = IMRSIM_VERSION(1,1,0);      /* The version number of IMRSIM：VERSION(x,y,z)=>((x<<16)|(y<<8)|z) */

struct imrsim_c{             /* Mapped devices in the Device Mapper framework, also known as logical devices. */
    struct dm_dev *dev;      /* block device */
    sector_t       start;    /* starting address */
};

/* Mutex resource locks */
struct mutex                     imrsim_zone_lock;
struct mutex                     imrsim_ioctl_lock;

/* IMRSIM Statistics */
static struct imrsim_state       *zone_state = NULL;
/* Array of zone status information */
static struct imrsim_zone_status *zone_status = NULL;

/* error log */
static __u32 imrsim_dbg_rerr;
static __u32 imrsim_dbg_werr;
static __u32 imrsim_dbg_log_enabled = 0;
static unsigned long imrsim_dev_idle_checkpoint = 0;

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

struct imr_lsm_block_entry {
    __u64 key;
    sector_t pba;
    sector_t source_pba;
    sector_t output_pba;
    __u8 source_pba_valid;
    __u8 output_mapped;
    __u8 output_copy_planned;
    __u8 output_copied;
    __u8 output_committed;
    __u8 valid;
    __u64 timestamp;
};

struct imr_lsm_segment {
    __u32 id;
    __u32 level;
    __u32 zone_idx;
    __u8 track_type;
    __u8 retired;
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
    __u8 zone_compaction_candidate_ready;
    __u32 last_zone_compaction_candidate_zone;
    __u32 last_zone_compaction_candidate_dest_zone;
    __u32 last_zone_compaction_candidate_map_size;
    __u8 last_zone_compaction_candidate_ready;
    __u64 zone_compaction_auto_run_count;
    __u64 zone_compaction_auto_run_failed_count;
    __u32 last_zone_compaction_auto_run_zone;
    int last_zone_compaction_auto_run_error;
    __u64 zone_compaction_count;
    __u64 zone_compaction_failed_count;
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
    __u32 last_compaction_from;
    __u32 last_compaction_to;
    __u32 last_compaction_input;
    __u32 last_compaction_output_total;
};

struct imr_lsm_metadata {
    bool initialized;
    __u8 zone_compaction_auto_run;
    __u8 zone_compaction_auto_running;
    __u64 timestamp;
    __u32 active_write_level;
    __u32 base_level;
    int lowest_unnecessary_level;
    __u32 level_max_entries[IMR_LSM_LEVELS];
    __u32 next_segment_id;
    __u32 segment_count;
    struct imr_lsm_segment *segment_head;
    struct imr_lsm_segment *segment_tail;
    struct rb_root read_tree;
    struct list_head read_lru;
    __u32 read_tree_limit;
    __u32 read_tree_size;
    struct imr_lsm_stats stats;
    struct imr_lsm_level_state levels[IMR_LSM_LEVELS];
};

static struct imr_lsm_metadata imr_lsm_meta;
static DEFINE_MUTEX(imr_lsm_lock);
static struct dentry *imr_lsm_debugfs_dir;
static struct block_device *imr_lsm_output_bdev;
static sector_t imr_lsm_output_bdev_start;

static int imrsim_read_page(struct block_device *dev, sector_t lba,
                            int size, struct page *page);
static int imrsim_write_page(struct block_device *dev, sector_t lba,
                             __u32 size, struct page *page);

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

/* Multi-device support, currently not supported */
int imrsim_single = 0;

/* Constants representing configuration changes */
enum imrsim_conf_change{
    IMR_NO_CHANGE     = 0x00,
    IMR_CONFIG_CHANGE = 0x01,
    IMR_STATS_CHANGE  = 0x02,
    IMR_STATUS_CHANGE = 0x04
};

/* persistent storage */
#define IMR_PSTORE_PG_EDG 92
#define IMR_PSTORE_PG_OFF 40
#define IMR_PSTORE_CHECK  1000
#define IMR_PSTORE_QDEPTH 128
#define IMR_PSTORE_PG_GAP 2

/* persistent storage task structure */
static struct imrsim_pstore_task
{
    struct task_struct  *pstore_thread; 
    __u32                sts_zone_idx;
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

/* read/write completion structure */
static struct imrsim_completion_control
{
    struct completion   read_event;
    struct completion   write_event;
    struct completion   rmw_event;
}imrsim_completion;

/* To get the size of the imrsim_stats structure. */
static __u32 imrsim_stats_size(void)
{
    return (sizeof(struct imrsim_dev_stats) + sizeof(__u32) + sizeof(__u64)*2 +
            sizeof(struct imrsim_zone_stats) * IMR_NUMZONES);
}

/* To get the size of the imrsim_state structure. */
static __u32 imrsim_state_size(void)
{
    return (sizeof(struct imrsim_state_header) + 
            sizeof(struct imrsim_config) + 
            sizeof(struct imrsim_dev_stats) + sizeof(__u32) +
            IMR_NUMZONES * sizeof(struct imrsim_zone_stats) + 
            IMR_NUMZONES * sizeof(struct imrsim_zone_status) +
            sizeof(__u32));
}

/* To get how many sectors a zone has. */
static __u32 num_sectors_zone(void)
{
    return (1 << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT);
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

static void imr_lsm_free_sorted_level_locked(__u32 level)
{
    struct imr_lsm_sorted_node *node = imr_lsm_meta.levels[level].sorted_head;

    while(node){
        struct imr_lsm_sorted_node *next = node->next;

        kfree(node);
        node = next;
    }

    imr_lsm_meta.levels[level].sorted_head = NULL;
    imr_lsm_meta.levels[level].sorted_count = 0;
}

static void imr_lsm_free_unsorted_level_locked(__u32 level)
{
    struct imr_lsm_unsorted_node *node = imr_lsm_meta.levels[level].unsorted_head;

    while(node){
        struct imr_lsm_unsorted_node *next = node->next;

        kfree(node);
        node = next;
    }

    imr_lsm_meta.levels[level].unsorted_head = NULL;
    imr_lsm_meta.levels[level].unsorted_count = 0;
}

static void imr_lsm_free_segments_locked(void)
{
    struct imr_lsm_segment *segment = imr_lsm_meta.segment_head;

    while(segment){
        struct imr_lsm_segment *next = segment->next;

        kfree(segment->block_table);
        kfree(segment->bloom_bits);
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
    }
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

static void imr_lsm_release_metadata_locked(void)
{
    __u32 level;

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        imr_lsm_free_unsorted_level_locked(level);
        imr_lsm_free_sorted_level_locked(level);
    }
    imr_lsm_free_segments_locked();
    imr_lsm_free_read_tree_locked();
    memset(&imr_lsm_meta, 0, sizeof(imr_lsm_meta));
    imr_lsm_init_read_tree_locked();
}

static void imr_lsm_release_metadata(void)
{
    mutex_lock(&imr_lsm_lock);
    imr_lsm_release_metadata_locked();
    mutex_unlock(&imr_lsm_lock);
}

static void imr_lsm_initialize_metadata_locked(void)
{
    imr_lsm_release_metadata_locked();
    imr_lsm_meta.initialized = true;
    imr_lsm_meta.timestamp = 0;
    imr_lsm_meta.active_write_level = IMR_LSM_MAX_LEVEL;
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

static struct imr_lsm_sorted_node *imr_lsm_sorted_find_locked(__u32 level,
                                                              __u64 key)
{
    struct imr_lsm_sorted_node *node = imr_lsm_meta.levels[level].sorted_head;

    while(node){
        if(node->key == key){
            return node;
        }
        if(node->key > key){
            return NULL;
        }
        node = node->next;
    }

    return NULL;
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

static __u32 imr_lsm_level_capacity(__u32 level)
{
    if(level >= IMR_LSM_LEVELS){
        return IMR_LSM_COMPACTION_THRESHOLD;
    }

    if(!imr_lsm_meta.level_max_entries[level] ||
       imr_lsm_meta.level_max_entries[level] == (__u32)~0U){
        return IMR_LSM_COMPACTION_THRESHOLD;
    }

    return imr_lsm_meta.level_max_entries[level];
}

static __u32 imr_lsm_level_total_count_locked(__u32 level)
{
    return imr_lsm_meta.levels[level].unsorted_count +
           imr_lsm_meta.levels[level].sorted_count;
}

static __u32 imr_lsm_mul_clamp_u32(__u32 value, __u32 multiplier)
{
    __u64 result = (__u64)value * multiplier;

    if(result > (__u32)~0U){
        return (__u32)~0U;
    }
    return (__u32)result;
}

static void imr_lsm_calculate_dynamic_levels_locked(void)
{
    __u32 max_level_size = 0;
    __u32 base_bytes_max = IMR_LSM_COMPACTION_THRESHOLD;
    __u32 base_bytes_min = base_bytes_max / IMR_LSM_LEVEL_RATIO;
    __u32 cur_level_size;
    __u32 base_level_size;
    __u32 level_size;
    int first_non_empty_level = -1;
    int level;

    if(!base_bytes_min){
        base_bytes_min = 1;
    }

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        imr_lsm_meta.level_max_entries[level] = (__u32)~0U;
    }
    imr_lsm_meta.lowest_unnecessary_level = -1;

    for(level = 1; level < IMR_LSM_LEVELS; level++){
        __u32 total_size = imr_lsm_level_total_count_locked(level);

        if(total_size > 0 && first_non_empty_level == -1){
            first_non_empty_level = level;
        }
        if(total_size > max_level_size){
            max_level_size = total_size;
        }
    }

    if(!max_level_size){
        imr_lsm_meta.base_level = IMR_LSM_MAX_LEVEL;
        imr_lsm_meta.active_write_level = imr_lsm_meta.base_level;
        imr_lsm_meta.level_max_entries[IMR_LSM_MAX_LEVEL] = base_bytes_max;
        return;
    }

    cur_level_size = max_level_size;
    for(level = IMR_LSM_MAX_LEVEL - 1; level >= first_non_empty_level; level--){
        cur_level_size /= IMR_LSM_LEVEL_RATIO;
        if(imr_lsm_meta.lowest_unnecessary_level == -1 &&
           cur_level_size <= base_bytes_min &&
           level < IMR_LSM_MAX_LEVEL - 1){
            imr_lsm_meta.lowest_unnecessary_level = level;
        }
    }

    if(cur_level_size <= base_bytes_min){
        imr_lsm_meta.base_level = first_non_empty_level;
        base_level_size = base_bytes_min + 1;
    }else{
        imr_lsm_meta.base_level = first_non_empty_level;
        while(imr_lsm_meta.base_level > 1 &&
              cur_level_size > base_bytes_max){
            imr_lsm_meta.base_level--;
            cur_level_size /= IMR_LSM_LEVEL_RATIO;
        }
        if(cur_level_size > base_bytes_max){
            base_level_size = base_bytes_max;
        }else{
            base_level_size = max_t(__u32, 1, cur_level_size);
        }
    }

    level_size = base_level_size;
    for(level = imr_lsm_meta.base_level; level < IMR_LSM_LEVELS; level++){
        if(level > imr_lsm_meta.base_level){
            level_size = imr_lsm_mul_clamp_u32(level_size,
                                               IMR_LSM_LEVEL_RATIO);
        }
        imr_lsm_meta.level_max_entries[level] =
            max_t(__u32, level_size, base_bytes_max);
    }
    imr_lsm_meta.active_write_level = imr_lsm_meta.base_level;
}

static void imr_lsm_debugfs_show_active_target_locked(struct seq_file *seq)
{
    imr_lsm_calculate_dynamic_levels_locked();
    seq_printf(seq, "active_write_level: %u\n",
               imr_lsm_meta.active_write_level);
    seq_printf(seq, "dynamic_base_level: %u\n", imr_lsm_meta.base_level);
    seq_printf(seq, "lowest_unnecessary_level: %d\n",
               imr_lsm_meta.lowest_unnecessary_level);
    seq_printf(seq, "insert_target: L%u unsorted\n",
               imr_lsm_meta.active_write_level);
    seq_puts(seq, "compact_target: next dynamic level\n");
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

static __u32 imr_lsm_bloom_choose_bits(__u32 key_count)
{
    __u64 target_bits;
    __u32 bits;

    if(!key_count){
        return IMR_LSM_BLOOM_MIN_BITS;
    }

    target_bits = (__u64)key_count * IMR_LSM_BLOOM_BITS_PER_KEY;
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

static int imr_lsm_segment_build_bloom(struct imr_lsm_segment *segment)
{
    __u32 key_count = segment->block_table_count;
    __u32 bits = imr_lsm_bloom_choose_bits(key_count);
    __u32 words = bits / 64;
    __u32 bits_per_key = key_count ?
        max_t(__u32, 1, bits / key_count) : IMR_LSM_BLOOM_BITS_PER_KEY;
    __u32 hashes = imr_lsm_bloom_choose_hashes(bits_per_key);
    __u32 entry_idx;

    segment->bloom_bits = kzalloc(sizeof(*segment->bloom_bits) * words,
                                  GFP_NOIO);
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
    kfree(builder->block_table);
    builder->block_table = NULL;
    builder->block_table_count = 0;
    builder->block_table_capacity = 0;
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
        new_capacity = IMR_LSM_COMPACTION_THRESHOLD;
    }

    new_table = kzalloc(sizeof(*new_table) * new_capacity, GFP_NOIO);
    if(!new_table){
        return -ENOMEM;
    }

    if(builder->block_table){
        memcpy(new_table, builder->block_table,
               sizeof(*new_table) * builder->block_table_count);
        kfree(builder->block_table);
    }
    builder->block_table = new_table;
    builder->block_table_capacity = new_capacity;

    return 0;
}

static int imr_lsm_segment_builder_add_block_entry(
    struct imr_lsm_segment_builder *builder, __u64 key, sector_t pba,
    __u8 valid, __u64 timestamp)
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
    entry->key = key;
    entry->pba = pba;
    entry->source_pba = 0;
    entry->output_pba = 0;
    entry->source_pba_valid = 0;
    entry->output_mapped = 0;
    entry->output_copy_planned = 0;
    entry->output_copied = 0;
    entry->output_committed = 0;
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

    ret = imr_lsm_segment_builder_add_block_entry(builder, key, pba,
                                                  valid, timestamp);
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
static bool imr_lsm_find_newer_record_locked(__u64 key, __u64 timestamp,
                                             __u64 *newer_timestamp,
                                             __u8 *newer_valid);
static int imr_lsm_compact_zone_locked(__u32 source_zone);

static int imr_lsm_append_segment_locked(
    __u32 level, __u8 track_type,
    struct imr_lsm_segment_builder *builder)
{
    struct imr_lsm_segment *segment;
    int ret;

    if(!builder->node_count){
        imr_lsm_segment_builder_release(builder);
        return 0;
    }

    segment = kzalloc(sizeof(*segment), GFP_NOIO);
    if(!segment){
        printk(KERN_ERR "imrsim: IMR-LSM segment alloc failed L%u nodes=%u\n",
               level, builder->node_count);
        return -ENOMEM;
    }

    segment->id = imr_lsm_meta.next_segment_id++;
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
    ret = imr_lsm_segment_build_bloom(segment);
    if(ret){
        printk(KERN_ERR "imrsim: IMR-LSM segment bloom alloc failed L%u table_entries=%u\n",
               level, builder->block_table_count);
        kfree(segment);
        return ret;
    }
    builder->block_table = NULL;
    builder->block_table_count = 0;
    builder->block_table_capacity = 0;
    imr_lsm_apply_segment_placement_locked(segment);
    imr_lsm_map_segment_output_entries_locked(segment);

    if(imr_lsm_meta.segment_tail){
        imr_lsm_meta.segment_tail->next = segment;
    }else{
        imr_lsm_meta.segment_head = segment;
    }
    imr_lsm_meta.segment_tail = segment;
    imr_lsm_meta.segment_count++;
    imr_lsm_recalculate_segment_invalid_stats_locked();

    printk(KERN_INFO "imrsim: IMR-LSM segment id=%u L%u nodes=%u key=%llu-%llu ts=%llu-%llu bloom_keys=%u bloom_bits=%u bloom_hashes=%u table_entries=%u placement=%u bottom_track=%u-%u top_track=%u-%u output=%u output_track=%u output_pba=%llu-%llu\n",
           segment->id,
           segment->level,
           segment->node_count,
           (unsigned long long)segment->min_key,
           (unsigned long long)segment->max_key,
           (unsigned long long)segment->min_timestamp,
           (unsigned long long)segment->max_timestamp,
           segment->bloom_key_count,
           segment->bloom_bits_count,
           segment->bloom_hash_count,
           segment->block_table_count,
           segment->placement_policy,
           segment->placement_bottom_track_start,
           segment->placement_bottom_track_end,
           segment->placement_top_track_start,
           segment->placement_top_track_end,
           segment->output_allocated,
           segment->output_track_type,
           (unsigned long long)segment->output_pba_start,
           (unsigned long long)segment->output_pba_end);

    return 0;
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

static bool imr_lsm_find_latest_record_locked(__u64 key,
                                              __u8 *latest_valid,
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

static bool imr_lsm_find_newer_record_locked(__u64 key, __u64 timestamp,
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
     * The legacy zone map is still a read-path fallback. Keep the newest
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
    __u32 segment_count = 0;
    __u32 entry_count = 0;
    __u64 invalid_segment_count = 0;
    __u64 invalid_entry_count = 0;
    __u64 obsolete_entry_count = 0;
    __u64 tombstone_entry_count = 0;
    __u64 delete_invalid_entry_count = 0;
    __u32 max_ratio = 0;
    __u32 max_ratio_segment_id = 0;

    while(segment){
        __u32 entry_idx;

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
                invalid = true;
                if(entry->valid && !newer_valid){
                    segment->delete_invalid_count++;
                    delete_invalid_entry_count++;
                }
            }

            if(invalid){
                segment->invalid_count++;
                invalid_entry_count++;
            }else{
                segment->live_count++;
            }
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

    segment->retired = 1;
    segment->compaction_candidate = 0;
    segment->compaction_score = 0;

    imr_lsm_meta.stats.segment_compaction_execute_count++;
    imr_lsm_meta.stats.last_segment_compaction_from_id = segment->id;
    imr_lsm_meta.stats.last_segment_compaction_to_id = new_segment_id;
    imr_lsm_meta.stats.last_segment_compaction_input_entries = input_entries;
    imr_lsm_meta.stats.last_segment_compaction_live_entries = live_entries;
    imr_lsm_meta.stats.last_segment_compaction_dropped_entries =
        dropped_entries;

    imr_lsm_recalculate_segment_invalid_stats_locked();
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
    imr_lsm_meta.stats.zone_compaction_candidate_ready = 0;
}

static void imr_lsm_record_zone_compaction_candidate_locked(__u32 zone_idx)
{
    __u32 dest_zone;
    __u8 ready = 0;
    bool new_candidate;

    if(!zone_status || zone_idx >= IMR_NUMZONES){
        return;
    }
    if(zone_status[zone_idx].z_map_size < TOTAL_ITEMS){
        return;
    }

    dest_zone = zone_idx + 1;
    if(dest_zone < IMR_NUMZONES &&
       !zone_status[dest_zone].z_map_size){
        ready = 1;
    }

    new_candidate =
        imr_lsm_meta.stats.zone_compaction_candidate_zone != zone_idx;
    if(new_candidate){
        imr_lsm_meta.stats.zone_compaction_candidate_count++;
    }

    imr_lsm_meta.stats.zone_compaction_candidate_zone = zone_idx;
    imr_lsm_meta.stats.zone_compaction_candidate_dest_zone =
        dest_zone < IMR_NUMZONES ? dest_zone :
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.zone_compaction_candidate_map_size =
        zone_status[zone_idx].z_map_size;
    imr_lsm_meta.stats.zone_compaction_candidate_ready = ready;

    imr_lsm_meta.stats.last_zone_compaction_candidate_zone = zone_idx;
    imr_lsm_meta.stats.last_zone_compaction_candidate_dest_zone =
        dest_zone < IMR_NUMZONES ? dest_zone :
        IMR_LSM_ZONE_COMPACTION_NONE;
    imr_lsm_meta.stats.last_zone_compaction_candidate_map_size =
        zone_status[zone_idx].z_map_size;
    imr_lsm_meta.stats.last_zone_compaction_candidate_ready = ready;

    if(new_candidate){
        printk(KERN_INFO "imrsim: IMR-LSM zone compaction candidate zone=%u dest=%u map_size=%u ready=%u\n",
               zone_idx,
               dest_zone < IMR_NUMZONES ? dest_zone :
               IMR_LSM_ZONE_COMPACTION_NONE,
               zone_status[zone_idx].z_map_size,
               ready);
    }
}

static void imr_lsm_record_zone_compaction_candidate(__u32 zone_idx)
{
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_record_zone_compaction_candidate_locked(zone_idx);
    mutex_unlock(&imr_lsm_lock);
}

static int imr_lsm_auto_run_zone_compaction_locked(__u32 zone_idx)
{
    int ret;

    if(!imr_lsm_meta.zone_compaction_auto_run ||
       imr_lsm_meta.zone_compaction_auto_running){
        return 0;
    }
    if(imr_lsm_meta.stats.zone_compaction_candidate_zone != zone_idx ||
       !imr_lsm_meta.stats.zone_compaction_candidate_ready){
        return 0;
    }

    imr_lsm_meta.zone_compaction_auto_running = 1;
    imr_lsm_meta.stats.last_zone_compaction_auto_run_zone = zone_idx;
    ret = imr_lsm_compact_zone_locked(zone_idx);
    imr_lsm_meta.stats.last_zone_compaction_auto_run_error = ret;
    if(ret){
        imr_lsm_meta.stats.zone_compaction_auto_run_failed_count++;
        printk(KERN_ERR "imrsim: IMR-LSM auto zone compaction failed zone=%u ret=%d\n",
               zone_idx, ret);
    }else{
        imr_lsm_meta.stats.zone_compaction_auto_run_count++;
        printk(KERN_INFO "imrsim: IMR-LSM auto zone compact zone=%u\n",
               zone_idx);
    }
    imr_lsm_meta.zone_compaction_auto_running = 0;

    return ret;
}

/*
 * Debug/test helper only.
 *
 * This seeds zone metadata as if the whole zone were already populated, which
 * lets VM validation exercise zone compaction without issuing a full-zone
 * write workload first.  It is not part of the normal LSM data path; formal
 * zone compaction still runs through imr_lsm_compact_zone_locked().
 */
static int imr_lsm_seed_full_zone_locked(__u32 zone_idx)
{
    __u32 bottom_blocks = imr_lsm_bottom_range_blocks();
    __u32 offset;

    if(!zone_status || zone_idx >= IMR_NUMZONES){
        return -EINVAL;
    }

    for(offset = 0; offset < TOTAL_ITEMS; offset++){
        sector_t pba;

        if(offset < bottom_blocks){
            pba = imr_lsm_zone_bottom_pba(zone_idx, offset);
        }else{
            pba = imr_lsm_zone_top_pba(zone_idx,
                                       offset - bottom_blocks);
        }

        zone_status[zone_idx].z_pba_map[offset] =
            (int)((pba - zone_idx_lba(zone_idx)) >>
                  IMR_BLOCK_SIZE_SHIFT);
    }
    zone_status[zone_idx].z_map_size = TOTAL_ITEMS;
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
    if(!imr_lsm_output_bdev){
        return -ENODEV;
    }

    ret = imrsim_read_page(imr_lsm_output_bdev,
                           imr_lsm_output_bdev_start + source_pba,
                           block_bytes, page);
    if(ret < 0){
        return ret;
    }
    ret = imrsim_write_page(imr_lsm_output_bdev,
                            imr_lsm_output_bdev_start + dest_pba,
                            block_bytes, page);
    if(ret < 0){
        return ret;
    }

    *copied = true;
    return 0;
}

static int imr_lsm_compact_zone_locked(__u32 source_zone)
{
    struct imr_lsm_segment_builder segment_builder = {0};
    struct imr_lsm_segment *new_segment;
    struct page *page;
    void *page_addr;
    __u32 block_bytes;
    __u32 bottom_blocks = imr_lsm_bottom_range_blocks();
    __u32 input_entries = TOTAL_ITEMS;
    __u32 live_entries = 0;
    __u32 skipped_entries = 0;
    __u32 copied_entries = 0;
    __u32 failed_entries = 0;
    __u32 dest_zone0 = source_zone;
    __u32 dest_zone1 = source_zone + 1;
    sector_t output_start = 0;
    sector_t output_end = 0;
    int ret = 0;
    __u32 offset;

    if(!zone_status || source_zone >= IMR_NUMZONES){
        ret = -EINVAL;
        goto out_stats;
    }
    if(dest_zone1 >= IMR_NUMZONES){
        ret = -ENOSPC;
        goto out_stats;
    }
    if(zone_status[source_zone].z_map_size < TOTAL_ITEMS){
        ret = -EINVAL;
        goto out_stats;
    }
    if(zone_status[dest_zone1].z_map_size){
        ret = -EBUSY;
        goto out_stats;
    }
    if((__u64)TOTAL_ITEMS > ((__u64)bottom_blocks * 2)){
        ret = -ENOSPC;
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

    for(offset = 0; offset < TOTAL_ITEMS; offset++){
        __u64 key = imr_lsm_zone_key_start(source_zone) + offset;
        __u8 latest_valid;
        sector_t latest_pba;
        bool latest_found;
        sector_t source_pba;
        sector_t dest_pba;
        __u32 output_index;
        __u32 dest_zone;
        __u32 dest_bottom_offset;
        bool copied;

        latest_found =
            imr_lsm_find_latest_record_locked(key, &latest_valid,
                                              &latest_pba);
        if(latest_found){
            if(!latest_valid){
                skipped_entries++;
                continue;
            }
            source_pba = latest_pba;
        }else{
            if(zone_status[source_zone].z_pba_map[offset] == -1){
                skipped_entries++;
                continue;
            }
            source_pba = zone_idx_lba(source_zone) +
                ((__u64)zone_status[source_zone].z_pba_map[offset] <<
                 IMR_BLOCK_SIZE_SHIFT);
        }

        output_index = live_entries;
        if(output_index < bottom_blocks){
            dest_zone = dest_zone0;
            dest_bottom_offset = output_index;
        }else{
            dest_zone = dest_zone1;
            dest_bottom_offset = output_index - bottom_blocks;
        }
        if(dest_bottom_offset >= bottom_blocks){
            ret = -ENOSPC;
            __free_page(page);
            goto out_release;
        }

        dest_pba = imr_lsm_zone_bottom_pba(dest_zone, dest_bottom_offset);
        memset(page_addr, 0, PAGE_SIZE);
        ret = imr_lsm_copy_zone_compaction_block(page, source_pba, dest_pba,
                                                 block_bytes, &copied);
        if(ret){
            failed_entries++;
            __free_page(page);
            goto out_release;
        }
        if(copied){
            copied_entries++;
        }

        ret = imr_lsm_segment_builder_add(&segment_builder, key, dest_pba,
                                          source_zone, 1,
                                          ++imr_lsm_meta.timestamp);
        if(ret){
            __free_page(page);
            goto out_release;
        }
        if(!live_entries){
            output_start = dest_pba;
        }
        output_end = dest_pba + (((sector_t)1 << IMR_BLOCK_SIZE_SHIFT) - 1);
        live_entries++;
    }

    __free_page(page);
    if(!live_entries){
        ret = -ENODATA;
        goto out_release;
    }

    ret = imr_lsm_append_segment_locked(IMR_LSM_MAX_LEVEL,
                                        IMR_LSM_TRACK_BOTTOM,
                                        &segment_builder);
    if(ret){
        goto out_release;
    }

    new_segment = imr_lsm_meta.segment_tail;
    memset(zone_status[source_zone].z_pba_map, -1,
           TOTAL_ITEMS * sizeof(int));
    for(offset = 0; offset < new_segment->block_table_count; offset++){
        struct imr_lsm_block_entry *entry =
            &new_segment->block_table[offset];
        __u32 logical_offset =
            (__u32)(entry->key - imr_lsm_zone_key_start(source_zone));

        zone_status[source_zone].z_pba_map[logical_offset] =
            (int)((entry->pba - zone_idx_lba(source_zone)) >>
                  IMR_BLOCK_SIZE_SHIFT);
        imr_lsm_tree_update_locked(entry->key, entry->pba, entry->valid,
                                   entry->timestamp);
    }
    zone_status[source_zone].z_map_size =
        live_entries > bottom_blocks ? bottom_blocks : live_entries;
    zone_status[dest_zone1].z_map_size =
        live_entries > bottom_blocks ? live_entries - bottom_blocks : 0;
    imr_lsm_clear_zone_top_usage(source_zone);
    imr_lsm_clear_zone_top_usage(dest_zone1);
    imr_lsm_recalculate_segment_invalid_stats_locked();

    imr_lsm_meta.stats.zone_compaction_count++;
    imr_lsm_record_zone_compaction_result(source_zone, dest_zone0,
                                          dest_zone1, input_entries,
                                          live_entries, skipped_entries,
                                          copied_entries, failed_entries,
                                          output_start, output_end, 0);
    imr_lsm_clear_zone_compaction_candidate_locked(source_zone);
    printk(KERN_INFO "imrsim: IMR-LSM zone compacted source=%u dest=%u,%u input=%u live=%u skipped=%u copied=%u output=%llu-%llu\n",
           source_zone, dest_zone0, dest_zone1, input_entries,
           live_entries, skipped_entries, copied_entries,
           (unsigned long long)output_start,
           (unsigned long long)output_end);
    return 0;

out_release:
    imr_lsm_segment_builder_release(&segment_builder);
out_stats:
    imr_lsm_meta.stats.zone_compaction_failed_count++;
    imr_lsm_record_zone_compaction_result(source_zone, dest_zone0,
                                          dest_zone1, input_entries,
                                          live_entries, skipped_entries,
                                          copied_entries, failed_entries,
                                          output_start, output_end, ret);
    printk(KERN_ERR "imrsim: IMR-LSM zone compaction failed source=%u ret=%d live=%u skipped=%u copied=%u failed=%u\n",
           source_zone, ret, live_entries, skipped_entries,
           copied_entries, failed_entries);
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

static int imr_lsm_sorted_upsert_value_locked(__u32 level, __u64 key,
                                              sector_t pba, __u32 zone_idx,
                                              __u8 valid, __u64 timestamp)
{
    struct imr_lsm_sorted_node *node;
    struct imr_lsm_sorted_node **link;

    node = imr_lsm_sorted_find_locked(level, key);
    if(node){
        if(timestamp > node->timestamp){
            node->pba = pba;
            node->zone_idx = zone_idx;
            node->valid = valid;
            node->timestamp = timestamp;
        }
        return 0;
    }

    node = kzalloc(sizeof(*node), GFP_NOIO);
    if(!node){
        return -ENOMEM;
    }

    node->key = key;
    node->pba = pba;
    node->zone_idx = zone_idx;
    node->valid = valid;
    node->timestamp = timestamp;

    link = &imr_lsm_meta.levels[level].sorted_head;
    while(*link && (*link)->key < node->key){
        link = &(*link)->next;
    }
    node->next = *link;
    *link = node;
    imr_lsm_meta.levels[level].sorted_count++;

    return 0;
}

static int imr_lsm_sorted_upsert_unsorted_locked(__u32 level,
                                                 struct imr_lsm_unsorted_node *src)
{
    return imr_lsm_sorted_upsert_value_locked(level, src->key, src->pba,
                                              src->zone_idx, src->valid,
                                              src->timestamp);
}

static int imr_lsm_sorted_upsert_sorted_locked(__u32 level,
                                               struct imr_lsm_sorted_node *src)
{
    return imr_lsm_sorted_upsert_value_locked(level, src->key, src->pba,
                                              src->zone_idx, src->valid,
                                              src->timestamp);
}

static int imr_lsm_flush_bottom_unsorted_locked(void)
{
    struct imr_lsm_unsorted_node *node;
    struct imr_lsm_segment_builder segment_builder = {0};
    __u32 input_count;
    int ret = 0;

    if(imr_lsm_meta.base_level >= IMR_LSM_MAX_LEVEL ||
       !imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].unsorted_count){
        return 0;
    }

    input_count = imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].unsorted_count;
    node = imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].unsorted_head;
    while(node){
        ret = imr_lsm_segment_builder_add_unsorted(&segment_builder, node);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM bottom flush table alloc failed L%u\n",
                   IMR_LSM_MAX_LEVEL);
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        ret = imr_lsm_sorted_upsert_unsorted_locked(IMR_LSM_MAX_LEVEL, node);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM bottom flush alloc failed L%u\n",
                   IMR_LSM_MAX_LEVEL);
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        node = node->next;
    }

    imr_lsm_free_unsorted_level_locked(IMR_LSM_MAX_LEVEL);
    ret = imr_lsm_append_segment_locked(IMR_LSM_MAX_LEVEL,
                                        IMR_LSM_TRACK_BOTTOM,
                                        &segment_builder);
    if(ret){
        imr_lsm_segment_builder_release(&segment_builder);
        return ret;
    }
    imr_lsm_meta.stats.compaction_count++;
    imr_lsm_meta.stats.last_compaction_from = IMR_LSM_MAX_LEVEL;
    imr_lsm_meta.stats.last_compaction_to = IMR_LSM_MAX_LEVEL;
    imr_lsm_meta.stats.last_compaction_input = input_count;
    imr_lsm_meta.stats.last_compaction_output_total =
        imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].sorted_count;
    imr_lsm_calculate_dynamic_levels_locked();

    printk(KERN_INFO "imrsim: IMR-LSM flushed bottom L%u unsorted=%u sorted=%u base=L%u\n",
           IMR_LSM_MAX_LEVEL,
           input_count,
           imr_lsm_meta.levels[IMR_LSM_MAX_LEVEL].sorted_count,
           imr_lsm_meta.base_level);

    return 0;
}

static __u32 imr_lsm_compaction_dst_level_locked(__u32 level)
{
    __u32 dst;

    if(level >= IMR_LSM_MAX_LEVEL){
        return IMR_LSM_MAX_LEVEL;
    }

    imr_lsm_calculate_dynamic_levels_locked();
    for(dst = level + 1; dst < IMR_LSM_LEVELS; dst++){
        if(imr_lsm_meta.level_max_entries[dst] != (__u32)~0U){
            return dst;
        }
    }

    return IMR_LSM_MAX_LEVEL;
}

static __u64 imr_lsm_compaction_score_locked(__u32 level,
                                             __u64 total_downcompact_entries)
{
    __u64 level_entries;
    __u64 target;
    __u64 score;

    if(level >= IMR_LSM_LEVELS - 1){
        return 0;
    }

    level_entries = imr_lsm_level_total_count_locked(level);
    if(!level_entries){
        return 0;
    }

    if(imr_lsm_meta.lowest_unnecessary_level >= 0 &&
       level <= (__u32)imr_lsm_meta.lowest_unnecessary_level){
        return IMR_LSM_SCORE_SCALE * IMR_LSM_SCORE_BOOST + 1 +
               ((__u32)imr_lsm_meta.lowest_unnecessary_level - level);
    }

    target = imr_lsm_level_capacity(level);
    if(!target){
        target = 1;
    }

    if(level_entries < target){
        return div64_u64(level_entries * IMR_LSM_SCORE_SCALE, target);
    }

    score = div64_u64(level_entries * IMR_LSM_SCORE_SCALE *
                      IMR_LSM_SCORE_BOOST,
                      target + total_downcompact_entries);
    return score;
}

static __u32 imr_lsm_pick_compaction_level_locked(void)
{
    __u32 best_level = IMR_LSM_LEVELS;
    __u64 best_score = 0;
    __u64 total_downcompact_entries = 0;
    __u32 level;

    imr_lsm_calculate_dynamic_levels_locked();
    for(level = 0; level < IMR_LSM_LEVELS - 1; level++){
        __u64 score = imr_lsm_compaction_score_locked(level,
                                                      total_downcompact_entries);
        __u32 level_entries = imr_lsm_level_total_count_locked(level);
        __u32 target = imr_lsm_level_capacity(level);

        if(score > best_score){
            best_score = score;
            best_level = level;
        }

        if(imr_lsm_meta.lowest_unnecessary_level >= 0 &&
           level <= (__u32)imr_lsm_meta.lowest_unnecessary_level){
            total_downcompact_entries += level_entries;
        }else if(level_entries > target){
            total_downcompact_entries += level_entries - target;
        }
    }

    if(best_score >= IMR_LSM_SCORE_SCALE){
        return best_level;
    }

    return IMR_LSM_LEVELS;
}

static int imr_lsm_compact_level_locked(__u32 level)
{
    struct imr_lsm_unsorted_node *node;
    struct imr_lsm_sorted_node *sorted_node;
    struct imr_lsm_segment_builder segment_builder = {0};
    __u32 compacted_count;
    __u32 sorted_count;
    __u32 dst_level;
    int ret = 0;

    compacted_count = imr_lsm_meta.levels[level].unsorted_count;
    sorted_count = imr_lsm_meta.levels[level].sorted_count;
    dst_level = imr_lsm_compaction_dst_level_locked(level);

    if(!compacted_count && (!sorted_count || dst_level == level)){
        return 0;
    }

    node = imr_lsm_meta.levels[level].unsorted_head;
    while(node){
        ret = imr_lsm_segment_builder_add_unsorted(&segment_builder, node);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM compaction table alloc failed L%u\n",
                   level);
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        ret = imr_lsm_sorted_upsert_unsorted_locked(dst_level, node);
        if(ret){
            printk(KERN_ERR "imrsim: IMR-LSM compaction alloc failed L%u\n",
                   level);
            imr_lsm_segment_builder_release(&segment_builder);
            return ret;
        }
        node = node->next;
    }

    if(dst_level != level){
        sorted_node = imr_lsm_meta.levels[level].sorted_head;
        while(sorted_node){
            ret = imr_lsm_segment_builder_add_sorted(&segment_builder,
                                                     sorted_node);
            if(ret){
                printk(KERN_ERR "imrsim: IMR-LSM compaction table alloc failed L%u->L%u\n",
                       level, dst_level);
                imr_lsm_segment_builder_release(&segment_builder);
                return ret;
            }
            ret = imr_lsm_sorted_upsert_sorted_locked(dst_level, sorted_node);
            if(ret){
                printk(KERN_ERR "imrsim: IMR-LSM compaction alloc failed L%u->L%u\n",
                       level, dst_level);
                imr_lsm_segment_builder_release(&segment_builder);
                return ret;
            }
            sorted_node = sorted_node->next;
        }

        imr_lsm_free_unsorted_level_locked(level);
        imr_lsm_free_sorted_level_locked(level);
    }else{
        imr_lsm_free_unsorted_level_locked(level);
    }
    ret = imr_lsm_append_segment_locked(dst_level, IMR_LSM_TRACK_BOTTOM,
                                        &segment_builder);
    if(ret){
        imr_lsm_segment_builder_release(&segment_builder);
        return ret;
    }
    imr_lsm_meta.stats.compaction_count++;
    imr_lsm_meta.stats.last_compaction_from = level;
    imr_lsm_meta.stats.last_compaction_to = dst_level;
    imr_lsm_meta.stats.last_compaction_input = compacted_count + sorted_count;
    imr_lsm_meta.stats.last_compaction_output_total =
        imr_lsm_meta.levels[dst_level].sorted_count;
    imr_lsm_calculate_dynamic_levels_locked();

    printk(KERN_INFO "imrsim: IMR-LSM compacted L%u->L%u unsorted=%u sorted=%u dst_sorted=%u base=L%u lowest_unnecessary=%d cleared_src=1\n",
           level,
           dst_level,
           compacted_count,
           sorted_count,
           imr_lsm_meta.levels[dst_level].sorted_count,
           imr_lsm_meta.base_level,
           imr_lsm_meta.lowest_unnecessary_level);

    return 0;
}

static int imr_lsm_run_compactions_locked(void)
{
    __u32 rounds;
    int ret;

    imr_lsm_calculate_dynamic_levels_locked();
    ret = imr_lsm_flush_bottom_unsorted_locked();
    if(ret){
        return ret;
    }

    for(rounds = 0; rounds < IMR_LSM_LEVELS; rounds++){
        __u32 level = imr_lsm_pick_compaction_level_locked();

        if(level >= IMR_LSM_LEVELS){
            return 0;
        }

        ret = imr_lsm_compact_level_locked(level);
        if(ret){
            return ret;
        }
    }

    return 0;
}

static int imr_lsm_append_unsorted_node_locked(__u32 level, __u64 key,
                                               sector_t pba, __u32 zone_idx,
                                               __u8 valid)
{
    struct imr_lsm_unsorted_node *node;
    int ret;

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
    imr_lsm_tree_update_locked(key, pba, valid, node->timestamp);

    printk(KERN_INFO "imrsim: IMR-LSM append unsorted L%u key=%llu pba=%llu valid=%u ts=%llu\n",
           level, key, (unsigned long long)pba, valid, node->timestamp);
    if(imr_lsm_meta.levels[level].unsorted_count >= imr_lsm_level_capacity(level)){
        printk(KERN_INFO "imrsim: IMR-LSM compaction should trigger L%u unsorted_count=%u capacity=%u\n",
               level,
               imr_lsm_meta.levels[level].unsorted_count,
               imr_lsm_level_capacity(level));
        ret = imr_lsm_compact_level_locked(level);
        if(ret){
            return ret;
        }
        ret = imr_lsm_run_compactions_locked();
        if(!ret){
            imr_lsm_recalculate_segment_invalid_stats_locked();
        }
        return ret;
    }

    ret = imr_lsm_run_compactions_locked();
    if(!ret){
        imr_lsm_recalculate_segment_invalid_stats_locked();
    }
    return ret;
}

static int imr_lsm_append_unsorting_node(__u32 level, __u64 key,
                                         sector_t pba, __u32 zone_idx)
{
    __u32 target_level = level;
    int ret;

    if(level >= IMR_LSM_LEVELS){
        printk(KERN_ERR "imrsim: IMR-LSM invalid level: %u\n", level);
        return -EINVAL;
    }

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_calculate_dynamic_levels_locked();
    ret = imr_lsm_flush_bottom_unsorted_locked();
    if(ret){
        mutex_unlock(&imr_lsm_lock);
        return ret;
    }
    if(level == IMR_LSM_DEFAULT_UNSORTED_LEVEL){
        target_level = imr_lsm_meta.active_write_level;
    }

    ret = imr_lsm_append_unsorted_node_locked(target_level, key, pba, zone_idx, 1);
    if(!ret){
        imr_lsm_meta.stats.lsm_write_count++;
    }
    mutex_unlock(&imr_lsm_lock);
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

    mutex_lock(&imr_lsm_lock);
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
    int ret;

    ret = imr_lsm_record_insert(zone_idx, logical_lba, physical_lba);
    if(!ret){
        imr_lsm_record_zone_compaction_candidate(zone_idx);
    }

    return ret;
}

static void imr_lsm_record_logical_write(void)
{
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.stats.logical_write_count++;
    mutex_unlock(&imr_lsm_lock);
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

    mutex_lock(&imr_lsm_lock);
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
        printk(KERN_INFO "imrsim: IMR-LSM read tree hit key=%llu pba=%llu ts=%llu\n",
               (unsigned long long)key,
               (unsigned long long)*pba,
               (unsigned long long)tree_timestamp);
        mutex_unlock(&imr_lsm_lock);
        return result;
    }
    if(result == IMR_LSM_LOOKUP_DELETED){
        imr_lsm_meta.stats.tombstone_hit_count++;
        printk(KERN_INFO "imrsim: IMR-LSM read tree tombstone key=%llu ts=%llu\n",
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
            printk(KERN_INFO "imrsim: IMR-LSM read hit key=%llu pba=%llu ts=%llu\n",
                   (unsigned long long)key,
                   (unsigned long long)*pba,
                   (unsigned long long)newest_timestamp);
        }
    }else if(result == IMR_LSM_LOOKUP_DELETED){
        imr_lsm_tree_update_locked(key, newest_pba, newest_valid,
                                   newest_timestamp);
        imr_lsm_meta.stats.tombstone_hit_count++;
        printk(KERN_INFO "imrsim: IMR-LSM read tombstone key=%llu ts=%llu\n",
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
 */
static int imr_lsm_delete(__u64 key)
{
    int ret;

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_calculate_dynamic_levels_locked();
    ret = imr_lsm_flush_bottom_unsorted_locked();
    if(ret){
        mutex_unlock(&imr_lsm_lock);
        return ret;
    }

    imr_lsm_tree_remove_locked(key);
    ret = imr_lsm_append_unsorted_node_locked(imr_lsm_meta.active_write_level,
                                              key, 0, 0, 0);
    if(!ret){
        imr_lsm_meta.stats.delete_count++;
    }
    mutex_unlock(&imr_lsm_lock);
    if(ret){
        return ret;
    }

    printk(KERN_INFO "imrsim: IMR-LSM delete append tombstone key=%llu\n",
           (unsigned long long)key);
    return 1;
}

static void imr_lsm_record_fallback(void)
{
    mutex_lock(&imr_lsm_lock);
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
    seq_printf(seq, "timestamp: %llu\n",
               (unsigned long long)imr_lsm_meta.timestamp);
    imr_lsm_debugfs_show_active_target_locked(seq);
    seq_printf(seq, "node_limit_per_level: %u (0 means unlimited)\n",
               IMR_LSM_DEBUG_NODE_LIMIT);
    seq_printf(seq, "compaction_base: %u\n", IMR_LSM_COMPACTION_THRESHOLD);
    seq_printf(seq, "level_ratio: %u\n", IMR_LSM_LEVEL_RATIO);

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_unsorted_node *node;
        __u32 trigger_capacity = imr_lsm_level_capacity(level);
        __u32 idx = 0;

        seq_printf(seq, "\nlevel %u unsorted_count: %u dynamic_target: %u logical_segments: %u\n",
                   level, imr_lsm_meta.levels[level].unsorted_count,
                   trigger_capacity,
                   imr_lsm_segment_count_locked(level));
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
    seq_printf(seq, "compaction_base: %u\n", IMR_LSM_COMPACTION_THRESHOLD);
    seq_printf(seq, "level_ratio: %u\n", IMR_LSM_LEVEL_RATIO);

    for(level = 0; level < IMR_LSM_LEVELS; level++){
        struct imr_lsm_sorted_node *node;
        __u32 trigger_capacity = imr_lsm_level_capacity(level);
        __u32 idx = 0;

        seq_printf(seq, "\nlevel %u sorted_count: %u dynamic_target: %u logical_segments: %u\n",
                   level, imr_lsm_meta.levels[level].sorted_count,
                   trigger_capacity,
                   imr_lsm_segment_count_locked(level));
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

    len = min(count, sizeof(buf) - 1);
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

    mutex_lock(&imr_lsm_lock);
    if(imr_lsm_meta.initialized){
        cleared = imr_lsm_clear_read_tree_locked();
    }
    mutex_unlock(&imr_lsm_lock);

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

    len = min(count, sizeof(buf) - 1);
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

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }
    imr_lsm_meta.read_tree_limit = limit;
    imr_lsm_tree_evict_locked();
    mutex_unlock(&imr_lsm_lock);

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

static int imr_lsm_debugfs_stats_show(struct seq_file *seq, void *unused)
{
    mutex_lock(&imr_lsm_lock);
    seq_printf(seq, "initialized: %u\n", imr_lsm_meta.initialized ? 1 : 0);
    imr_lsm_debugfs_show_active_target_locked(seq);
    seq_printf(seq, "logical_write_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.logical_write_count);
    seq_printf(seq, "lsm_record_insert_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.lsm_record_insert_count);
    seq_printf(seq, "lsm_write_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.lsm_write_count);
    seq_printf(seq, "delete_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.delete_count);
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
    seq_printf(seq, "last_zone_compaction_candidate_ready: %u\n",
               imr_lsm_meta.stats.last_zone_compaction_candidate_ready);
    seq_printf(seq, "zone_compaction_auto_run_enabled: %u\n",
               imr_lsm_meta.zone_compaction_auto_run ? 1 : 0);
    seq_printf(seq, "zone_compaction_auto_run_count: %llu\n",
               (unsigned long long)imr_lsm_meta.stats.zone_compaction_auto_run_count);
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
    seq_printf(seq, "last_compaction_from: L%u\n",
               imr_lsm_meta.stats.last_compaction_from);
    seq_printf(seq, "last_compaction_to: L%u\n",
               imr_lsm_meta.stats.last_compaction_to);
    seq_printf(seq, "last_compaction_input: %u\n",
               imr_lsm_meta.stats.last_compaction_input);
    seq_printf(seq, "last_compaction_output_total: %u\n",
               imr_lsm_meta.stats.last_compaction_output_total);
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

    len = min(count, sizeof(buf) - 1);
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtoull(buf, 0, &key);
    if(ret){
        return ret;
    }

    ret = imr_lsm_delete(key);
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
    int ret;

    len = min(count, sizeof(buf) - 1);
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

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_compact_level_locked(level);
    mutex_unlock(&imr_lsm_lock);
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

    len = min(count, sizeof(buf) - 1);
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

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_compact_selected_segment_locked();
    mutex_unlock(&imr_lsm_lock);
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

    len = min(count, sizeof(buf) - 1);
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &zone_idx);
    if(ret){
        return ret;
    }

    mutex_lock(&imrsim_zone_lock);
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_compact_zone_locked(zone_idx);
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
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
    int ret;

    len = min(count, sizeof(buf) - 1);
    if(copy_from_user(buf, ubuf, len)){
        return -EFAULT;
    }
    buf[len] = '\0';

    ret = kstrtouint(buf, 0, &zone_idx);
    if(ret){
        return ret;
    }

    mutex_lock(&imrsim_zone_lock);
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_seed_full_zone_locked(zone_idx);
    if(!ret){
        ret = imr_lsm_auto_run_zone_compaction_locked(zone_idx);
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    if(ret){
        return ret;
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
    int ret;

    len = min(count, sizeof(buf) - 1);
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
    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    imr_lsm_meta.zone_compaction_auto_run = enabled ? 1 : 0;
    if(enabled &&
       imr_lsm_meta.stats.zone_compaction_candidate_zone !=
       IMR_LSM_ZONE_COMPACTION_NONE){
        candidate_zone =
            imr_lsm_meta.stats.zone_compaction_candidate_zone;
        ret = imr_lsm_auto_run_zone_compaction_locked(candidate_zone);
    }
    mutex_unlock(&imr_lsm_lock);
    mutex_unlock(&imrsim_zone_lock);
    if(ret){
        return ret;
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

    len = min(count, sizeof(buf) - 1);
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

    mutex_lock(&imr_lsm_lock);
    if(!imr_lsm_meta.initialized){
        imr_lsm_initialize_metadata_locked();
    }

    ret = imr_lsm_commit_output_locked(run);
    mutex_unlock(&imr_lsm_lock);
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
    imrsim_reset_stats();  
    /* To allocate space for the zone_status array and initialize it. */
    zone_status = (struct imrsim_zone_status *)&zone_state->stats.zone_stats[IMR_NUMZONES];
    for(i=0; i<IMR_NUMZONES; i++){
        zone_status[i].z_start = i;
        zone_status[i].z_length = num_sectors_zone();
        zone_status[i].z_type = Z_TYPE_CONVENTIONAL;
        zone_status[i].z_conds = Z_COND_NO_WP;
        zone_status[i].z_flag = 0;
        for(j=0;j<TOP_TRACK_NUM_TOTAL;j++){
            memset(zone_status[i].z_tracks[j].isUsedBlock,0,IMR_TOP_TRACK_SIZE*sizeof(__u8));
        }
        zone_status[i].z_map_size = 0;
        memset(zone_status[i].z_pba_map,-1,TOP_TRACK_NUM_TOTAL*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)*sizeof(int));
    }
    printk(KERN_INFO "imrsim: %s zone_status init!\n", __FUNCTION__);
    magic = (__u32 *)&zone_status[IMR_NUMZONES];
    *magic = 0xBEEFBEEF;
}

/* To initial a device. */
int imrsim_init_zone_state(__u64 sizedev)
{
    __u32 state_size;

    if(!sizedev){
        printk(KERN_ERR "imrsim: zero capacity detected\n");
        return -EINVAL;
    }
    imrsim_init_zone_default(sizedev);     /* Initialize the basic information of the zone. */
    /* zone_state should not have allocated space, if it already exists, reclaim the space. */
    if(zone_state){
        vfree(zone_state);
    }
    state_size = imrsim_state_size();
    zone_state = vzalloc(state_size);     /* Allocate memory space for zone_state */
    if(!zone_state){
        printk(KERN_ERR "imrsim: memory alloc failed for zone state\n");
        return -ENOMEM;
    }
    imrsim_init_zone_state_default(state_size);   
    imrsim_dev_idle_init();      
    imr_lsm_init_metadata();
    return 0;
}

/* read completion */
static void imrsim_read_completion(struct bio *bio, int err)
{
    if(err){
        printk(KERN_ERR "imrsim: bio read err: %d\n", err);
    }
    if(bio){
        complete((struct completion *)bio->bi_private);
    }
}

/* write completion */
static void imrsim_write_completion(struct bio *bio, int err)
{
    if(err){
        printk(KERN_ERR "imrsim: bio write err:%d\n", err);
    }
    if(bio){
        complete((struct completion *)bio->bi_private);
    }
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
    //dump_stack();   // #include<asm/ptrace.h> Debugging: View function call stacks
    int ret = 0;
    struct bio *bio = bio_alloc(GFP_NOIO, 1);

    if(!bio){
        printk(KERN_ERR "imrsim: %s bio_alloc failed\n", __FUNCTION__);
        return -EFAULT;
    }
    bio->bi_bdev = dev;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
    bio->bi_sector = lba;
    #else
    bio->bi_iter.bi_sector = lba;
    #endif
    bio_add_page(bio, page, size, 0);
    init_completion(&imrsim_completion.read_event);
    bio->bi_private = &imrsim_completion.read_event;
    bio->bi_end_io = imrsim_read_completion;
    submit_bio(READ | REQ_SYNC, bio);
    wait_for_completion(&imrsim_completion.read_event);
    ret = test_bit(BIO_UPTODATE, &bio->bi_flags);
    if(!ret){
        printk(KERN_ERR "imrsim: pstore bio read failed\n");
        ret = -EIO;
    }
    bio_put(bio);
    return ret;
}

/* write page (for meta-data) */
static int imrsim_write_page(struct block_device *dev, sector_t lba,
                            __u32 size, struct page *page)
{
    int ret = 0;
    struct bio *bio = bio_alloc(GFP_NOIO, 1);

    if(!bio){
        printk(KERN_ERR "imrsim: %s bio_alloc failed\n", __FUNCTION__);
        return -EFAULT;
    }
    bio->bi_bdev = dev;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
    bio->bi_sector = lba;
    #else
    bio->bi_iter.bi_sector = lba;
    #endif
    bio_add_page(bio, page, size, 0);
    init_completion(&imrsim_completion.write_event);
    bio->bi_private = &imrsim_completion.write_event;
    bio->bi_end_io = imrsim_write_completion;
    submit_bio(WRITE_FLUSH_FUA, bio);
    wait_for_completion(&imrsim_completion.write_event);
    ret = test_bit(BIO_UPTODATE, &bio->bi_flags);
    if(!ret){
        printk(KERN_ERR "imrsim: pstore bio write failed\n");
        ret = -EIO;
    }
    bio_put(bio);
    return ret;
}

/* End event for rmw bio */
static void imrsim_end_rmw(struct bio *bio, int err)
{
    int i;
    //struct bio_vec *bvec;

    if(bio){
        // if(bio_data_dir(bio) == WRITE){
        //     printk(KERN_INFO "imrsim: release pages.\n");
        //     bio_for_each_segment_all(bvec, bio, i)
        //         __free_page(bvec->bv_page);
        // }
        // if(bio_data_dir(bio) == READ){
        //     printk(KERN_INFO "imrsim: read bio end.\n");
        // }
        complete((struct completion *)bio->bi_private);   // rmw 同步控制
        bio_put(bio);
    }
}

/* rmw task - sub thread*/
int read_modify_write_task(void *arg)
{
    
    struct dm_target * ti = (struct dm_target *)arg;
    struct imrsim_c *c = ti->private;
    __u8 i;
    __u8 n = imrsim_rmw_task.lba_num;
    struct page *pages[2];
    void  *page_addrs[2];

    if(imrsim_rmw_task.bio)
    {
        printk(KERN_INFO "imrsim: enter rmw process and back up\n");
        // read the blocks needed to back up
        for(i=0; i<n; i++)
        {
            pages[i] = alloc_page(GFP_KERNEL);
            if(!pages[i]){
                printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
                return -ENOMEM;
            }
            page_addrs[i] = page_address(pages[i]);
            if(!page_addrs[i]){
                printk(KERN_ERR "imrsim: read page vm addr null\n");
                __free_page(pages[i]);
            }
            //printk(KERN_INFO "imrsim: page_addr:0x%lx\n", page_addrs[i]);
            memset(page_addrs[i], 0, PAGE_SIZE);
            struct bio *rbio = bio_alloc(GFP_NOIO, 1);
            init_completion(&imrsim_completion.read_event);
            rbio->bi_private = &imrsim_completion.read_event;
            rbio->bi_bdev = c->dev->bdev;
            rbio->bi_iter.bi_sector = imrsim_map_sector(ti, imrsim_rmw_task.lba[i]);
            rbio->bi_end_io = imrsim_end_rmw;
            bio_add_page(rbio, pages[i], PAGE_SIZE, 0);
            submit_bio(READ | REQ_SYNC, rbio);
            wait_for_completion(&imrsim_completion.read_event);
            cond_resched();
        }

        printk(KERN_INFO "imrsim: write bio.\n");
        // write current bio
        submit_bio(WRITE_FUA, imrsim_rmw_task.bio);
        cond_resched();

        printk(KERN_INFO "imrsim: write back.\n");
        // write back
        for(i=0; i<n; i++)
        {
            struct bio *wbio = bio_alloc(GFP_NOIO, 1);
            init_completion(&imrsim_completion.write_event);
            wbio->bi_private = &imrsim_completion.write_event;
            wbio->bi_bdev = c->dev->bdev;
            wbio->bi_iter.bi_sector = imrsim_map_sector(ti, imrsim_rmw_task.lba[i]);
            wbio->bi_end_io = imrsim_end_rmw;
            bio_add_page(wbio, pages[i], PAGE_SIZE, 0);
            submit_bio(WRITE_FUA, wbio);
            wait_for_completion(&imrsim_completion.write_event);
            cond_resched();
        }

        // release pages
        for(i=0; i<n; i++)
        {
            __free_page(pages[i]);
        }
        printk(KERN_INFO "imrsim: release pages.\n");
        imrsim_rmw_task.lba_num = 0;
        complete(&imrsim_completion.rmw_event);   // rmw completion release
    }
    return 0;
}

/* RMW event caused by update to bottom track */
void imrsim_rmw_thread(struct dm_target *ti)
{
    imrsim_rmw_task.task = kthread_create(read_modify_write_task, ti, "rmw thread");
    if(imrsim_rmw_task.task){
        printk(KERN_INFO "imrsim: rmw thread created : %d.\n", imrsim_rmw_task.task->pid);
        init_completion(&imrsim_completion.rmw_event);
        wake_up_process(imrsim_rmw_task.task);
        wait_for_completion(&imrsim_completion.rmw_event);
        //kthread_stop(imrsim_rmw_task.task);
        printk(KERN_INFO "imrsim: rmw task end.\n");
    }
}


/* To get page number and next page. */
static __u32 imrsim_pstore_pg_idx(__u32 idx, __u32 *pg_nxt)
{
    __u32 tmp = IMR_PSTORE_PG_OFF + sizeof(struct imrsim_zone_stats)*idx;
    __u32 pg_cur = tmp / PAGE_SIZE;

    *pg_nxt = tmp % PAGE_SIZE ? pg_cur+1 : pg_cur;
    return pg_cur;
}

/* Persistent storage - handled in a variety of cases depending on the type of metadata change.*/
static int imrsim_flush_persistence(struct dm_target *ti)
{
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            idx;
    __u32            crc;    
    __u32            pg_cur;
    __u32            pg_nxt;
    __u32            qidx;

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
    crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header),
                zone_state->header.length - sizeof(struct imrsim_state_header));
    zone_state->header.crc32 = crc;
    if(imrsim_ptask.flag &= IMR_CONFIG_CHANGE){
        imrsim_ptask.flag &= ~IMR_CONFIG_CHANGE;
    }
    memcpy(page_addr, (unsigned char *)zone_state, PAGE_SIZE);
    imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba, PAGE_SIZE, page);

    /* Handling of Disk Statistics Changes. */
    if(imrsim_ptask.flag &= IMR_STATS_CHANGE){
        imrsim_ptask.flag &= ~IMR_STATS_CHANGE;
        if(imrsim_ptask.sts_zone_idx > IMR_PSTORE_PG_EDG){
            pg_cur = imrsim_pstore_pg_idx(imrsim_ptask.sts_zone_idx, &pg_nxt);
            imrsim_ptask.sts_zone_idx = 0;
            for(idx = pg_cur; idx <= pg_nxt; idx++){
                memcpy(page_addr, ((unsigned char *)zone_state + idx * PAGE_SIZE), PAGE_SIZE);
                imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT),
                                PAGE_SIZE, page);
            }
        }
    }

    /* Handling of disk state changes. */
    if(imrsim_ptask.flag &= IMR_STATUS_CHANGE){
        imrsim_ptask.flag &= ~IMR_STATUS_CHANGE;
        for(qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++){
            pg_cur = imrsim_pstore_pg_idx(imrsim_ptask.stu_zone_idx[qidx], &pg_nxt);
            imrsim_ptask.stu_zone_idx[qidx] = 0;
            for(idx = pg_cur; idx <= pg_nxt; idx++){
                memcpy(page_addr, ((unsigned char *)zone_state + idx * PAGE_SIZE), PAGE_SIZE);
                imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT),
                                PAGE_SIZE, page);
            }
        }
        imrsim_ptask.stu_zone_idx_cnt = 0;
        imrsim_ptask.stu_zone_idx_gap = 0;
    }

    if(imrsim_dbg_log_enabled && printk_ratelimit()){
        printk(KERN_ERR "imrsim: flush persist success\n");
    }
    __free_pages(page, 0);
    return 0;
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
        imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                         (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
    }
    if(part_page){
        memcpy(page_addr, ((unsigned char *)zone_state + 
              num_pages * PAGE_SIZE), part_page);
        imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                          (num_pages << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
    }
    if(imrsim_dbg_log_enabled && printk_ratelimit()){
        printk(KERN_INFO "imrsim: save persist success\n");
    }
    __free_pages(page, 0);
    return 0;
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
    struct imrsim_state_header header;

    printk(KERN_INFO "imrsim: load persistence\n");

    zdev = ti->private;
    sizedev = ti->len;
    imrsim_init_zone_default(sizedev);
    /* The starting address for persistent storage. */
    imrsim_ptask.pstore_lba = IMR_NUMZONES_DEFAULT
                              << IMR_ZONE_SIZE_SHIFT_DEFAULT
                              << IMR_BLOCK_SIZE_SHIFT_DEFAULT;
    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        goto pgerr;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: read page vm addr null\n");
        goto rderr;
    }
    memset(page_addr, 0, PAGE_SIZE);
    imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba, PAGE_SIZE, page);
    memcpy(&header, page_addr, sizeof(struct imrsim_state_header));
    if(header.magic == 0xBEEFBEEF){
        zone_state = vzalloc(header.length);
        if(!zone_state){
            printk(KERN_ERR "imrsim: zone_state error: no enough memory\n");
            goto rderr;
        }
        num_pages = div_u64_rem(header.length, PAGE_SIZE, &part_page);
        if(num_pages){
            memcpy((unsigned char *)zone_state, page_addr, PAGE_SIZE);  // load header
        }
        for(idx = 1; idx < num_pages; idx++){
            memset(page_addr, 0, PAGE_SIZE);
            imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                            (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
            memcpy(((unsigned char *)zone_state + 
                  idx * PAGE_SIZE), page_addr, PAGE_SIZE);
        }
        if(part_page){
            if(num_pages){
                memset(page_addr, 0, PAGE_SIZE);
                imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (num_pages << IMR_PAGE_SIZE_SHIFT_DEFAULT), 
                                PAGE_SIZE, page);
            }
            memcpy(((unsigned char *)zone_state + 
                   idx * PAGE_SIZE), page_addr, part_page);
        }
        crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header), 
                   zone_state->header.length - sizeof(struct imrsim_state_header));
        if(crc != zone_state->header.crc32){
            printk(KERN_ERR "imrsim: error: crc checking. apply default config ...\n");
            goto rderr;
        }
        IMR_NUMZONES = zone_state->stats.num_zones;
        zone_status = (struct imrsim_zone_status *)&zone_state->stats.zone_stats[IMR_NUMZONES];
        IMR_ZONE_SIZE_SHIFT = index_power_of_2(zone_status[0].z_length >> IMR_BLOCK_SIZE_SHIFT);
        printk(KERN_INFO "imrsim: load persist success\n");
    }else{
        printk(KERN_ERR "imrsim: load persistence magic doesn't match. Setup the default\n");
        goto rderr;
    }
    __free_pages(page, 0);
    return 0;
    rderr:
        __free_pages(page, 0);
    pgerr:
        imrsim_init_zone_state(sizedev);
    return -EINVAL;
}

/* persistent storage task */
static int imrsim_persistence_task(void *arg)
{
    struct dm_target *ti = (struct dm_target *)arg;

    while(!kthread_should_stop()){
        if(imrsim_ptask.flag){
            mutex_lock(&imrsim_zone_lock);
            if(imrsim_ptask.flag & IMR_CONFIG_CHANGE){
                if(IMR_NUMZONES == 0){
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                }else{
                    imrsim_save_persistence(ti);
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                }
            }else{
                if(imrsim_ptask.stu_zone_idx_gap >= IMR_PSTORE_PG_GAP){
                    imrsim_save_persistence(ti);
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                    imrsim_ptask.stu_zone_idx_gap = 0;
                    memset(imrsim_ptask.stu_zone_idx, 0, sizeof(__u32) * IMR_PSTORE_QDEPTH);
                    imrsim_ptask.stu_zone_idx_cnt = 0;
                }else{
                    imrsim_flush_persistence(ti);
                }
            }
            mutex_unlock(&imrsim_zone_lock);
        }
        msleep_interruptible(IMR_PSTORE_CHECK);
    }
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
    if(ret){
        imrsim_save_persistence(ti);
    }
    // create thread
    imrsim_ptask.pstore_thread = kthread_create(imrsim_persistence_task, 
                                                ti, "imrsim pthread");
    if(imrsim_ptask.pstore_thread){
        printk(KERN_INFO "imrsim persistence thread created\n");
        // After a thread is created with kthread_create, the thread will not start immediately, 
		// but needs to be started after calling the wake_up_process function.
        wake_up_process(imrsim_ptask.pstore_thread);
    }else{
        printk(KERN_ERR "imrsim persistence thread create failed\n");
        return -EAGAIN;
    }
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
    *size_zone = num_sectors_zone();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_size_zone_default);

/* To set the default zone size. */
int imrsim_set_size_zone_default(__u32 size_zone)
{
    struct imrsim_state *sta_tmp;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if((size_zone % (1 << IMR_BLOCK_SIZE_SHIFT)) || !(is_power_of_2(size_zone))){
        printk(KERN_ERR "imrsim: Wrong zone size specified\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    IMR_ZONE_SIZE_SHIFT = index_power_of_2((size_zone) >> IMR_BLOCK_SIZE_SHIFT);
    IMR_NUMZONES = ((IMR_CAPACITY >> IMR_BLOCK_SIZE_SHIFT) >> IMR_ZONE_SIZE_SHIFT);
    sta_tmp = vzalloc(imrsim_state_size());
    if(!sta_tmp){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -EINVAL;
    }
    vfree(zone_state);
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(imrsim_state_size());
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_size_zone_default);

/* To reset default config. */
int imrsim_reset_default_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    imrsim_reset_default_zone_config();
    imrsim_reset_default_device_config();
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_config);

/* To reset default device config. */
int imrsim_reset_default_device_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.out_of_policy_read_flag = 0;
    zone_state->config.dev_config.out_of_policy_write_flag = 0;
    zone_state->config.dev_config.r_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    zone_state->config.dev_config.w_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
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
    zone_state->config.dev_config.out_of_policy_read_flag = 
        device_config->out_of_policy_read_flag;
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
    zone_state->config.dev_config.out_of_policy_write_flag = 
        device_config->out_of_policy_write_flag;
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
    zone_state->config.dev_config.r_time_to_rmw_zone = 
        device_config->r_time_to_rmw_zone;
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
    zone_state->config.dev_config.w_time_to_rmw_zone = 
        device_config->w_time_to_rmw_zone;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_wconfig_delay);

/* To reset default zone config. */
int imrsim_reset_default_zone_config(void)
{
    struct imrsim_state *sta_tmp;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    IMR_NUMZONES = IMR_NUMZONES_DEFAULT;
    IMR_ZONE_SIZE_SHIFT = IMR_ZONE_SIZE_SHIFT_DEFAULT;
    sta_tmp = vzalloc(imrsim_state_size());
    vfree(zone_state);
    if(!sta_tmp){
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -EINVAL;
    }
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(imrsim_state_size());
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_zone_config);

/* To clear config of a zone. */
int imrsim_clear_zone_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    memset(zone_state->stats.zone_stats, 0, 
       zone_state->stats.num_zones * sizeof(struct imrsim_zone_stats));
    mutex_lock(&imrsim_zone_lock);
    zone_state->stats.num_zones = 0;
    memset(zone_status, 0, IMR_NUMZONES * sizeof(struct imrsim_zone_status));
    IMR_NUMZONES = 0;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
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
    __u32 count = imrsim_zone_seq_count();

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__); 
    if(!z_status){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(IMR_NUMZONES <= z_status->z_start){
        printk(KERN_ERR "imrsim: config does not exist\n");
        return -EINVAL;
    }
    if(1 >= count && (Z_TYPE_SEQUENTIAL == z_status->z_type) &&
      (Z_TYPE_SEQUENTIAL == zone_status[z_status->z_start].z_type))
    {
          printk(KERN_ERR "imrsim: zone type is not allowed to modify\n");
          return -EINVAL;
    }
    if(z_status->z_length != num_sectors_zone()){
        printk(KERN_ERR "imrsim: zone size is not allowed to change individually\n");
        return -EINVAL;
    }
    if(!imrsim_zone_cond_check(z_status->z_conds)){
        printk(KERN_ERR "imrsim: wrong zone condition\n");
        return -EINVAL;
    }
    if((z_status->z_conds == Z_COND_NO_WP) && 
        (z_status->z_type != Z_TYPE_CONVENTIONAL))
    {
        printk(KERN_ERR "imrsim: condition and type mismatch\n");
        return -EINVAL;
    }
    if ((Z_COND_EMPTY == z_status->z_conds) && 
       (Z_TYPE_SEQUENTIAL == z_status->z_type) ) {
      printk(KERN_ERR "imrsim: empty zone isn't empty\n");
      return -EINVAL;
    }

    mutex_lock(&imrsim_zone_lock);
    zone_status[z_status->z_start].z_conds = 
      (enum imrsim_zone_conditions)z_status->z_conds;
    zone_status[z_status->z_start].z_type = 
      (enum imrsim_zone_type)z_status->z_type;
    zone_status[z_status->z_start].z_flag = 0;
    mutex_unlock(&imrsim_zone_lock);
    printk(KERN_DEBUG "imrsim: zone[%lu] modified. type:0x%x conds:0x%x\n",
      zone_status[z_status->z_start].z_start,
      zone_status[z_status->z_start].z_type, 
      zone_status[z_status->z_start].z_conds);
    return 0;
}
EXPORT_SYMBOL(imrsim_modify_zone_config);

/* To add zone configuration. @Deprecated */
int imrsim_add_zone_config(struct imrsim_zone_status *zone_sts)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!zone_sts){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(zone_sts->z_start >= IMR_NUMZONES_DEFAULT){
        printk(KERN_ERR "imrsim: zone config start lba is out of range\n");
        return -EINVAL;
    }
    if(zone_sts->z_start != IMR_NUMZONES){
        printk(KERN_ERR "imrsim: zone config does not start at the end of current zone\n");
        printk(KERN_INFO "imrsim: z_start: %u  IMR_NUMZONES: %u\n", (__u32)zone_sts->z_start,
             IMR_NUMZONES);
        return -EINVAL;
    }
    if ((zone_sts->z_type != Z_TYPE_CONVENTIONAL) && (zone_sts->z_type != Z_TYPE_SEQUENTIAL)) {
      printk(KERN_ERR "imrsim: zone config type is not allowed with current config\n");
      return -EINVAL;
   }
   if ((zone_sts->z_type == Z_TYPE_CONVENTIONAL) && (zone_sts->z_conds != Z_COND_NO_WP)) {
      printk(KERN_ERR "imrsim: zone config condition is wrong. Need to be NO WP\n");
      return -EINVAL;
   }
   if ((zone_sts->z_type == Z_TYPE_SEQUENTIAL) && (zone_sts->z_conds != Z_COND_EMPTY)) {
      printk(KERN_ERR "imrsim: zone config condition is wrong. Need to be EMPTY\n");
      return -EINVAL;
   }
   if (zone_sts->z_length != (1 << IMR_ZONE_SIZE_SHIFT << IMR_BLOCK_SIZE_SHIFT)) {
      printk(KERN_ERR "imrsim: zone config size is not allowed with current config\n");
      return -EINVAL;
   }
   zone_sts->z_flag = 0;
   mutex_lock(&imrsim_zone_lock);
   memcpy(&(zone_status[IMR_NUMZONES]), zone_sts, sizeof(struct imrsim_zone_status));
   zone_state->stats.num_zones++;
   IMR_NUMZONES++;
   mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_add_zone_config);

/* To reset statistics for a zone. */
int imrsim_reset_zone_stats(sector_t start_sector)
{
    __u32 zone_idx = start_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(IMR_NUMZONES <= zone_idx){
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
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_zone_stats);

/* To reset zone_stats. */
int imrsim_reset_stats(void)
{
    printk(KERN_INFO "imrsim: %s: called.\n", __FUNCTION__);
    memset(&zone_state->stats.dev_stats.idle_stats, 0, sizeof(struct imrsim_idle_stats));
    memset(&zone_state->stats.extra_write_total, 0, sizeof(__u64));
    memset(&zone_state->stats.write_total, 0, sizeof(__u64));
    memset(zone_state->stats.zone_stats, 0, zone_state->stats.num_zones * 
          sizeof(struct imrsim_zone_stats));
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
    memcpy(stats, &(zone_state->stats), imrsim_stats_size());
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
    int iRet;
    char dummy;
    struct imrsim_c *c = NULL;
    __u64 num;

    printk(KERN_INFO "imrsim: %s called\n", __FUNCTION__);
    if(imrsim_single){
        printk(KERN_ERR "imrsim: No multiple device support currently\n");
        return -EINVAL;
    }
    if(!ti){
        printk(KERN_ERR "imrsim: error: invalid device\n");
        return -EINVAL;
    }
    if(2 != argc){
        ti->error = "dm-imrsim: error: invalid argument count; !=2";
        return -EINVAL;
    }
    if(1 != sscanf(argv[1], "%llu%c", &tmp, &dummy)){
        ti->error = "dm-imrsim: error: invalid argument device sector";
        return -EINVAL;
    }
    c = kmalloc(sizeof(*c), GFP_KERNEL);    // To allocate physically contiguous memory.
    if(!c){
        ti->error = "dm-imrsim: error: no enough memory";
        return -ENOMEM;
    }
    c->start = tmp;
    // Fill in the bdev of the device specified by path and the corresponding interval, permission, mode, etc. into ti->table.
    iRet = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
    if(iRet){
        ti->error = "dm-imrsim: error: device lookup failed";
        kfree(c);
        return iRet;
    }
    if(ti->len > IMR_MAX_CAPACITY){
        printk(KERN_ERR "imrsim: capacity %llu exceeds the maximum 10TB\n", (__u64)ti->len);
        kfree(c);
        return -EINVAL;
    }
    num = ti->len >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if((num << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT) != ti->len){
        printk(KERN_ERR "imrsim:error: total size must be zone size (256MB) aligned\n");
    }
    if (ti->len < (1 << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT)) {
      printk(KERN_INFO "imrsim: capacity: %llu sectors\n", (__u64)ti->len);
      printk(KERN_ERR "imrsim:error: capacity is too small. The default config is multiple of 256MB\n"); 
      kfree(c);
      return -EINVAL;
   }
   ti->num_flush_bios = ti->num_discard_bios = ti->num_write_same_bios = 1;
   ti->private = c;
   mutex_lock(&imr_lsm_lock);
   imr_lsm_output_bdev = c->dev->bdev;
   imr_lsm_output_bdev_start = c->start;
   mutex_unlock(&imr_lsm_lock);
   imrsim_dbg_rerr = imrsim_dbg_werr = imrsim_dbg_log_enabled = 0;
   mutex_init(&imrsim_zone_lock);
   mutex_init(&imrsim_ioctl_lock);
   // To open a persistent thread.
   if(imrsim_persistence_thread(ti)){
       printk(KERN_ERR "imrsim: error: metadata will not be persisted\n");
   }
   imrsim_single = 1;
   return 0;
}

/* device destory */
static void imrsim_dtr(struct dm_target *ti)
{
    struct imrsim_c *c = (struct imrsim_c *) ti->private;

    kthread_stop(imrsim_ptask.pstore_thread);  // To kill the persistent thread.
    mutex_destroy(&imrsim_zone_lock);
    mutex_destroy(&imrsim_ioctl_lock);
    mutex_lock(&imr_lsm_lock);
    imr_lsm_output_bdev = NULL;
    imr_lsm_output_bdev_start = 0;
    mutex_unlock(&imr_lsm_lock);
    dm_put_device(ti, c->dev);
    kfree(c);
    imr_lsm_release_metadata();
    vfree(zone_state);
    imrsim_single = 0;
    printk(KERN_INFO "imrsim target destructed\n");
}

/* Device Write Rules */
int imrsim_write_rule_check(struct bio *bio, __u32 zone_idx,
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
    if(bio->bi_private != &imrsim_completion.write_event)
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
						printk(KERN_INFO "imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
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
						printk(KERN_INFO "imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}
				}else{            // lba is in the mapping table, indicating an update operation
					// Get pba from the mapping table, modify lba in bio
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					printk(KERN_INFO "imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
					lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
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
						printk(KERN_INFO "imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
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
						printk(KERN_INFO "imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
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
						printk(KERN_INFO "imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
						if(lsm_ret){
							return lsm_ret;
						}
						lba = bio->bi_sector;
					}
				}else{            // an update operation
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					printk(KERN_INFO "imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_sector);
					lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_sector);
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
                        printk(KERN_INFO "imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
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
                        printk(KERN_INFO "imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // lba is in the mapping table, indicating an update operation
                    // Get pba from the mapping table, modify lba in bio
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    printk(KERN_INFO "imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                    lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
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
                        printk(KERN_INFO "imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
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
                        printk(KERN_INFO "imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
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
                        printk(KERN_INFO "imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
                        if(lsm_ret){
                            return lsm_ret;
                        }
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // an update operation
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    printk(KERN_INFO "imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_iter.bi_sector);
                    lsm_ret = imr_lsm_record_insert_and_check_zone_full(zone_idx, lba, bio->bi_iter.bi_sector);
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
        printk(KERN_INFO "imrsim DIRECT write option.\n");
    }
    
    physical_zone_idx = imrsim_lba_zone_idx((sector_t)lba);
    if(physical_zone_idx >= IMR_NUMZONES){
        printk(KERN_ERR "imrsim: remapped write lba is out of range. zone_idx: %u\n",
               physical_zone_idx);
        return IMR_ERR_OUT_RANGE;
    }
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
    printk(KERN_INFO "imrsim: %s trackno: %u, isTopTrack: %u.\n",__FUNCTION__, trackno, isTopTrack);

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
            printk(KERN_INFO "imrsim: write amplification(zone_idx[%u]trackno), block: %u .\n",physical_zone_idx, (__u32)(blockno*10000/trackrate));
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
            printk(KERN_INFO "imrsim: write amplification(trackno+1), block: %u .\n", (__u32)(blockno*10000/trackrate));
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
            printk(KERN_INFO "imrsim: WA, wa_pba_1:%d,wa_pba_2:%d.\n", wa_pba1, wa_pba2);
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

    zlba = zone_idx_lba(zone_idx);

    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    lba = bio->bi_sector;
    block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT;
    lsm_lookup = imr_lsm_read(lba >> IMR_BLOCK_SIZE_SHIFT, &lsm_pba);
    if(lsm_lookup == IMR_LSM_LOOKUP_VALID){
        bio->bi_sector = lsm_pba + ((lba-zlba) % (1 << IMR_BLOCK_SIZE_SHIFT));
        printk(KERN_INFO "imrsim: IMR-LSM read hit zone %u key=%llu pba=%llu\n",
               zone_idx,
               (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT),
               (unsigned long long)bio->bi_sector);
        lba = bio->bi_sector;
    }else if(lsm_lookup == IMR_LSM_LOOKUP_DELETED){
        printk(KERN_INFO "imrsim: IMR-LSM read deleted zone %u key=%llu\n",
               zone_idx,
               (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT));
        rv++;
    }else if(zone_status[zone_idx].z_pba_map[block_offset] != -1){
        imr_lsm_record_fallback();
        bio->bi_sector = zlba 
            + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
        printk(KERN_INFO "imrsim: read_ops on zone %u - start lba is %llu, pba is %llu\n", zone_idx, lba, bio->bi_sector); 
        lba = bio->bi_sector;
    }else{
        rv++;
    }
    #else
    lba = bio->bi_iter.bi_sector;
    if(bio->bi_private != &imrsim_completion.read_event)
    {
        block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT;
        lsm_lookup = imr_lsm_read(lba >> IMR_BLOCK_SIZE_SHIFT, &lsm_pba);
        if(lsm_lookup == IMR_LSM_LOOKUP_VALID){
            bio->bi_iter.bi_sector = lsm_pba
                + ((lba-zlba) % (1 << IMR_BLOCK_SIZE_SHIFT));
            printk(KERN_INFO "imrsim: IMR-LSM read hit zone %u key=%llu pba=%llu\n",
                   zone_idx,
                   (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT),
                   (unsigned long long)bio->bi_iter.bi_sector);
            lba = bio->bi_iter.bi_sector;
        }else if(lsm_lookup == IMR_LSM_LOOKUP_DELETED){
            printk(KERN_INFO "imrsim: IMR-LSM read deleted zone %u key=%llu\n",
                   zone_idx,
                   (unsigned long long)(lba >> IMR_BLOCK_SIZE_SHIFT));
            rv++;
        }else if(zone_status[zone_idx].z_pba_map[block_offset] != -1){
            imr_lsm_record_fallback();
            bio->bi_iter.bi_sector = zlba 
                + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT)
                + (lba-zlba)%(1<<IMR_BLOCK_SIZE_SHIFT);
            printk(KERN_INFO "imrsim: read_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT); 
            lba = bio->bi_iter.bi_sector;
        }else{
            rv++;
            //printk(KERN_ERR "imrsim: read none data\n"); 
        }
    }else{
        printk(KERN_INFO "imrsim DIRECT read option.\n");
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
    return 0;
}

static bool imrsim_ptask_queue_ok(__u32 idx)
{
   __u32 qidx;
   
    for (qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++) {
       if (abs(idx - (imrsim_ptask.stu_zone_idx[qidx]))
          <= IMR_PSTORE_PG_EDG) {
          return false;
       }
    }
    return true;
}

static bool imrsim_ptask_gap_ok(__u32 idx)
{
   __u32 qidx;
   
    for (qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++) {
       if (abs(idx - (imrsim_ptask.stu_zone_idx[qidx]))
          <= IMR_PSTORE_PG_GAP * IMR_PSTORE_PG_EDG) {
          return true;
       }
    }
    return false;
}

/* I/O mapping */
int imrsim_map(struct dm_target *ti, struct bio *bio)
{
    struct imrsim_c *c = ti->private;
    int cdir = bio_data_dir(bio);     

    if(bio){
        printk(KERN_INFO "imrsim_map: the bio has %u sectors.\n", bio_sectors(bio));
    }

    sector_t bio_sectors = bio_sectors(bio);
    int policy_rflag = 0;
    int policy_wflag = 0;
    int ret = 0;
    unsigned int penalty;
    __u32 zone_idx;
    __u64 lba;

    mutex_lock(&imrsim_zone_lock);
    //printk(KERN_INFO "zone_lock.\n");
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    zone_idx = bio->bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_sector;
    #else
    zone_idx = bio->bi_iter.bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_iter.bi_sector;
    #endif

    //printk(KERN_INFO "imrsim: map- lba is %llu\n", lba);

    imrsim_dev_idle_update();

    if(IMR_NUMZONES <= zone_idx){
        printk(KERN_ERR "imrsim: lba is out of range. zone_idx: %u\n", zone_idx);
        imrsim_log_error(bio, IMR_ERR_OUT_RANGE);
        goto nomap;
    }
    if(imrsim_dbg_log_enabled){
        printk(KERN_DEBUG "imrsim: %s bio_sectors=%llu\n", __FUNCTION__, 
                (unsigned long long)bio_sectors);
    }
    if((lba + bio_sectors) > (zone_idx_lba(zone_idx) + 2 * num_sectors_zone())){
        printk(KERN_ERR "imrsim: error: %s bio_sectors() is too large\n", __FUNCTION__);
        imrsim_log_error(bio, IMR_ERR_OUT_OF_POLICY);
        goto nomap;
    }
    if(zone_status[zone_idx].z_conds == Z_COND_OFFLINE){
        printk(KERN_ERR "imrsim: error: zone is offline. zone_idx:%u\n", zone_idx);
        imrsim_log_error(bio, IMR_ERR_ZONE_OFFLINE);
        goto nomap;
    }
    bio->bi_bdev = c->dev->bdev;
    policy_rflag = zone_state->config.dev_config.out_of_policy_read_flag;
    policy_wflag = zone_state->config.dev_config.out_of_policy_write_flag;
    
    // read or write ?
    if(cdir == WRITE){
        if(imrsim_dbg_log_enabled){
            printk(KERN_DEBUG "imrsim: %s WRITE %u.%012llx:%08lx.\n", __FUNCTION__,
                zone_idx, lba, bio_sectors);
        }
        if ((zone_status[zone_idx].z_conds == Z_COND_RO) && !policy_wflag) {
            printk(KERN_ERR "imrsim:error: zone is read only. zone_idx: %u\n", zone_idx);  
            imrsim_log_error(bio, IMR_ERR_WRITE_RO);
            goto nomap;
        }
        if ((zone_status[zone_idx].z_conds == Z_COND_FULL) &&
            (lba != zone_idx_lba(zone_idx)) && !policy_wflag) {
            printk(KERN_ERR "imrsim:error: zone is full. zone_idx: %u\n", zone_idx);
            imrsim_log_error(bio, IMR_ERR_WRITE_FULL);
            goto nomap;
        }
        ret = imrsim_write_rule_check(bio, zone_idx, bio_sectors, policy_wflag);
        if(ret<0){
            if(policy_wflag == 1 && policy_rflag == 1){
                goto mapped;
            }
            penalty = 0;
            if(policy_wflag == 1){
                penalty = zone_state->config.dev_config.w_time_to_rmw_zone;
                printk(KERN_ERR "imrsim: %s: write error passed: out of policy write flagged on\n", __FUNCTION__);
                udelay(penalty);
            }else{
                goto nomap;
            }
        }
        if(ret>0){
            goto submitted;
        }
        imrsim_ptask.flag |= IMR_STATUS_CHANGE;
        if(imrsim_ptask.stu_zone_idx_cnt == IMR_PSTORE_QDEPTH){
            imrsim_ptask.stu_zone_idx_gap = IMR_PSTORE_PG_GAP;
        }else if(imrsim_ptask_queue_ok(zone_idx)){
            imrsim_ptask.stu_zone_idx[imrsim_ptask.stu_zone_idx_cnt] = zone_idx;
            imrsim_ptask.stu_zone_idx_cnt++;
            if(!imrsim_ptask_gap_ok(zone_idx)){
                imrsim_ptask.stu_zone_idx_gap++;
            }
        }
    }
    else if(cdir == READ){
        if (imrsim_dbg_log_enabled) {
            printk(KERN_DEBUG "imrsim: %s READ %u.%012llx:%08lx.\n", __FUNCTION__,
                    zone_idx, lba, bio_sectors);
        }
        ret = imrsim_read_rule_check(bio, zone_idx, bio_sectors, policy_rflag);
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
    printk(KERN_INFO "imrsim_map: submitted and conduct rmw!\n");
    imrsim_rmw_task.bio = bio;
    imrsim_rmw_thread(ti);
    mutex_unlock(&imrsim_zone_lock);
    printk(KERN_INFO "imrsim_map: end rmw!\n");
    return DM_MAPIO_SUBMITTED;

    nomap:
    imrsim_ptask.flag |= IMR_STATS_CHANGE;
    imrsim_ptask.sts_zone_idx = zone_idx;
    mutex_unlock(&imrsim_zone_lock);
    //printk(KERN_INFO "zone_unlock.\n");
    return IMR_DM_IO_ERR;
}

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

    if(!num_zones || !ptr){
        printk(KERN_ERR "imrsim: NULL pointer passed through.\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_idx = lba >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if(0 == *num_zones || IMR_NUMZONES < (*num_zones + zone_idx)){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: number of zone out of range\n");
        return -EINVAL;
    }
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
    __u32                      param = IMR_NUMZONES;
    
    imrsim_dev_idle_update();
    mutex_lock(&imrsim_ioctl_lock);
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        /* IMRSIM config IOCTLs */
        case IOCTL_IMRSIM_RESET_DEFAULTCONFIG:
            if(imrsim_reset_default_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_ZONECONFIG:
            if(imrsim_reset_default_zone_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_DEVCONFIG:
            if(imrsim_reset_default_device_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
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
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        default:
            break;
    }
    if(imrsim_ptask.flag & IMR_CONFIG_CHANGE){
        wake_up_process(imrsim_ptask.pstore_thread);
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

/* Core structure - represents the target-driven plug-in, 
and the structure collects the function entry for the functions implemented by the driver plug-in */
static struct target_type imrsim_target = 
{
    .name            = "imrsim",
    .version         = {1, 0, 0},
    .module          = THIS_MODULE,
    .ctr             = imrsim_ctr,
    .dtr             = imrsim_dtr,
    .map             = imrsim_map,
    .status          = imrsim_status,
    .ioctl           = imrsim_ioctl,
    .merge           = imrsim_merge,
    .iterate_devices = imrsim_iterate_devices
};

/* init IMRSim module */
static int __init dm_imrsim_init(void)
{
    int ret = 0;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    ret = dm_register_target(&imrsim_target);
    if(ret < 0){
        printk(KERN_ERR "imrsim: register failed\n");
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
}

module_init(dm_imrsim_init);    
module_exit(dm_imrsim_exit);    

/* Module related signature information */
MODULE_DESCRIPTION(DM_NAME "IMR Simulator");
MODULE_AUTHOR("Zhimin Zeng <im_zzm@126.com>");
MODULE_LICENSE("GPL");
