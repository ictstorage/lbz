#ifndef _LBZ_ZONED_H_
#define _LBZ_ZONED_H_

/*
 * Zone descriptor.
 */
struct lbz_zone {
	/* For listing the zone depending on its state */
	struct list_head	link;

	/* Device containing this zone */
	struct dmz_dev		*dev;

	/* Zone type and state */
	unsigned long		flags;

	/* Zone activation reference count */
	atomic_t		refcount;

	/* Zone id */
	unsigned int		id;

	/* Zone write pointer block (relative to the zone start block) */
	unsigned int		wp_block;

	/* Zone weight (number of valid blocks in the zone) */
	unsigned int		weight;

	/* The chunk that the zone maps */
	unsigned int		chunk;
};

/*
 * In-memory metadata.
 */
struct lbz_metadata {
	struct dmz_dev		*dev;
	unsigned int		nr_devs;

	char			devname[BDEVNAME_SIZE];
	char			label[BDEVNAME_SIZE];
	uuid_t			uuid;

	unsigned int		zone_bits_per_mblk;

	sector_t		zone_nr_blocks;
	sector_t		zone_nr_blocks_shift;

	sector_t		zone_nr_sectors;
	sector_t		zone_nr_sectors_shift;

	unsigned int		nr_bitmap_blocks;
	unsigned int		nr_map_blocks;

	unsigned int		nr_zones;
	unsigned int		nr_useable_zones;
	unsigned int		nr_meta_blocks;
	unsigned int		nr_meta_zones;
	unsigned int		nr_data_zones;
	unsigned int		nr_cache_zones;
	unsigned int		nr_rnd_zones;
	unsigned int		nr_reserved_seq;
	unsigned int		nr_chunks;

	/* Zone information array */
	struct xarray		zones;

	struct dmz_sb		sb[2];
	unsigned int		mblk_primary;
	unsigned int		sb_version;
	u64			sb_gen;
	unsigned int		min_nr_mblks;
	unsigned int		max_nr_mblks;
	atomic_t		nr_mblks;
	struct rw_semaphore	mblk_sem;
	struct mutex		mblk_flush_lock;
	spinlock_t		mblk_lock;
	struct rb_root		mblk_rbtree;
	struct list_head	mblk_lru_list;
	struct list_head	mblk_dirty_list;
	struct shrinker		mblk_shrinker;

	/* Zone allocation management */
	struct mutex		map_lock;
	struct dmz_mblock	**map_mblk;

	unsigned int		nr_cache;
	atomic_t		unmap_nr_cache;
	struct list_head	unmap_cache_list;
	struct list_head	map_cache_list;

	atomic_t		nr_reserved_seq_zones;
	struct list_head	reserved_seq_zones_list;

	wait_queue_head_t	free_wq;
};

#endif
