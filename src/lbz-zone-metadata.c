#include "lbz-zone-metadata.h"
#include "lbz-dev.h"
#include "lbz-gc.h"

#define LBZ_MSG_PREFIX "lbz-zmd"

static void lbz_zone_add_to_list(struct lbz_zone_metadata *zmd, struct lbz_zone *zone, enum lbz_zone_state state);

/*
 * Various accessors
 */
sector_t lbz_start_sect(struct lbz_zone_metadata *zmd, struct lbz_zone *zone)
{
	unsigned int zone_id = zone->id;

	return (sector_t)zone_id << zmd->zone_nr_sectors_shift;
}

sector_t lbz_start_block(struct lbz_zone_metadata *zmd, struct lbz_zone *zone)
{
	unsigned int zone_id = zone->id;

	return (sector_t)zone_id << zmd->zone_nr_blocks_shift;
}

unsigned int lbz_zone_nr_blocks(struct lbz_zone_metadata *zmd)
{
	return zmd->zone_nr_blocks;
}

unsigned int lbz_zone_nr_blocks_shift(struct lbz_zone_metadata *zmd)
{
	return zmd->zone_nr_blocks_shift;
}

unsigned int lbz_zone_nr_sectors(struct lbz_zone_metadata *zmd)
{
	return zmd->zone_nr_sectors;
}

unsigned int lbz_zone_nr_sectors_shift(struct lbz_zone_metadata *zmd)
{
	return zmd->zone_nr_sectors_shift;
}

unsigned int lbz_nr_zones(struct lbz_zone_metadata *zmd)
{
	return zmd->nr_zones;
}

int lbz_close_zone(struct lbz_zone *zone, struct lbz_zone_metadata *zmd)
{
	int ret = 0;
	struct lbz_device *dev = zmd->host;

	/*ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_CLOSE, zone->start_sector, zone->len_sectors, GFP_NOIO);*/
	ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_CLOSE, zone->start_sector, zone->capacity_sectors, GFP_NOIO);

	return ret;
}

int lbz_open_zone(struct lbz_zone *zone, struct lbz_zone_metadata *zmd)
{
	int ret = 0;
	struct lbz_device *dev = zmd->host;

	/*ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_OPEN, zone->start_sector, zone->len_sectors, GFP_NOIO);*/
	ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_OPEN, zone->start_sector, zone->len_sectors, GFP_NOIO);

	return ret;
}

int lbz_reset_zone(struct lbz_zone *zone, struct lbz_zone_metadata *zmd)
{
	int ret = 0;
	struct lbz_device *dev = zmd->host;

	/*ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_RESET, zone->start_sector, zone->len_sectors, GFP_NOIO);*/
	ret = blkdev_zone_mgmt(dev->phy_bdev, REQ_OP_ZONE_RESET, zone->start_sector, zone->len_sectors, GFP_NOIO);

	return ret;
}

static struct lbz_zone *__get_zone(struct lbz_zone_metadata *zmd, unsigned int zone_id)
{
	return zmd->zones[zone_id];
}

void lbz_get_zone(struct lbz_zone *zone)
{
	atomic_inc(&zone->refcount);
}

/* write zone: may referenced by read, if zone write full but still referenced by read, it can be moved to full list.
 * GC zone: trigger reset after all gc write completed and zone not referenced by any context.*/
void lbz_put_zone(struct lbz_zone *zone)
{
	unsigned long flag = 0;

	spin_lock_irqsave(&zone->lock, flag);
	if (atomic_dec_and_test(&zone->refcount)) {
		/* After gc, zone won't be referenced.*/
		/*zone state may be closed before.*/
		if (is_lbz_zone_state(LBZ_ZONE_GC, zone)) {
			zone->state = BLK_ZONE_COND_EMPTY;
			spin_unlock_irqrestore(&zone->lock, flag);
			return;
		}
	}
	/*wp has moved to invalid addr, but write io may not have completed.*/
	if (is_lbz_zone_state(LBZ_ZONE_TO_FULL, zone) && 0 == atomic_read(&zone->pending_write_io)) {
		zone->state = BLK_ZONE_COND_FULL;
		spin_unlock_irqrestore(&zone->lock, flag);
		return;
	}
	spin_unlock_irqrestore(&zone->lock, flag);
}

static void __pending_write(struct lbz_zone *zone)
{
	atomic_inc(&zone->pending_write_io);
}

void lbz_zone_complete_write(struct lbz_zone *zone)
{
	atomic_dec(&zone->pending_write_io);
}

void lbz_zone_release_global_res(struct lbz_zone_metadata *zmd, struct lbz_zone *zone)
{
	atomic_dec(&zone->weight);
	atomic_dec(&zmd->nr_valid_blks);
}

struct lbz_zone *get_zone_by_pbid(struct lbz_zone_metadata *zmd, unsigned int pbid)
{
	int index;
	struct lbz_device *dev = zmd->host;
	
	/*
	 * index = pbid / zmd->zone_size_blks;
	 */
	index = blk_queue_zone_no(bdev_get_queue(dev->phy_bdev), blkid_to_sector(pbid));
	return __get_zone(zmd, index);
}

static void lbz_zone_destroy(struct lbz_zone_metadata *zmd, struct lbz_zone *zone)
{
	int i = 0;

	for (; i < zmd->zone_nr_bitmap_blocks; i++) {
		if (!zone->zone_valid_bitmap[i])
			break;
		free_page((unsigned long)zone->zone_valid_bitmap[i]);
	}
	if (zone->zone_valid_bitmap)
		LBZ_FREE_MEM(zone->zone_valid_bitmap, zmd->zone_nr_bitmap_blocks * sizeof(unsigned long *));

	for (i = 0; i < zmd->zone_nr_reverse_map_blocks; i++) {
		if (!zone->zrms[i])
			break;
		free_page((unsigned long)zone->zrms[i]);
	}
	if (zone->zrms)
		LBZ_FREE_MEM(zone->zrms, zmd->zone_nr_reverse_map_blocks * sizeof(struct zone_reverse_mapping *));
	if (zone)
		LBZ_FREE_MEM(zone, sizeof(struct lbz_zone));
}

static struct lbz_zone *lbz_zone_insert(struct lbz_zone_metadata *zmd,
				  unsigned int zone_id, struct lbz_device *dev)
{
	int i = 0;
	struct lbz_zone *zone;

	LBZ_ALLOC_MEM(zone, sizeof(struct lbz_zone), GFP_KERNEL);

	if (!zone)
		return ERR_PTR(-ENOMEM);

	LBZ_ALLOC_MEM(zone->zone_valid_bitmap, zmd->zone_nr_bitmap_blocks * sizeof(unsigned long *), GFP_NOIO);
	for (; i < zmd->zone_nr_bitmap_blocks; i++) {
		zone->zone_valid_bitmap[i] = (unsigned long *)__get_free_page(GFP_NOIO);
		/* 0 for not mapped, 1 for mapped. */
		memset(zone->zone_valid_bitmap[i], 0x00, PAGE_SIZE);
	}

	LBZ_ALLOC_MEM(zone->zrms, zmd->zone_nr_reverse_map_blocks * sizeof(struct zone_reverse_mapping *), GFP_NOIO);
	for (i = 0; i < zmd->zone_nr_reverse_map_blocks; i++) {
		zone->zrms[i] = (struct zone_reverse_mapping *)__get_free_page(GFP_NOIO);
		/* LBZ_INVALID_PBID for not mapped. */
		memset(zone->zrms[i], 0xff, PAGE_SIZE);
	}

	INIT_LIST_HEAD(&zone->link);
	spin_lock_init(&zone->lock);
	zone->flags = 0;
	zone->state = 0;
	atomic_set(&zone->refcount, 0);
	zone->id = zone_id;

	return zone;
}

void __init_zmd_by_first_zone(struct lbz_zone_metadata *zmd, struct blk_zone *blkz)
{
	/* Init */
	zmd->zone_nr_sectors = blkz->capacity;
	zmd->zone_nr_sectors_shift = ilog2(zmd->zone_nr_sectors);
	zmd->zone_nr_blocks = sector_to_blkid(zmd->zone_nr_sectors);
	zmd->zone_nr_blocks_shift = ilog2(zmd->zone_nr_blocks);
	zmd->zone_bitmap_size = (zmd->zone_nr_blocks + 7) >> 3;
	zmd->zone_nr_bitmap_blocks = (zmd->zone_bitmap_size + LBZ_BITMAP_BLK_SIZE - 1) >> LBZ_BITMAP_BLK_SHIFT;
	zmd->zone_bits_per_block = LBZ_BITMAP_BITS_PER_BLK;
	zmd->zone_reverse_map_size = zmd->zone_nr_blocks * sizeof(unsigned int);
	zmd->zone_nr_reverse_map_blocks = (zmd->zone_reverse_map_size + LBZ_REVERSE_MAP_BLK_SIZE - 1) >> LBZ_REVERSE_MAP_BLK_SHIFT;
}

/*
 * Initialize a zone descriptor.
 */
static int lbz_init_zone(struct blk_zone *blkz, unsigned int num, void *data)
{
	struct lbz_zone_metadata *zmd = data;
	struct lbz_device *dev = zmd->host;
	int idx = num;
	int i = 0;
	struct lbz_zone *zone = NULL;

	/*Assume that all the zone are the same as first zone in capacity.*/
	if (num == 0)
		__init_zmd_by_first_zone(zmd, blkz);

	zone = lbz_zone_insert(zmd, idx, dev);
	if (IS_ERR(zone))
		return PTR_ERR(zone);

	/*
	 * Devices that have zones with a capacity smaller than the zone size
	 * (NVMe zoned namespaces) are supported.
	 *
	 * if (blkz->capacity != blkz->len)
	 * 	return -ENXIO;
	 */

	zone->wp_block = sector_to_blkid(blkz->wp - blkz->start);
	atomic_set(&zone->weight, zone->wp_block);
	zone->start_sector = (unsigned int)blkz->start;
	zone->len_sectors = (unsigned int)blkz->len;
	zone->capacity_sectors = (unsigned int)blkz->capacity;
	zone->cond = blkz->cond;
	atomic_set(&zone->pending_write_io, 0);

	zone->stream = -1;

	/*set zone state.*/
	if (zone->wp_block == 0) {
		zone->state = BLK_ZONE_COND_EMPTY;
		lbz_set_zone_state(LBZ_ZONE_INIT, zone);
		atomic_add(zmd->zone_nr_blocks, &zmd->nr_allocable_blks);
		lbz_zone_add_to_list(zmd, zone, LBZ_ZONE_INIT);
	} else if (zone->wp_block < sector_to_blkid(blkz->capacity)) {
		/*using implict open.*/
		lbz_get_zone(zone);
		zone->state = BLK_ZONE_COND_IMP_OPEN;
		lbz_set_zone_state(LBZ_ZONE_ACTIVE, zone);
		/*only permit one zone per stream to be opened and not full.*/
		for (; i < LBZ_ZONE_MAX_STREAM; i++) {
			if (NULL == zmd->active_zone[i]) {
				zone->stream = i;
				zmd->active_zone[i] = zone;
				break;
			}
		}
		BUG_ON(i == LBZ_ZONE_MAX_STREAM);
		atomic_add(zmd->zone_nr_blocks - zone->wp_block, &zmd->nr_allocable_blks);
	} else {
		zone->state = BLK_ZONE_COND_FULL;
		lbz_set_zone_state(LBZ_ZONE_FULL, zone);
		lbz_zone_add_to_list(zmd, zone, LBZ_ZONE_FULL);
	}
	/*assume all written data are valid at this time.*/
	atomic_add(zone->wp_block, &zmd->nr_valid_blks);

	if (blkz->cond == BLK_ZONE_COND_OFFLINE) {
		set_bit(LBZ_ZONE_OFFLINE, &zone->flags);
		zone->state = BLK_ZONE_COND_OFFLINE;
	} else if (blkz->cond == BLK_ZONE_COND_READONLY) {
		set_bit(LBZ_ZONE_READ_ONLY, &zone->flags);
		zone->state = BLK_ZONE_COND_READONLY;
	} else {
		zmd->nr_useable_zones++;
	}
	atomic_add(zmd->zone_nr_blocks, &zmd->nr_total_blks);
	zmd->zones[num] = zone;
	lbz_close_zone(zone, zmd);

	return 0;
}

static void lbz_drop_zones(struct lbz_zone_metadata *zmd)
{
	int i = 0;

	for (; i < zmd->nr_zones; i++)
		lbz_zone_destroy(zmd, zmd->zones[i]);
	LBZ_FREE_MEM(zmd->zones, zmd->nr_zones * sizeof(struct lbz_zone *));
}

/*
 * Allocate and initialize zone descriptors using the zone
 * information from disk.
 */
static int lbz_init_zones(struct lbz_zone_metadata *zmd, struct lbz_device *dev)
{
	int ret = 0;

	if (!zmd->nr_zones) {
		LBZERR("(%s): No zones found", dev->devname);
		return -ENXIO;
	}
	LBZ_ALLOC_MEM(zmd->zones, zmd->nr_zones * sizeof(struct lbz_zone *), GFP_NOIO);

	LBZINFO("(%s)Using %lu B for zone information",
		dev->devname, sizeof(struct lbz_zone) * zmd->nr_zones);

	/*
	 * Get zone information and initialize zone descriptors.  At the same
	 * time, determine where the super block should be: first block of the
	 * first randomly writable zone.
	 */
	ret = blkdev_report_zones(dev->phy_bdev, 0, BLK_ALL_ZONES,
				  lbz_init_zone, zmd);
	if (ret < 0) {
		LBZDEBUG("(%s): Failed to report zones, error %d",
			dev->devname, ret);
		lbz_drop_zones(zmd);
		return ret;
	}

	return 0;
}

void lbz_zone_update_reverse_map(struct lbz_zone_metadata *zmd, struct lbz_zone *zone,
		unsigned int pbid, unsigned int blkid)
{
	unsigned int dis = pbid - sector_to_blkid(zone->start_sector);
	unsigned int index = dis / LBZ_REVERSE_MAP_ENTRIES;
	unsigned int offset = dis % LBZ_REVERSE_MAP_ENTRIES;

	LBZDEBUG("(%lx) pbid: %u, blkid: %u", (unsigned long)zone, pbid, blkid);
	zone->zrms[index]->blks[offset] = blkid;
}

/* find zone to GC. */
struct lbz_zone *lbz_find_victim_zone(struct lbz_zone_metadata *zmd, enum lbz_victim_mod mod)
{
	struct lbz_zone *min_zone = NULL, *pos;
	int min_valid = ((int)(~0U >> 1));
	unsigned long flag = 0;

	spin_lock_irqsave(&zmd->zmd_lock, flag);
	if (list_empty(&zmd->full_zone_list)) {
		goto out;
	}
	list_for_each_entry(pos, &zmd->full_zone_list, link) {
		if (atomic_read(&pos->weight) < min_valid) {
			min_valid = atomic_read(&pos->weight);
			min_zone = pos;
		}
	}
	/*no block to reclaim and no block in full_zone_list.*/
	if (min_valid >= zmd->zone_nr_blocks) {
		LBZINFO_LIMIT("min_zone(%lx) valid blocks: %d", (unsigned long)min_zone, min_valid);
		min_zone = NULL;
		goto out;
	}
	if (mod == LBZ_VICTIM_REGULAR && !lbz_check_zone_low_wm(zmd, min_zone)) {
		min_zone = NULL;
		goto out;
	}
	list_del_init(&min_zone->link);
	lbz_get_zone(min_zone); /* must get before set state, because LBZ_ZONE_GC may induce reset of zone
							 * read: lbz_get_zone->read->lbz_put_zone->change state to BLK_ZONE_COND_EMPTY->reset
							 * by reset work.*/
	lbz_set_zone_state(LBZ_ZONE_GC, min_zone);
	zmd->full_zone_count--;
	zmd->active_zone_count++;
out:
	spin_unlock_irqrestore(&zmd->zmd_lock, flag);

	return min_zone;
}

void lbz_zone_del_list(struct lbz_zone_metadata *zmd, struct lbz_zone *zone, enum lbz_zone_state state)
{
	unsigned long flag;

	spin_lock_irqsave(&zmd->zmd_lock, flag);
    clear_bit(state, &zone->flags);
	list_del_init(&zone->link);
	spin_unlock_irqrestore(&zmd->zmd_lock, flag);
}

void lbz_zone_add_to_list(struct lbz_zone_metadata *zmd, struct lbz_zone *zone, enum lbz_zone_state state)
{
	unsigned long flag;

	spin_lock_irqsave(&zmd->zmd_lock, flag);
	switch (state) {
		case LBZ_ZONE_FULL:
			list_add(&zone->link, &zmd->full_zone_list);
			zmd->full_zone_count++;
			break;
		case LBZ_ZONE_ACTIVE:
		case LBZ_ZONE_GC:
			list_add(&zone->link, &zmd->active_zone_list);
			break;
		case LBZ_ZONE_EMPTY:
		case LBZ_ZONE_INIT:
			list_add_tail(&zone->link, &zmd->empty_zone_list);
			zmd->empty_zone_count++;
			break;
		default:
			BUG();
	}
	spin_unlock_irqrestore(&zmd->zmd_lock, flag);
}

/*must be locked by caller.*/
struct lbz_zone * __get_free_zone(struct lbz_zone_metadata *zmd)
{
	struct lbz_zone *zone = NULL;

	if (!list_empty(&zmd->empty_zone_list)) {
		zone = list_entry(zmd->empty_zone_list.next, struct lbz_zone, link);
		list_del_init(&zone->link);
		zmd->empty_zone_count--;
	}

	return zone;
}

void __alloc_res(struct lbz_zone *zone)
{
	zone->wp_block++;
	atomic_inc(&zone->weight);
}

/*
 * user write alloc res will be denied when there are no space for gc.
 */
int lbz_zone_alloc_res(struct lbz_zone_metadata *zmd, struct lbz_zone **ret_zone, enum lbz_alloc_flag mod, int stream_id)
{
	unsigned long flag;
	struct lbz_device *dev = zmd->host;
	int ret = 0;
	bool open_zone = false;

	spin_lock_irqsave(&zmd->zmd_lock, flag);
	if (is_dev_faulty(dev)) {
		ret = -EIO;
		LBZERR("(%s)lbz dev faulty", dev->devname);
		goto out;
	}
	if (mod == LBZ_ALLOC_FLAG_USER) {
		if (atomic_read(&zmd->nr_allocable_blks) < zmd->reserved_blks_gc) {
			ret = -EAGAIN;
			LBZDEBUG("(%s)have not enough blks for user write.", dev->devname);
			goto out;
		}
	}
	if (unlikely((NULL == zmd->active_zone[stream_id]) || (zmd->active_zone[stream_id]->wp_block == zmd->zone_nr_blocks))) {
		/*wp_block will not be modified after this put*/
		if (NULL != zmd->active_zone[stream_id])
			lbz_put_zone(zmd->active_zone[stream_id]);
		zmd->active_zone[stream_id] = __get_free_zone(zmd);
		if (NULL == zmd->active_zone[stream_id]) {
			LBZDEBUG("alloc zone encounter error.");
			ret = -EAGAIN;
			/*if one stream can not alloc res, it may retry alloc on another stream by caller.*/
#ifndef CONFIG_LBZ_NAT_SIT_STREAM_SUPPORT
			lbz_trigger_gc_reclaim(dev->gc_ctx);
			BUG_ON(mod == LBZ_ALLOC_FLAG_GC);
#endif
			goto out;
		}
		zmd->active_zone[stream_id]->stream = stream_id;
		/*zone state will not handle this during write.*/
		lbz_get_zone(zmd->active_zone[stream_id]);
		lbz_set_zone_state(LBZ_ZONE_ACTIVE, zmd->active_zone[stream_id]);
		zmd->active_zone[stream_id]->state = BLK_ZONE_COND_EXP_OPEN;
		open_zone = true;
	}
	/* get for io write and gc write.
	 * in case that another context close this zone.*/
	lbz_get_zone(zmd->active_zone[stream_id]);
	__alloc_res(zmd->active_zone[stream_id]);
	atomic64_inc(&zmd->active_zone_writes[stream_id]); /*statistic stream writes.*/
	atomic_dec(&zmd->nr_allocable_blks);
	atomic_inc(&zmd->nr_valid_blks);
	*ret_zone = zmd->active_zone[stream_id];
	__pending_write(zmd->active_zone[stream_id]);
	if (zmd->active_zone[stream_id]->wp_block == zmd->zone_nr_blocks) {
		lbz_clear_zone_state(LBZ_ZONE_ACTIVE, zmd->active_zone[stream_id]);
		lbz_set_zone_state(LBZ_ZONE_TO_FULL, zmd->active_zone[stream_id]);
		lbz_put_zone(zmd->active_zone[stream_id]);
		zmd->active_zone[stream_id] = NULL;
	}
out:
	spin_unlock_irqrestore(&zmd->zmd_lock, flag);
	/*may open by another context, but not important.*/
	if (open_zone) {
		/*ret = lbz_open_zone(*ret_zone, zmd);*/
		/*check code for reopen.*/
		if (ret < 0) {
			LBZERR("zone: %lx, open encounter error: %d", (unsigned long)*ret_zone, ret);
			lbz_dev_set_faulty(dev);
			/*for active and write.*/
			lbz_put_zone(*ret_zone);
			lbz_put_zone(*ret_zone);
		}
	}
	return ret;
}

void lbz_zone_proc_read(struct lbz_zone_metadata *zmd, struct seq_file *seq)
{
	int i = 0;
	struct lbz_zone *zone;

	seq_printf(seq, "empty_zone_count: %u\n"
					"full_zone_count: %u\n"
					"nr_total_blks: %d\n"
					"nr_allocable_blks: %d\n"
					"nr_valid_blks: %d\n"
					"reserved_blks_gc: %u\n"
					"reclaim_wm_gc_low: %u\n"
					"reclaim_wm_gc_high: %u\n"
					"zone_size_sectors: %u\n"
					"zone_size_blks: %u\n"
					"nr_zones: %u\n"
					"nr_useable_zones: %u\n"
					"zone_nr_blocks: %u\n"
					"zone_nr_sectors: %u\n"
					"zone_bitmap_size: %u\n"
					"zone_nr_bitmap_blocks: %u\n"
					"zone_reverse_map_size: %u\n"
					"zone_nr_reverse_map_blocks: %u\n"
					"zs_close_times: %lu\n"
					"zs_reset_times: %lu\n",
					zmd->empty_zone_count,
					zmd->full_zone_count,
					atomic_read(&zmd->nr_total_blks),
					atomic_read(&zmd->nr_allocable_blks),
					atomic_read(&zmd->nr_valid_blks),
					zmd->reserved_blks_gc,
					zmd->reclaim_wm_gc_low,
					zmd->reclaim_wm_gc_high,
					zmd->zone_size_sectors,
					zmd->zone_size_blks,
					zmd->nr_zones,
					zmd->nr_useable_zones,
					zmd->zone_nr_blocks,
					zmd->zone_nr_sectors,
					zmd->zone_bitmap_size,
					zmd->zone_nr_bitmap_blocks,
					zmd->zone_reverse_map_size,
					zmd->zone_nr_reverse_map_blocks,
					zmd->zs_close_times,
					zmd->zs_reset_times);
	for (i = 0; i < LBZ_ZONE_MAX_STREAM; i++)
		seq_printf(seq, "active_zone_writes[%d]: %llu\n", i, atomic64_read(&zmd->active_zone_writes[i]));

	seq_printf(seq, "-------zone info-------\n");
	for (i = 0; i < zmd->nr_zones; i++) {
		zone = zmd->zones[i];
		seq_printf(seq, "zone(%d): state(%d), flags(%lu), refcount(%d), wp_block(%u),"
						"start_sector(%u), len_sectors(%u), capacity_sectors(%u), weight(%u),"
						"pending_write_io(%d), valid_percent(%d%%), stream(%d)\n",
						zone->id, zone->state, zone->flags, atomic_read(&zone->refcount),
						zone->wp_block, zone->start_sector, zone->len_sectors, zone->capacity_sectors,
						atomic_read(&zone->weight), atomic_read(&zone->pending_write_io),
						atomic_read(&zone->weight) * 100 / zmd->zone_nr_blocks, zone->stream);
	}
}

void lbz_destroy_zone_metadata(struct lbz_zone_metadata *zmd)
{
	int i = 0;

	del_timer_sync(&zmd->zone_state_timer);
	destroy_workqueue(zmd->zone_state_wq);
	/*close all zone no matter whether it opened or not.*/
	for (; i < zmd->nr_zones; i++)
		lbz_close_zone(zmd->zones[i], zmd);
	lbz_drop_zones(zmd);
}

static void zone_state_wk_fn(struct work_struct *work)
{
	struct lbz_zone_metadata *zmd = container_of(work, struct lbz_zone_metadata, zone_state_wk);
	struct lbz_device *dev = zmd->host;
	struct lbz_zone *zone = NULL;
	unsigned long flag = 0;
	bool send_zone_mgmgt = false;
	int i = 0, ret = 0, k = 0;

	if (is_dev_faulty(dev) || !is_dev_ready(dev))
		return;

	for (i = 0; i < zmd->nr_zones; i++) {
		zone = zmd->zones[i];
		/*close zone.*/
		if (is_lbz_zone_state(LBZ_ZONE_TO_FULL, zone)) {
			spin_lock_irqsave(&zone->lock, flag);
			if (zone->state == BLK_ZONE_COND_FULL) {
				send_zone_mgmgt = true;
				zone->state = BLK_ZONE_COND_CLOSED;
			}
			spin_unlock_irqrestore(&zone->lock, flag);
			if (send_zone_mgmgt) {
				lbz_close_zone(zone, zmd);
				if (ret < 0) {
					lbz_dev_set_faulty(dev);
					goto out;
				}
				zone->flags = (1 << LBZ_ZONE_FULL);
				spin_lock_irqsave(&zmd->zmd_lock, flag);
				/*not in any list before.*/
				list_add_tail(&zone->link, &zmd->full_zone_list);
				zmd->full_zone_count++;
				spin_unlock_irqrestore(&zmd->zmd_lock, flag);
				zmd->zs_close_times++;
			}
			continue;
		}
		if (0 != atomic_read(&zone->refcount))
			continue;
		/*reset zone.*/
		if (is_lbz_zone_state(LBZ_ZONE_GC, zone)) {
			spin_lock_irqsave(&zone->lock, flag);
			if (zone->state == BLK_ZONE_COND_EMPTY) {
				send_zone_mgmgt = true;
			}
			spin_unlock_irqrestore(&zone->lock, flag);
			if (send_zone_mgmgt) {
				ret = lbz_reset_zone(zone, zmd);
				if (ret < 0) {
					lbz_dev_set_faulty(dev);
					LBZERR("zone(%lx) reset encounter error: %d", (unsigned long)zone, ret);
					goto out;
				}
				if (atomic_read(&zone->weight) != 0)
					panic("zone(%lx), reset encounter valid blocks!", (unsigned long)zone);

				zone->stream = -1;

				zone->wp_block = 0;
				zone->state = BLK_ZONE_COND_EMPTY;
				zone->flags = (1 << LBZ_ZONE_INIT);
				for (k = 0; k < zmd->zone_nr_reverse_map_blocks; k++)
					memset(zone->zrms[k], 0xff, PAGE_SIZE);
				spin_lock_irqsave(&zmd->zmd_lock, flag);
				/*not in any list before.*/
				list_add_tail(&zone->link, &zmd->empty_zone_list);
				zmd->empty_zone_count++;
				/*lbz_find_victim_zone added.*/
				zmd->active_zone_count--;
				atomic_add(zmd->zone_nr_blocks, &zmd->nr_allocable_blks);
				spin_unlock_irqrestore(&zmd->zmd_lock, flag);
				zmd->zs_reset_times++;
			}
			lbz_complete_one_zone(dev->gc_ctx);
			continue;
		}
	}
out:
	return;
}

static void __zs_timer_fn(struct timer_list *timer)
{
	struct lbz_zone_metadata *zmd = container_of(timer, struct lbz_zone_metadata, zone_state_timer);

	queue_work(zmd->zone_state_wq, &zmd->zone_state_wk);
	mod_timer(&zmd->zone_state_timer, jiffies + zmd->zone_state_expire);
	zmd->last_jiffies = jiffies;
}

int lbz_init_zone_metadata(struct lbz_zone_metadata *zmd, struct lbz_device *dev)
{
	int ret = 0, i = 0;

	INIT_LIST_HEAD(&zmd->empty_zone_list);
	INIT_LIST_HEAD(&zmd->partial_zone_list);
	INIT_LIST_HEAD(&zmd->full_zone_list);
	INIT_LIST_HEAD(&zmd->active_zone_list);
	for (; i < LBZ_ZONE_MAX_STREAM; i++) {
		zmd->active_zone[i] = NULL;
		atomic64_set(&zmd->active_zone_writes[i], 0);
	}
	zmd->empty_zone_count = 0;
	zmd->partial_zone_count = 0;
	zmd->full_zone_count = 0;
	zmd->active_zone_count = 0;
	spin_lock_init(&zmd->zmd_lock);
	zmd->nr_zones = blk_queue_nr_zones(bdev_get_queue(dev->phy_bdev));
	zmd->zone_size_sectors = blk_queue_zone_sectors(bdev_get_queue(dev->phy_bdev));
	zmd->zone_size_blks = sector_to_blkid(zmd->zone_size_sectors);

	atomic_set(&zmd->nr_total_blks, 0);
	atomic_set(&zmd->nr_allocable_blks, 0);
	atomic_set(&zmd->nr_valid_blks, 0);

	zmd->host = dev;

	ret = lbz_init_zones(zmd, dev);
	if (ret < 0) {
		LBZERR("(%s)init zone encounter error: %d", dev->devname, ret);
		return ret;
	}

	/*reserve one zone for gc, in case that gc cannot execute because of out of space.*/
	zmd->reserved_blks_gc = zmd->zone_nr_blocks;
	zmd->reclaim_wm_gc_low = LBZ_GC_RECLAIM_DEFAULT_WM_LOW;
	zmd->reclaim_wm_gc_high = LBZ_GC_RECLAIM_DEFAULT_WM_HIGH;
	zmd->zone_reclaim_wm = LBZ_ZONE_RECLAIM_DEFAULT_WM;

	zmd->zs_close_times = 0;
	zmd->zs_reset_times = 0;
	snprintf(zmd->zone_state_name, BDEVNAME_SIZE, "%s_zs", dev->devname);
	zmd->zone_state_wq = create_singlethread_workqueue(zmd->zone_state_name);
	if (IS_ERR(zmd->zone_state_wq)) {
		LBZERR("alloc workqueue [%s] error:%ld", zmd->zone_state_name, PTR_ERR(zmd->zone_state_wq));
		lbz_drop_zones(zmd);
		return PTR_ERR(zmd->zone_state_wq);
	}
	INIT_WORK(&zmd->zone_state_wk, zone_state_wk_fn);
	timer_setup(&zmd->zone_state_timer, __zs_timer_fn, 0);
	zmd->zone_state_expire = LBZ_ZONE_STATE_EXPIRE;
	zmd->zone_state_timer.expires = jiffies + zmd->zone_state_expire;
	add_timer(&zmd->zone_state_timer);

	return 0;
}
