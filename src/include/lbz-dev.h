#ifndef _LBZ_DEV_H_
#define _LBZ_DEV_H_
#include "lbz-common.h"

enum lbz_dev_state {
	LBZ_DEV_STATE_READY = 0,
	LBZ_DEV_STATE_REMOVE,
	LBZ_DEV_STATE_FAULTY
};

struct lbz_zone_metadata;
struct lbz_mapping;
struct lbz_io_scheduler;
struct lbz_gc_context;
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
struct lbz_nat_sit_mgmt;
#endif
struct lbz_device {
	struct list_head list;
	struct gendisk *disk;
	char devname[BDEVNAME_SIZE];
	int minor;
	struct block_device *phy_bdev;
	unsigned int meta_bytes;
	sector_t dev_size; /*in sectors*/

	struct bio_set bio_split;

	struct proc_dir_entry *proc_entry;

	atomic_t refcnt;
	atomic64_t transaction_id; /*for transaction support.*/
	atomic64_t timestamp; /*for any write.*/

	unsigned int nr_zones;
	unsigned int zone_offset; /*not in use.*/

	unsigned long flags;

	sector_t zone_nr_sectors; /*not in use.*/
	struct lbz_zone_metadata *zone_metadata;

	struct lbz_mapping *mapping;

	struct lbz_io_scheduler *iosched;

	struct lbz_gc_context *gc_ctx;

#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	struct lbz_nat_sit_mgmt *nat_sit_mgmt;
#endif

	/*statistics for IO and GC.*/
	/*not include io, which encounter error before submit_bio.*/
	atomic64_t user_write_inflight_io_cnt;
	/*not include zero read io.*/
	atomic64_t user_read_inflight_io_cnt;
	atomic64_t user_read_err_cnt;
	atomic64_t user_read_zero_cnt;
	atomic64_t user_write_err_cnt;

	atomic64_t user_encounter_emergency;
	atomic64_t write_alloc_encounter_eagain;
	atomic64_t gc_alloc_encounter_eagain;

	atomic64_t user_write_blocks;
	atomic64_t user_read_blocks;
	
	atomic64_t gc_inflight_io_cnt;
	atomic64_t gc_write_err_cnt;
	atomic64_t gc_read_err_cnt;

	atomic64_t gc_read_blocks;
	atomic64_t gc_write_blocks;
	atomic64_t gc_write_agency_blocks;
	atomic64_t gc_write_complete_blocks;
	atomic64_t gc_write_discarded_blocks;
	atomic64_t gc_reset_blocks; /*gc zones * zone_nr_blocks.*/
};

static inline void lbz_dev_set_unready(struct lbz_device *dev)
{
	clear_bit(LBZ_DEV_STATE_READY, &dev->flags);
}

static inline void lbz_dev_set_ready(struct lbz_device *dev)
{
	set_bit(LBZ_DEV_STATE_READY, &dev->flags);
}

static inline int is_dev_ready(struct lbz_device *dev)
{
	return test_bit(LBZ_DEV_STATE_READY, &dev->flags);
}

static inline int is_dev_faulty(struct lbz_device *dev)
{
	return test_bit(LBZ_DEV_STATE_FAULTY, &dev->flags);
}

static inline void lbz_dev_set_remove(struct lbz_device *dev)
{
	set_bit(LBZ_DEV_STATE_REMOVE, &dev->flags);
}

static inline int is_dev_remove(struct lbz_device *dev)
{
	return test_bit(LBZ_DEV_STATE_REMOVE, &dev->flags);
}

static inline void lbz_dev_set_faulty(struct lbz_device *dev)
{
	set_bit(LBZ_DEV_STATE_FAULTY, &dev->flags);
}

static inline long lbz_dev_get_tx_id(struct lbz_device *dev)
{
	return atomic64_inc_return(&dev->transaction_id);
}

static inline long lbz_dev_read_tx_id(struct lbz_device *dev)
{
	return atomic64_read(&dev->transaction_id);
}

static inline long lbz_dev_get_timestamp(struct lbz_device *dev)
{
	return atomic64_inc_return(&dev->timestamp);
}

static inline void lbz_inc_gc_write_err(struct lbz_device *dev)
{
	atomic64_inc(&dev->gc_write_err_cnt);
}

static inline void lbz_inc_gc_read_err(struct lbz_device *dev)
{
	atomic64_inc(&dev->gc_read_err_cnt);
}

static inline void lbz_inc_gc_inflight(struct lbz_device *dev)
{
	atomic64_inc(&dev->gc_inflight_io_cnt);
}

static inline void lbz_dec_gc_inflight(struct lbz_device *dev)
{
	atomic64_dec(&dev->gc_inflight_io_cnt);
}

int lbz_create_device(unsigned int block_size,
		sector_t sectors, struct block_device *phy_bdev);
struct lbz_device *lbz_dev_find_by_minor(int minor);
int lbz_dev_init(void);
void lbz_dev_exit(void);
#endif
