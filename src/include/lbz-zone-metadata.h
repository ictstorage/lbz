#ifndef _LBZ_ZONE_METADATA_H_
#define _LBZ_ZONE_METADATA_H_
#include "lbz-common.h"

/*
 * lbz creates block devices with 4KB blocks, always.
 */
#define LBZ_BLOCK_SHIFT		12
#define LBZ_BLOCK_SIZE		(1 << LBZ_BLOCK_SHIFT)
#define LBZ_BLOCK_MASK		(LBZ_BLOCK_SIZE - 1)

#define LBZ_BLOCK_SHIFT_BITS	(LBZ_BLOCK_SHIFT + 3)
#define LBZ_BLOCK_SIZE_BITS	(1 << LBZ_BLOCK_SHIFT_BITS)
#define LBZ_BLOCK_MASK_BITS	(LBZ_BLOCK_SIZE_BITS - 1)

#define LBZ_BLOCK_SECTORS_SHIFT	(LBZ_BLOCK_SHIFT - SECTOR_SHIFT)
#define LBZ_BLOCK_SECTORS	(LBZ_BLOCK_SIZE >> SECTOR_SHIFT)
#define LBZ_BLOCK_SECTORS_MASK	(LBZ_BLOCK_SECTORS - 1)
/*
 * Zone flags.
 */
enum lbz_zone_state {
	LBZ_ZONE_OFFLINE,
	LBZ_ZONE_READ_ONLY,
	/* Zone list state */
	LBZ_ZONE_INIT,
	LBZ_ZONE_EMPTY, /*not in use.*/
	LBZ_ZONE_ACTIVE,
	LBZ_ZONE_PARITAL, /*not in use.*/
	LBZ_ZONE_TO_FULL, /*write full.*/
	LBZ_ZONE_FULL,
	LBZ_ZONE_GC, /* Not in any list. */
	LBZ_ZONE_GC_COMPLETED /* Not in any list. */
};

/*
 * gc because of emergency or timeout
 */
enum lbz_victim_mod {
	LBZ_VICTIM_DEFAULT,
	LBZ_VICTIM_REGULAR,
	LBZ_VICTIM_EMERGENCY
};

#define LBZ_REVERSE_MAP_BLK_SHIFT (12)
#define LBZ_REVERSE_MAP_BLK_SIZE (1 << (LBZ_REVERSE_MAP_BLK_SHIFT))
#define LBZ_REVERSE_MAP_ENTRIES (LBZ_REVERSE_MAP_BLK_SIZE / sizeof(unsigned int))

#define LBZ_BITMAP_BLK_SHIFT (12)
#define LBZ_BITMAP_BLK_SIZE (1 << (LBZ_BITMAP_BLK_SHIFT))
#define LBZ_BITMAP_BITS_PER_BLK (1 << (LBZ_BITMAP_BLK_SHIFT + 3))

struct zone_reverse_mapping {
	unsigned int blks[LBZ_REVERSE_MAP_ENTRIES];
};

struct lbz_device;

/*
 * Zone descriptor.
 */
struct lbz_zone {
	/* For listing the zone depending on its state */
	struct list_head link;

	/*
	 * lock only protect state, which may be modified by multi context:
	 *	1. zone state thread
	 *	2. lbz_put_zone
	 */
	spinlock_t lock;
	enum blk_zone_cond state;

	/* internal zone state */
	unsigned long flags;

	/* Zone activation reference count */
	atomic_t refcount;

	/*This zone afford which stream write, init is -1.*/
	int stream;

	/* Zone id */
	unsigned int id;

	/* Zone write pointer block (relative to the zone start block) */
	unsigned int wp_block;

	/*variable in blk_zone.*/
	unsigned int start_sector; /*u64 in blk_zone.*/
	unsigned int len_sectors; /*number of sectors, u64 in blk_zone.*/
	unsigned int capacity_sectors; /*capacity in sectors.*/
	unsigned int cond;

	/* Zone weight (number of valid blocks in the zone) */
	atomic_t weight;

	atomic_t pending_write_io;

	/* Zone reverse mapping. */
    struct zone_reverse_mapping **zrms;

	/* Zone valid bitmap. */
	unsigned long **zone_valid_bitmap;
};

#define LBZ_ZONE_STATE_EXPIRE (5 * HZ)
/*20% of total blks except reserved_blks_gc can be alloced to user write.*/
#define LBZ_GC_RECLAIM_DEFAULT_WM_LOW (2)
#define LBZ_GC_RECLAIM_DEFAULT_WM_HIGH (4)
#define LBZ_ZONE_RECLAIM_DEFAULT_WM (1)
#define LBZ_ZONE_MAX_STREAM (2)
struct lbz_zone_metadata {
	/*Use zone index zones array.*/
	struct lbz_zone **zones;

	struct list_head empty_zone_list;
	/* Full zone and partial zone list.
	 * GC may travel this list to find victim zone.
	 * There is no need to link victim zone to specific list, because this
	 * system are for small capacity namespace.*/
	struct list_head partial_zone_list; /*not in use.*/
	struct list_head full_zone_list;
	/* For write context, active zone may be full, partial and reference by
	 * some context, so active zone would more than 1. */
	struct list_head active_zone_list; /*not in use.*/
	struct lbz_zone *active_zone[LBZ_ZONE_MAX_STREAM]; /*2 stream support f2fs.*/
	atomic64_t active_zone_writes[LBZ_ZONE_MAX_STREAM]; /*2 stream support f2fs.*/

	int empty_zone_count;
	int partial_zone_count; /*not in use.*/
	int full_zone_count;
	int active_zone_count; /*gc zone will not in any list, but it is active.*/

	/* Protect list and variable. */
	spinlock_t zmd_lock;

	/*zone state management thread.*/
	char zone_state_name[LBZ_MAX_NAME_LEN];
	struct workqueue_struct *zone_state_wq;
	struct work_struct zone_state_wk;
	struct timer_list zone_state_timer;
	unsigned int zone_state_expire;
	unsigned long last_jiffies;
	unsigned long zs_close_times;
	unsigned long zs_reset_times;

	/*Global resource, referenced by gc and alloc context.*/
	atomic_t nr_total_blks;
	atomic_t nr_allocable_blks;
	atomic_t nr_valid_blks; /*not accurate and don't need.*/

	/*watermark*/
	unsigned int reserved_blks_gc;
	unsigned int reclaim_wm_gc_low;
	unsigned int reclaim_wm_gc_high;
	unsigned int zone_reclaim_wm;

	/*zone size.*/
	unsigned int zone_size_sectors;
	unsigned int zone_size_blks;
	unsigned int nr_zones;
	unsigned int nr_useable_zones;

	/*zone capacity.*/
	unsigned int zone_nr_blocks;
	unsigned int zone_nr_blocks_shift;

	unsigned int zone_nr_sectors;
	unsigned int zone_nr_sectors_shift;

	unsigned int zone_bitmap_size;
	unsigned int zone_nr_bitmap_blocks;
	unsigned int zone_bits_per_block;

	unsigned int valid_bitmap_entries; /*not in use.*/
	unsigned int zone_reverse_map_size;
	unsigned int zone_nr_reverse_map_blocks;

	void *host; /*struct lbz_device*/
};

enum lbz_alloc_flag {
	LBZ_ALLOC_FLAG_USER,
	LBZ_ALLOC_FLAG_GC
};

#define LBZ_REVERSE_MAP_FOR_EACH_ENTRY(pos, zmd, zone, iter, iter1, iter2) \
	for (iter1 = iter / LBZ_REVERSE_MAP_ENTRIES, \
			iter2 = iter % LBZ_REVERSE_MAP_ENTRIES, \
			pos = zone->zrms[iter1]->blks[iter2]; \
			iter < zone->wp_block; \
			iter++, \
			iter1 = iter / LBZ_REVERSE_MAP_ENTRIES, \
			iter2 = iter % LBZ_REVERSE_MAP_ENTRIES, \
			pos = zone->zrms[iter1]->blks[iter2] \
			)

static inline bool is_lbz_zone_clear(struct lbz_zone *zone)
{
	return 0 == atomic_read(&zone->weight);
}

static inline bool is_lbz_zone_state(enum lbz_zone_state state, struct lbz_zone *zone)
{
	return test_bit(state, &zone->flags);
}

static inline void lbz_set_zone_state(enum lbz_zone_state state, struct lbz_zone *zone)
{
	set_bit(state, &zone->flags);
}

static inline void lbz_clear_zone_state(enum lbz_zone_state state, struct lbz_zone *zone)
{
	clear_bit(state, &zone->flags);
}

static inline bool lbz_check_zone_low_wm(struct lbz_zone_metadata *zmd, struct lbz_zone *zone)
{
	unsigned int wm = zmd->zone_nr_blocks * zmd->zone_reclaim_wm / 100;

	if (atomic_read(&zone->weight) <= wm)
		return true;
	return false;
}

static inline bool lbz_check_need_reclaim_low(struct lbz_zone_metadata *zmd)
{
	unsigned int wm = atomic_read(&zmd->nr_total_blks) * zmd->reclaim_wm_gc_low / 100;

	if (atomic_read(&zmd->nr_allocable_blks) - zmd->reserved_blks_gc < wm)
		return true;
	return false;
}

static inline bool lbz_check_need_reclaim_high(struct lbz_zone_metadata *zmd)
{
	unsigned int wm = atomic_read(&zmd->nr_total_blks) * zmd->reclaim_wm_gc_high / 100;

	if (atomic_read(&zmd->nr_allocable_blks) - zmd->reserved_blks_gc < wm)
		return true;
	return false;
}

void lbz_get_zone(struct lbz_zone *zone);
void lbz_put_zone(struct lbz_zone *zone);
void lbz_zone_complete_write(struct lbz_zone *zone);
void lbz_zone_release_global_res(struct lbz_zone_metadata *zmd, struct lbz_zone *zone);
struct lbz_zone *get_zone_by_pbid(struct lbz_zone_metadata *zmd, unsigned int pbid);
void lbz_zone_update_reverse_map(struct lbz_zone_metadata *zmd, struct lbz_zone *zone,
		unsigned int pbid, unsigned int blkid);
struct lbz_zone *lbz_find_victim_zone(struct lbz_zone_metadata *zmd, enum lbz_victim_mod mod);
int lbz_zone_alloc_res(struct lbz_zone_metadata *zmd, struct lbz_zone **ret_zone, enum lbz_alloc_flag mod, int stream_id);
/*not in use.*/
void lbz_zone_del_list(struct lbz_zone_metadata *zmd, struct lbz_zone *zone, enum lbz_zone_state state);

void lbz_zone_proc_read(struct lbz_zone_metadata *zmd, struct seq_file *seq);
void lbz_destroy_zone_metadata(struct lbz_zone_metadata *zmd);
int lbz_init_zone_metadata(struct lbz_zone_metadata *zmd, struct lbz_device *dev);
#endif
