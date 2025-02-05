#include "lbz-request.h"
#include "lbz-io-scheduler.h"
#include "lbz-dev.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-request"

extern struct request *nvme_alloc_request(struct request_queue *q,
		struct nvme_command *cmd, blk_mq_req_flags_t flags);
static struct nvme_command * prep_nvme_cmd(struct bio *bio, struct lbz_device *d)
{
	struct nvme_command *c = kzalloc(sizeof(struct nvme_command), GFP_NOIO);
	enum nvme_opcode write = (bio_data_dir(bio) == WRITE ? nvme_cmd_write : nvme_cmd_read);
	u32 slba = bio->bi_iter.bi_sector >> (PAGE_SHIFT - 9);

	c->rw.opcode = write;
	c->rw.flags = 0;
	c->rw.nsid = cpu_to_le32(d->phy_bdev->bd_disk->fops->ioctl(d->phy_bdev, 0, NVME_IOCTL_ID, 0));
	c->rw.slba = cpu_to_le64(slba);
	c->rw.length = cpu_to_le16(0);
	c->rw.control = cpu_to_le16(0);
	c->rw.dsmgmt = cpu_to_le32(0);
	c->rw.reftag = cpu_to_le32(0);
	c->rw.apptag = cpu_to_le16(0);
	c->rw.appmask = cpu_to_le16(0);

	return c;
}

void copy_user_bio_data(struct bio *bio, struct page *pages)
{
	struct bio_vec bv;                                    
	struct bvec_iter iter;

	bio_for_each_bvec(bv, bio, iter) {
		memcpy(page_address(pages), page_address(bv.bv_page), PAGE_SIZE);
	}
}

void copy_data_to_user(struct bio *bio, struct page *pages)
{
	struct bio_vec bv;                                    
	struct bvec_iter iter;

	bio_for_each_bvec(bv, bio, iter) {
		memcpy(page_address(bv.bv_page), page_address(pages), PAGE_SIZE);
	}
}

static void lbz_req_end_fn(struct request *rq, blk_status_t error)                                   
{                                    
	struct lbz_req_hook *hook = rq->end_io_data;

	hook->user_bio->bi_status = error;
	if (bio_data_dir(hook->user_bio) == READ)
		copy_data_to_user(hook->user_bio, hook->clone_pages);
	__free_pages(hook->clone_pages, 1);
	bio_endio(hook->user_bio);
	if (error)
		LBZERR("request call back:%lx, %d, hook: %lx", (unsigned long)rq, error, (unsigned long)hook);
	/*
	 * up to here, rq->bio is NULL. bio_map_kern_endio will bio_put clone_bio.
	 * bio_put(hook->clone_bio);
	 */
	blk_mq_free_request(rq);
	kfree(hook->c);
	kfree(hook);
}

int __attribute__ ((unused)) clone_and_prep_request(struct bio *bio) {
	struct lbz_device *d = bio->bi_bdev->bd_disk->private_data;
	struct page *pages;
	int ret = 0;
	struct bio *clone_bio;
	struct block_device *bd_disk = d->phy_bdev->bd_disk->part0;
	struct lbz_req_hook *hook = kzalloc(sizeof(struct lbz_req_hook), GFP_NOIO);
	struct nvme_command *c;
	struct request *req;
	u32 slba = bio->bi_iter.bi_sector >> (PAGE_SHIFT - 9);

	c = prep_nvme_cmd(bio, d);
	req = nvme_alloc_request(d->phy_bdev->bd_disk->queue, c, 0);
	if (IS_ERR(req)) {
		ret = PTR_ERR(req);
		goto err_alloc_rq;
	}
	pages = alloc_pages(GFP_NOIO, 1);
	if (bio_data_dir(bio) == WRITE) {
		if (slba % 2 == 1)
			memset(page_address(pages + 1), 0x89, 64);
		else
			memset(page_address(pages + 1), 0x77, 64);
	}
	if (bio_data_dir(bio) == WRITE)
		copy_user_bio_data(bio, pages);
	/*
	 * create bio, map pages to bio and assign it to req.
	 */
	ret = blk_rq_map_kern(d->phy_bdev->bd_disk->queue, req, page_address(pages), PAGE_SIZE + 64, GFP_NOIO);
	if (ret) {
		LBZERR("map pages:%lx to bio encounter error: %d", (unsigned long)pages, ret);
		goto err_map_kern;
	}
	clone_bio = req->bio;
	bio_set_dev(clone_bio, bd_disk);

	hook->clone_pages = pages;
	hook->user_bio = bio;
	hook->c = c;
	req->end_io_data = hook;
	blk_execute_rq_nowait(d->phy_bdev->bd_disk, req, false, lbz_req_end_fn);
	/*panic("alloc req successfully req:%lx, command: %lx", (unsigned long)req, (unsigned long)c);*/
	return 0;
err_map_kern:
	__free_pages(pages, 1);
	blk_mq_free_request(req);
err_alloc_rq:
	kfree(c);
	return ret;
}
/***************just for test io path***************/
static void __attribute__ ((unused)) lbz_bio_endio(struct bio *bio) {
	struct lbz_bio_hook *hook = (struct lbz_bio_hook *)bio->bi_private;

	bio->bi_private = hook->user_private;
	bio->bi_end_io = hook->user_endio;
	kfree(hook);
	//bio_integrity_free(bio);
	bio_endio(bio);
}

/*block layer request for profile, but nvme driver don't have when not support pi.*/
//void * __attribute__ ((unused)) add_integrity(struct bio *bio) {
void * add_integrity(struct bio *bio) {
	int len = 64;
	u32 seed = bio->bi_iter.bi_sector >> (PAGE_SHIFT - 9);
	struct bio_integrity_payload *bip;
	int ret = -ENOMEM;
	void *buf;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		goto out;

	memset(buf, 0x89, len);
	bip = bio_integrity_alloc(bio, GFP_KERNEL, 1);
	if (IS_ERR(bip)) {
		ret = PTR_ERR(bip);
		goto out_free_meta;
	}

	bip->bip_iter.bi_size = len;
	bip->bip_iter.bi_sector = seed;
	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
			offset_in_page(buf));
	if (ret == len)
		return buf;
	ret = -ENOMEM;
out_free_meta:
	kfree(buf);
out:
	return ERR_PTR(ret);
}
/***************just for test io path***************/

#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
struct lbz_flush_hook {
	void *src_priv;
	bio_end_io_t *src_endio;
	struct completion *done;
};

static void lbz_flush_bio_endio(struct bio *bio)
{
	struct lbz_flush_hook *hook = bio->bi_private;

	complete(hook->done);
}

static void __lbz_submit_flush_bio_wait(struct lbz_device *dev, struct bio *bio)
{
	DECLARE_COMPLETION_ONSTACK_MAP(done,
			bio->bi_bdev->bd_disk->lockdep_map);
	unsigned long hang_check;
	struct lbz_flush_hook *hook;
	int errno = 0;
	struct bio_list *bls = current->bio_list;

	LBZ_ALLOC_MEM(hook, sizeof(struct lbz_flush_hook), GFP_NOIO);
	hook->src_priv = bio->bi_private;
	hook->src_endio = bio->bi_end_io;
	hook->done = &done;
	bio->bi_private = hook;
	bio->bi_end_io = lbz_flush_bio_endio;
	bio->bi_opf |= REQ_SYNC;
	current->bio_list = NULL;
	lbz_submit_io_to_iosched(dev->iosched, bio);
	current->bio_list = bls;

	/* Prevent hang_check timer from firing at us during very long I/O */
	hang_check = sysctl_hung_task_timeout_secs;
	if (hang_check)
		while (!wait_for_completion_io_timeout(&done,
					hang_check * (HZ/2)))
			;
	else
		wait_for_completion_io(&done);

	errno = blk_status_to_errno(bio->bi_status);
	/*io-scheduler already mark dev as faulty when encounter error.*/
#ifdef CONFIG_LBZ_NAT_SIT_FREE_SUPPORT
	if (!errno) {
		lbz_nat_sit_tx_complete(dev->nat_sit_mgmt);
	}
#endif
	bio->bi_private = hook->src_priv;
	bio->bi_end_io = hook->src_endio;
	bio_endio(bio);
	LBZ_FREE_MEM(hook, sizeof(struct lbz_flush_hook));
}
#endif

struct lbz_long_context {
	struct bio *user_bio_long;
	void *user_private_long;
	bio_end_io_t *user_endio_long;
	atomic_t remaining;
};

static void lbz_long_bio_endio(struct bio *bio)
{
	struct lbz_long_context *ctx = bio->bi_private;
	struct bio *user_bio = ctx->user_bio_long;

	if (bio->bi_status && !user_bio->bi_status)
		user_bio->bi_status = bio->bi_status;

	if (atomic_dec_and_test(&ctx->remaining)) {
		user_bio->bi_private = ctx->user_private_long;
		user_bio->bi_end_io = ctx->user_endio_long;
		bio_endio(user_bio);
		LBZ_FREE_MEM(ctx, sizeof(struct lbz_long_context));
	}
	if (bio != user_bio)
		bio_put(bio);

}

blk_qc_t lbz_dev_submit_bio(struct bio *bio)
{
	/*
	 * struct lbz_device *d = bio->bi_bdev->bd_disk->private_data;
	 * struct lbz_bio_hook *hook;
	 */
	struct lbz_device *dev = bio->bi_bdev->bd_disk->private_data;
	unsigned max_sectors = 1 << (LBZ_DATA_BLK_SHIFT - SECTOR_SHIFT);
	unsigned int blkid = sector_to_blkid(bio->bi_iter.bi_sector);

	if (op_is_flush(bio->bi_opf)) {
		/*don't need to wait because f2fs had waited.*/
#if 0
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		wait_queue_head_t wq;

		init_waitqueue_head(&wq);
		/*wait io complete.*/
		do {
			wait_event_timeout(wq,
					atomic64_read(&dev->nat_sit_mgmt->nat_sit_inflight_writes) == 0, HZ);
		} while (atomic64_read(&dev->nat_sit_mgmt->nat_sit_inflight_writes) > 0);
#endif
#endif

		LBZDEBUG("receive write flush: %u", blkid);
		/*return directly for flush bio without data, which may be issued by blkdev_issue_flush.*/
		if (!bio_has_data(bio)) {
			bio_endio(bio);
			return BLK_QC_T_NONE;
		}
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		if (lbz_nat_sit_is_cp_block(dev->nat_sit_mgmt, blkid)) {
#ifdef CONFIG_LBZ_NAT_SIT_FREE_SUPPORT
			__lbz_submit_flush_bio_wait(dev, bio);
#else
			lbz_submit_io_to_iosched(dev->iosched, bio);
#endif
			lbz_nat_sit_set_cur_cp(dev->nat_sit_mgmt, blkid);
			return BLK_QC_T_NONE;
		}
#endif
	}

	if (!bio_has_data(bio) || is_dev_faulty(dev) || !is_dev_ready(dev)) {
		bio_endio(bio);
		return BLK_QC_T_NONE;
	}

#if 0
	if (bio_sectors(bio) > max_sectors) {
		struct bio *split = bio_split(bio, max_sectors, GFP_NOIO, &dev->bio_split);

		LBZDEBUG("receive bio bi_size: %u, bi_vcnt:%d", bio->bi_iter.bi_size, bio->bi_vcnt);
		bio_chain(split, bio);
		submit_bio_noacct(bio);
		bio = split;
	}
#endif
	if (bio_sectors(bio) > max_sectors) {
		struct lbz_long_context *ctx;

		LBZ_ALLOC_MEM(ctx, sizeof(struct lbz_long_context), GFP_NOIO);
		ctx->user_bio_long = bio;
		ctx->user_endio_long = bio->bi_end_io;
		ctx->user_private_long = bio->bi_private;
		atomic_set(&ctx->remaining, 1);

		while (bio_sectors(bio) > max_sectors) {
			struct bio *split = bio_split(bio, max_sectors, GFP_NOIO, &dev->bio_split);

			LBZDEBUG("receive bio bi_size: %u, bi_vcnt:%d", bio->bi_iter.bi_size, bio->bi_vcnt);
			atomic_inc(&ctx->remaining);
			split->bi_private = ctx;
			split->bi_end_io = lbz_long_bio_endio;

			lbz_submit_io_to_iosched(dev->iosched, split);
		}
		bio->bi_private = ctx;
		bio->bi_end_io = lbz_long_bio_endio;
	}

	lbz_submit_io_to_iosched(dev->iosched, bio);
	/*
	 *if (bio_data_dir(bio) == WRITE) {
	 *	bio->bi_iter.bi_sector = 0;
	 *	bio->bi_opf |= REQ_OP_ZONE_APPEND;
	 *	bio->bi_bdev = d->phy_bdev;
	 *	add_integrity(bio);
	 *	submit_bio(bio);
	 *} else
	 *	bio_endio(bio);
	 */
	/*clone_and_prep_request(bio);*/
	//LBZINFO("bio->bi_max_vecs : %d", bio->bi_max_vecs);
	//clone_and_prep_request(bio);
	return BLK_QC_T_NONE;
	/*endio immediately.*/
	/*bio_endio(bio); */
	/* try to send bio by blk_integrity related func, but it doesn't work, because it only
	 * support protection information.
	 *
	 * hook = kmalloc(sizeof(struct lbz_bio_hook), GFP_NOIO);

	 * hook->user_endio = bio->bi_end_io;
	 * hook->user_private = bio->bi_private;
	 * bio->bi_private = hook;
	 * bio->bi_iter.bi_sector = 0;
	 * //bio->bi_opf |= (bio_data_dir(bio) == WRITE ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN);//invalid operation.
	 * bio->bi_opf |= REQ_NOMERGE_FLAGS;
	 * bio->bi_end_io = lbz_bio_endio;
	 * bio->bi_bdev = d->phy_bdev;
	 * //add_integrity(bio);
	 * //submit_bio(bio);
	 * //return submit_bio(bio);
	 */
	//return BLK_QC_T_NONE;
}
