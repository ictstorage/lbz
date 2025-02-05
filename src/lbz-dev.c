#include "lbz-dev.h"
#include "lbz-io-scheduler.h"
#include "lbz-zone-metadata.h"
#include "lbz-mapping.h"
#include "lbz-gc.h"
#include "lbz-request.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-dev"

atomic64_t lbz_mem_bytes = ATOMIC_INIT(0);
LIST_HEAD(lbz_sets);
static LIST_HEAD(lbz_devices);
struct mutex lbz_register_lock;

static int lbz_major = 0;
static DEFINE_IDA(lbz_device_idx);
#define LBZ_MINORS		128
/* limitation of lbz devices number on single system */
#define LBZ_DEVICE_IDX_MAX	((1U << MINORBITS) / LBZ_MINORS)
#define LBZ_DEV_ENTRY_FILENAME "lbz-dev"
struct proc_dir_entry *lbz_dev_proc_file = NULL;

static int open_dev(struct block_device *b, fmode_t mode)
{
	struct lbz_device *d = b->bd_disk->private_data;

	if (!is_dev_faulty(d) && !is_dev_remove(d) && is_dev_ready(d)) {
		atomic_inc(&d->refcnt);
		return 0;
	}
	return -EINVAL;
}

static void release_dev(struct gendisk *b, fmode_t mode)
{
	struct lbz_device *d = b->private_data;
	wait_queue_head_t wq;

	init_waitqueue_head(&wq);
	do {
		wait_event_timeout(wq,
				(atomic64_read(&d->user_write_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_read_inflight_io_cnt) == 0) ||
				is_dev_faulty(d), HZ);
	} while (!(atomic64_read(&d->user_write_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_read_inflight_io_cnt) == 0) &&
				!is_dev_faulty(d));

	atomic_dec(&d->refcnt);
}

static int ioctl_dev(struct block_device *b, fmode_t mode,
		     unsigned int cmd, unsigned long arg)
{
	struct lbz_device *d = b->bd_disk->private_data;

	if (is_dev_faulty(d) && is_dev_ready(d))
		return 0;
	return -EIO;
}

static const struct block_device_operations lbz_dev_ops = {
	.submit_bio	= lbz_dev_submit_bio,
	.open		= open_dev,
	.release	= release_dev,
	.ioctl		= ioctl_dev,
	.owner		= THIS_MODULE,
};

static inline int first_minor_to_idx(int first_minor)
{
	return (first_minor / LBZ_MINORS);
}

static inline int idx_to_first_minor(int idx)
{
	return (idx * LBZ_MINORS);
}

static void __read_device_info(struct lbz_device *dev, struct seq_file *seq)
{
	long gc_reset_blocks = atomic64_read(&dev->gc_reset_blocks);

	seq_printf(seq, "devname: %s\n"
					"meta_bytes:%u\n"
					"dev_size: %llu(%llu GiB)\n"
					"refcnt:%u\n"
					"transaction_id:%lld\n"
					"timestamp:%lld\n"
					"nr_zones: %u\n"
					"flags: %lu\n"
					"user_write_inflight_io_cnt: %lld\n"
					"user_read_inflight_io_cnt: %lld\n"
					"user_read_err_cnt: %lld\n"
					"user_read_zero_cnt: %lld\n"
					"user_write_err_cnt: %lld\n"
					"user_encounter_emergency: %lld\n"
					"write_alloc_encounter_eagain: %lld\n"
					"gc_alloc_encounter_eagain: %lld\n"
					"user_write_blocks: %lld(%lld GiB)\n"
					"user_read_blocks: %lld(%lld GiB)\n"
					"gc_inflight_io_cnt: %lld\n"
					"gc_write_err_cnt: %lld\n"
					"gc_read_err_cnt: %lld\n"
					"gc_read_blocks: %lld(%lld GiB)\n"
					"gc_write_blocks: %lld(%lld GiB)\n"
					"gc_write_agency_blocks: %lld\n"
					"gc_write_complete_blocks: %lld\n"
					"gc_write_discarded_blocks: %lld\n"
					"gc_reset_blocks: %ld\n"
					"gc read percentage: %lld%%\n"
					"gc write percentage: %lld%%\n"
					"lbz_mem_bytes: %lld(%llu MiB)\n",
					dev->devname,
					dev->meta_bytes,
					dev->dev_size, dev->dev_size >> (30 - SECTOR_SHIFT),
					atomic_read(&dev->refcnt),
					atomic64_read(&dev->transaction_id),
					atomic64_read(&dev->timestamp),
					dev->nr_zones,
					dev->flags,
					atomic64_read(&dev->user_write_inflight_io_cnt),
					atomic64_read(&dev->user_read_inflight_io_cnt),
					atomic64_read(&dev->user_read_err_cnt),
					atomic64_read(&dev->user_read_zero_cnt),
					atomic64_read(&dev->user_write_err_cnt),
					atomic64_read(&dev->user_encounter_emergency),
					atomic64_read(&dev->write_alloc_encounter_eagain),
					atomic64_read(&dev->gc_alloc_encounter_eagain),
					atomic64_read(&dev->user_write_blocks), atomic64_read(&dev->user_write_blocks) >> (30 - LBZ_DATA_BLK_SHIFT),
					atomic64_read(&dev->user_read_blocks), atomic64_read(&dev->user_read_blocks) >> (30 - LBZ_DATA_BLK_SHIFT),
					atomic64_read(&dev->gc_inflight_io_cnt),
					atomic64_read(&dev->gc_write_err_cnt),
					atomic64_read(&dev->gc_read_err_cnt),
					atomic64_read(&dev->gc_read_blocks), atomic64_read(&dev->gc_read_blocks) >> (30 - LBZ_DATA_BLK_SHIFT),
					atomic64_read(&dev->gc_write_blocks), atomic64_read(&dev->gc_write_blocks) >> (30 - LBZ_DATA_BLK_SHIFT),
					atomic64_read(&dev->gc_write_agency_blocks),
					atomic64_read(&dev->gc_write_complete_blocks),
					atomic64_read(&dev->gc_write_discarded_blocks),
					gc_reset_blocks,
					gc_reset_blocks == 0 ? 0 : atomic64_read(&dev->gc_read_blocks) * 100 / gc_reset_blocks,
					gc_reset_blocks == 0 ? 0 : atomic64_read(&dev->gc_write_blocks) * 100 / gc_reset_blocks,
					atomic64_read(&lbz_mem_bytes), atomic64_read(&lbz_mem_bytes) >> 20);
}

static int read_lbz_dev_proc(struct seq_file *seq, void *v)
{
	struct lbz_device *dev = PDE_DATA(file_inode(seq->file));

	seq_printf(seq, "------------device info------------\n");
	__read_device_info(dev, seq);
	seq_printf(seq, "------------io scheduler------------\n");
	lbz_iosched_proc_read(dev->iosched, seq);
	seq_printf(seq, "------------gc context------------\n");
	lbz_gc_proc_read(dev->gc_ctx, seq);
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	seq_printf(seq, "------------nat-sit mgmt------------\n");
	lbz_nat_sit_proc_read(dev->nat_sit_mgmt, seq);
#endif
	seq_printf(seq, "------------zone metadata------------\n");
	lbz_zone_proc_read(dev->zone_metadata, seq);

	return 0;
}

static void *dev_seq_start(struct seq_file *seq, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *dev_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return NULL;
}

static void dev_seq_stop(struct seq_file *seq, void *v)
{

}

static const struct seq_operations lbz_dev_proc_ops = {
	.start = dev_seq_start,
	.next = dev_seq_next,
	.stop = dev_seq_stop,
	.show = read_lbz_dev_proc,
};
static void lbz_dev_remove_proc(struct lbz_device *dev)
{
	remove_proc_entry(dev->devname, lbz_dev_proc_file);
}

static int lbz_dev_create_proc(struct lbz_device *dev)
{
	dev->proc_entry = proc_create_seq_data(dev->devname, 0644, lbz_dev_proc_file, &lbz_dev_proc_ops, dev);
	if (!dev->proc_entry) {
		LBZERR("(%s)create dev proc entry failed!", dev->devname);
		return -ENOENT;
	}
	return 0;
}

static int lbz_device_init(struct lbz_device *d, unsigned int block_size,
		sector_t sectors, struct block_device *phy_bdev,
		const struct block_device_operations *ops)
{
	struct request_queue *q;
	int idx;

	idx = ida_simple_get(&lbz_device_idx, 0,
				LBZ_DEVICE_IDX_MAX, GFP_KERNEL);
	if (idx < 0) {
		LBZINFO("LBZ not get minor for lbz device");
		goto out;
	}
	d->minor = idx;

	if (block_size > PAGE_SIZE) {
		LBZINFO("%s: sb/logical block size (%u) greater than page size (%lu)",
			d->disk->disk_name, q->limits.logical_block_size, PAGE_SIZE);
        goto out;
	}

	d->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!d->disk)
		goto out_ida_remove;

	set_capacity(d->disk, sectors);
	d->dev_size = sectors;
	atomic_set(&d->refcnt, 0);
	atomic64_set(&d->transaction_id, 0);
	atomic64_set(&d->timestamp, 0);
	d->nr_zones = blk_queue_nr_zones(bdev_get_queue(phy_bdev));
	d->meta_bytes = 64; /*default config.*/
	snprintf(d->disk->disk_name, DISK_NAME_LEN, "lbz%i", idx);


	atomic64_set(&d->user_write_inflight_io_cnt, 0);
	atomic64_set(&d->user_read_inflight_io_cnt, 0);
	atomic64_set(&d->user_read_err_cnt, 0);
	atomic64_set(&d->user_read_zero_cnt, 0);
	atomic64_set(&d->user_write_err_cnt, 0);

	atomic64_set(&d->user_encounter_emergency, 0);
	atomic64_set(&d->write_alloc_encounter_eagain, 0);
	atomic64_set(&d->gc_alloc_encounter_eagain, 0);

	atomic64_set(&d->user_write_blocks, 0);
	atomic64_set(&d->user_read_blocks, 0);

	atomic64_set(&d->gc_inflight_io_cnt, 0);
	atomic64_set(&d->gc_write_err_cnt, 0);
	atomic64_set(&d->gc_read_err_cnt, 0);

	atomic64_set(&d->gc_read_blocks, 0);
	atomic64_set(&d->gc_write_blocks, 0);
	atomic64_set(&d->gc_write_agency_blocks, 0);
	atomic64_set(&d->gc_write_complete_blocks, 0);
	atomic64_set(&d->gc_write_discarded_blocks, 0);
	atomic64_set(&d->gc_reset_blocks, 0);

	snprintf(d->devname, DISK_NAME_LEN, "lbz%i", idx);

	d->disk->major		= lbz_major;
	d->disk->first_minor	= idx_to_first_minor(idx);
	d->disk->minors		= LBZ_MINORS;
	d->disk->fops		= ops;
	d->disk->private_data	= d;

	q = d->disk->queue;
	q->limits.max_hw_sectors	= UINT_MAX;
	q->limits.max_sectors		= UINT_MAX;
	q->limits.max_segment_size	= UINT_MAX;
	q->limits.max_segments		= BIO_MAX_VECS;
	blk_queue_max_discard_sectors(q, UINT_MAX);
	q->limits.discard_granularity	= 512;
	q->limits.io_min		= block_size;
	q->limits.logical_block_size	= block_size;
	q->limits.physical_block_size	= block_size;

	blk_queue_flag_set(QUEUE_FLAG_NONROT, d->disk->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, d->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, d->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, d->disk->queue);

	/*
	 * set flush and fua to true.
	 */
	blk_queue_write_cache(q, true, true);

	return 0;

out_ida_remove:
	ida_simple_remove(&lbz_device_idx, idx);
out:
	return -ENOMEM;
}

static void lbz_device_free(struct lbz_device *d)
{
	struct gendisk *disk = d->disk;

	if (disk)
		LBZINFO("%s stopped\n", disk->disk_name);
	else
		LBZINFO("lbz device (NULL gendisk) stopped\n");

	if (disk) {
		bool disk_added = (disk->flags & GENHD_FL_UP) != 0;
		int first_minor = disk->first_minor;

		if (disk_added)
			del_gendisk(disk);

		blk_cleanup_disk(disk);
		ida_simple_remove(&lbz_device_idx, first_minor);
	}
}

void lbz_remove_device(struct lbz_device *d)
{
	wait_queue_head_t wq;

	init_waitqueue_head(&wq);
	lbz_dev_set_remove(d);
	/*wait io complete.*/
	do {
		wait_event_timeout(wq,
				atomic64_read(&d->gc_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_write_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_read_inflight_io_cnt) == 0 &&
				atomic_read(&d->refcnt) == 0, HZ);
	} while (!(atomic64_read(&d->gc_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_write_inflight_io_cnt) == 0 &&
				atomic64_read(&d->user_read_inflight_io_cnt) == 0 &&
				atomic_read(&d->refcnt) == 0));
	/*set unready before waitting may let IO can not be sent to phy_bdev and wait forever.*/
	lbz_dev_set_unready(d);
	lbz_dev_remove_proc(d);
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	lbz_nat_sit_destroy(d->nat_sit_mgmt);
	LBZ_FREE_MEM(d->nat_sit_mgmt, sizeof(struct lbz_nat_sit_mgmt));
#endif
	lbz_destroy_gc_thread(d->gc_ctx);
	LBZ_FREE_MEM(d->gc_ctx, sizeof(struct lbz_gc_context));
	lbz_iosched_destory(d->iosched);
	LBZ_FREE_MEM(d->iosched, sizeof(struct lbz_io_scheduler));
	lbz_mapping_destroy(d->mapping);
	LBZ_FREE_MEM(d->mapping, sizeof(struct lbz_mapping));
	lbz_destroy_zone_metadata(d->zone_metadata);
	LBZ_FREE_MEM(d->zone_metadata, sizeof(struct lbz_zone_metadata));
    lbz_device_free(d);
	list_del_init(&d->list);
	blkdev_put(d->phy_bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
	LBZ_FREE_MEM(d, sizeof(struct lbz_device));
}

int lbz_create_device(unsigned int block_size,
		sector_t sectors, struct block_device *phy_bdev)
{
	int ret = 0;
	struct lbz_device *d;
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	struct nat_sit_args args;
#endif

	LBZ_ALLOC_MEM(d, sizeof(struct lbz_device), GFP_KERNEL);
	if (!d) {
		LBZERR("malloc lbz_device encounter err: -ENOMEM");
		return -ENOMEM;
	}
	LBZINFO("alloc lbz_device: %lx", (unsigned long)d);

	d->phy_bdev = phy_bdev;
	INIT_LIST_HEAD(&d->list);
	ret = lbz_device_init(d, block_size, sectors, phy_bdev, &lbz_dev_ops);
	if (ret) {
		LBZERR("init lbz device encounter err: %d", ret);
		goto out;
	}
	LBZINFO("init device");

	LBZ_ALLOC_MEM(d->zone_metadata, sizeof(struct lbz_zone_metadata), GFP_NOIO);
	ret = lbz_init_zone_metadata(d->zone_metadata, d);
	if (ret < 0) {
		LBZERR("init zone failed: %d", ret);
		goto zone_err;
	}
	LBZINFO("init zone_metadata: %lx", (unsigned long)d->zone_metadata);

	LBZ_ALLOC_MEM(d->mapping, sizeof(struct lbz_mapping), GFP_NOIO);
	lbz_mapping_init(d->mapping, d);
	LBZINFO("init mapping: %lx", (unsigned long)d->mapping);

	LBZ_ALLOC_MEM(d->iosched, sizeof(struct lbz_io_scheduler), GFP_NOIO);
	ret = lbz_iosched_init(d->iosched, d);
	if (ret < 0) {
		LBZERR("init iosched failed: %d", ret);
		goto iosched_err;
	}
	LBZINFO("init iosched: %lx", (unsigned long)d->iosched);

	LBZ_ALLOC_MEM(d->gc_ctx, sizeof(struct lbz_gc_context), GFP_NOIO);
	ret = lbz_create_gc_thread(d->gc_ctx, d);
	if (ret < 0) {
		LBZERR("init gc_ctx failed: %d", ret);
		goto gc_err;
	}
	LBZINFO("init gc: %lx", (unsigned long)d->gc_ctx);

	ret = lbz_dev_create_proc(d);
	if (ret < 0) {
		LBZERR("create proc file: %s failed: %d", d->devname, ret);
		goto proc_err;
	}
	ret = bioset_init(&d->bio_split, BIO_POOL_SIZE, 0, 0);
	if (ret < 0) {
		LBZERR("init bio_split failed: %d", ret);
		goto bs_err;
	}
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	LBZ_ALLOC_MEM(d->nat_sit_mgmt, sizeof(struct lbz_nat_sit_mgmt), GFP_NOIO);
	memset(&args, 0x0, sizeof(struct nat_sit_args));
	lbz_nat_sit_add_rule(d->nat_sit_mgmt, &args, d);
#endif
	list_add(&d->list, &lbz_devices);
	add_disk(d->disk);
	lbz_dev_set_ready(d);
	LBZINFO("Added disk: %s, size: %llu sectors", d->disk->disk_name, sectors);
	return 0;
bs_err:
	lbz_dev_remove_proc(d);
proc_err:
	lbz_destroy_gc_thread(d->gc_ctx);
gc_err:
	LBZ_FREE_MEM(d->gc_ctx, sizeof(struct lbz_gc_context));
	lbz_iosched_destory(d->iosched);
iosched_err:
	LBZ_FREE_MEM(d->iosched, sizeof(struct lbz_io_scheduler));
	lbz_mapping_destroy(d->mapping);
	LBZ_FREE_MEM(d->mapping, sizeof(struct lbz_mapping));
	lbz_destroy_zone_metadata(d->zone_metadata);
zone_err:
	LBZ_FREE_MEM(d->zone_metadata, sizeof(struct lbz_zone_metadata));
	lbz_device_free(d);
out:
	LBZ_FREE_MEM(d, sizeof(struct lbz_device));
	return ret;
}

struct lbz_device *lbz_dev_find_by_minor(int minor)
{
	struct lbz_device *d, *tmp;

	list_for_each_entry_safe(d, tmp, &lbz_devices, list) {
		if (d->minor == minor)
			return d;
	}
	return NULL;
}
int lbz_dev_init(void)
{
	int ret = 0;

	lbz_major = register_blkdev(0, "lbz");
	if (lbz_major < 0) {
		LBZERR("register major failed: %d", lbz_major);
		return lbz_major;
	}
	lbz_dev_proc_file = proc_mkdir(LBZ_DEV_ENTRY_FILENAME, NULL);
	if (!lbz_dev_proc_file) {
		ret = -EPERM;
		unregister_blkdev(lbz_major, "lbz");
		LBZERR("create dev proc dir failed: %d", ret);
		return ret;
	}

	return 0;
}

void lbz_dev_exit(void)
{
	struct lbz_device *d, *tmp;

	list_for_each_entry_safe(d, tmp, &lbz_devices, list) {
		lbz_remove_device(d);
	}
	if (lbz_major)
		unregister_blkdev(lbz_major, "lbz");
	proc_remove(lbz_dev_proc_file);
	LBZINFO("lbz dev: see you later, you have %lld bytes not free!", atomic64_read(&lbz_mem_bytes));
	BUG_ON(atomic64_read(&lbz_mem_bytes) != 0);
}
/*
 * module_exit(lbz_exit);
 * module_init(lbz_init);
 * MODULE_DESCRIPTION("LBZ: a Log-structured block device for zns");
 * MODULE_AUTHOR("Yang Yongpeng <yangyongpeng18b@ict.ac.cn>");
 * MODULE_LICENSE("GPL");
 */
