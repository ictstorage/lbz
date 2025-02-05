#include "lbz-io-scheduler.h"
#include "lbz-dev.h"
#include "lbz-zone-metadata.h"
#include "lbz-mapping.h"
#include "lbz-gc.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-iosched"

static void task_get(struct lbz_io_task *task);
static void __gc_task_callback(struct lbz_io_task *task, unsigned int pbid);
static int __submit_gc_task(struct lbz_io_scheduler *iosched, struct lbz_io_task *task);
static void trigger_retry_work(struct lbz_io_scheduler *iosched, unsigned long delay);
static void __add_task_to_retry(struct lbz_io_scheduler *iosched, struct lbz_io_task *task);

static struct lbz_io_task *insert_task_to_tree(struct lbz_io_scheduler *iosched, struct lbz_io_task *tk)
{
	struct rb_node **p = &iosched->task_tree.rb_node;
	struct rb_node *parent = NULL;
	struct lbz_io_task *retk = NULL, *tkp = NULL;
	unsigned long flag = 0;
	
	write_lock_irqsave(&iosched->task_lock, flag);
	while (*p) {
		parent = *p;
		tkp = container_of(*p, struct lbz_io_task, node);
		if (tk->blkid > tkp->blkid) {
			p = &(*p)->rb_right;
		} else if (tk->blkid < tkp->blkid) {
			p = &(*p)->rb_left;
		} else {
			retk = tkp;
			task_get(retk);
			goto out;
		}
	}
	rb_link_node(&tk->node, parent, p);
	rb_insert_color(&tk->node, &iosched->task_tree);
	task_get(tk); /*after: task's refcount is 2.*/
	atomic64_inc(&iosched->task_count);
out:
	write_unlock_irqrestore(&iosched->task_lock, flag);
	return retk;
}

static void del_task_from_tree(struct lbz_io_scheduler *iosched, struct lbz_io_task *tk)
{
	unsigned long flag = 0;

	write_lock_irqsave(&iosched->task_lock, flag);
	rb_erase(&tk->node, &iosched->task_tree);
	write_unlock_irqrestore(&iosched->task_lock, flag);
	atomic64_dec(&iosched->task_count);
}

static __attribute__ ((unused)) struct lbz_io_task *search_task(struct lbz_io_scheduler *iosched, unsigned int blkid)
{
	struct rb_node **p = &iosched->task_tree.rb_node;
	struct lbz_io_task *tk = NULL;
	unsigned long flag = 0;

	read_lock_irqsave(&iosched->task_lock, flag);
	while (*p) {
		tk = container_of(*p, struct lbz_io_task, node);
		if (tk->blkid > blkid) {
			p = &(*p)->rb_right;
		} else if (tk->blkid < blkid) {
			p = &(*p)->rb_left;
		} else {
			break;
		}
		tk = NULL;
	}
	if (tk) {
		task_get(tk);
	}
	read_unlock_irqrestore(&iosched->task_lock, flag);
	return tk;
}

static __attribute__ ((unused)) bool is_task_tree_empty(struct lbz_io_scheduler *iosched)
{
	bool empty;
	unsigned long flag = 0;

	read_lock_irqsave(&iosched->task_lock, flag);
	empty = RB_EMPTY_ROOT(&iosched->task_tree);
	read_unlock_irqrestore(&iosched->task_lock, flag);

	return empty;
}

static void task_get(struct lbz_io_task *task)
{
	atomic_inc(&task->refcount);
}

static void task_destroy(struct lbz_io_task *task)
{
	if (NULL != task->integrity_buf)
		LBZ_FREE_MEM(task->integrity_buf, sizeof(struct lbz_disk_log));
	LBZ_FREE_MEM(task, sizeof(struct lbz_io_task));
}

static void task_put(struct lbz_io_task *task)
{
	smp_mb__before_atomic();
	if (atomic_dec_and_test(&task->refcount)) {
		smp_mb__after_atomic();
		if (task->type != LBZ_TASK_GC && NULL != task->pending_gc_node) {
			struct lbz_io_scheduler *iosched = task->pending_gc_node->iosched;
			struct lbz_device *dev = iosched->host;

			/*write error will deliver to gc.*/
			if (task->error < 0)
				task->pending_gc_node->error = task->error;
			/*task->error == -ENOENT, callback just invalid reverse mapping.*/
			__gc_task_callback(task->pending_gc_node, LBZ_INVALID_PBID);
			lbz_dec_gc_inflight(dev);
		}
		task_destroy(task);
	}
}

static void __task_init(struct lbz_io_task *task)
{
	/*list and node need to be initialized before using.*/
	atomic_set(&task->refcount, 1);
	task->status = LBZ_TASK_INIT;
	task->error = 0;
	task->zone = NULL;
	task->pending_gc_node = NULL;
	RB_CLEAR_NODE(&task->node);
	INIT_LIST_HEAD(&task->list);
}

static struct lbz_io_task *task_alloc(enum lbz_task_type type, gfp_t flag)
{
	struct lbz_io_task *task;

	LBZ_ALLOC_MEM(task, sizeof(struct lbz_io_task), GFP_NOIO);
	__task_init(task);
	task->type = type;

	return task;
}

static void __unhook_io(struct bio *bio)
{
	struct lbz_io_hook *hook = bio->bi_private;

	bio->bi_private = hook->user_private;
	bio->bi_end_io = hook->user_endio;
	
	bio_endio(bio);
	LBZ_FREE_MEM(hook, sizeof(struct lbz_io_hook));
}

static void lbz_read_io_endio(struct bio *bio)
{
	struct lbz_io_hook *hook = bio->bi_private;
	struct lbz_device *dev = hook->dev;
	struct lbz_zone *zone = hook->zone;
	int errno = blk_status_to_errno(bio->bi_status);

	if (errno < 0) {
		atomic64_inc(&dev->user_read_err_cnt);
		LBZERR("read IO encounter error: %d", errno);
		lbz_dev_set_faulty(dev);
	}
	__unhook_io(bio);
	lbz_put_zone(zone); /*lbz_mapping_lookup*/
	atomic64_dec(&dev->user_read_inflight_io_cnt);
}

static void lbz_write_io_endio(struct bio *bio)
{
	struct lbz_io_hook *hook = bio->bi_private;
	struct lbz_device *dev = hook->dev;
	struct lbz_io_task *task = hook->task;
	struct lbz_zone *zone = task->zone;
	sector_t ret_sec = bio->bi_iter.bi_sector;
	unsigned int pbid = sector_to_blkid(ret_sec), old_pbid = LBZ_INVALID_PBID;
	int errno = blk_status_to_errno(bio->bi_status);

	if (0 == errno) {
		old_pbid = lbz_mapping_add(dev->mapping, task->blkid, pbid);
		if (old_pbid != LBZ_INVALID_PBID) {
			struct lbz_zone *old_zone = get_zone_by_pbid(dev->zone_metadata, old_pbid);

			/*GC and write callback will not exec at the same time, global release won't dec zero.*/
			lbz_zone_update_reverse_map(dev->zone_metadata,
					old_zone, old_pbid, LBZ_INVALID_PBID);
			lbz_zone_release_global_res(dev->zone_metadata, old_zone);
		}
		lbz_zone_update_reverse_map(dev->zone_metadata, zone, pbid, task->blkid);
	} else {
		atomic64_inc(&dev->user_write_err_cnt);
		LBZERR("write IO encounter error: %d", errno);
		lbz_dev_set_faulty(dev);
	}
	__unhook_io(bio);
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	if (task->type != LBZ_TASK_USER_WRITE) {
		lbz_nat_sit_complete_write(dev->nat_sit_mgmt);
	}
#endif
	if (task->status > LBZ_TASK_INIT) {
		del_task_from_tree(dev->iosched, task);
		task_put(task); /*insert_task_to_tree*/
	}
	task_put(task); /*__task_init*/
	lbz_zone_complete_write(zone); /* This statement must be at end of this func and before lbz_put_zone,
									* because zone which have no zone pending write may
									* be gc by gc_thread and conflict with write task if
									* complete write before del_task_from_tree.*/
	lbz_put_zone(zone); /*lbz_zone_alloc_res*/
	atomic64_dec(&dev->user_write_inflight_io_cnt);
}

void __hook_io(struct lbz_device *dev, struct bio *bio, unsigned int blkid, int rw, void *priv)
{
	struct lbz_io_hook *hook = NULL;

	LBZ_ALLOC_MEM(hook, sizeof(struct lbz_io_hook), GFP_NOIO);
	hook->dev = dev;
	hook->priv = priv; /*task for write, zone for read.*/
	hook->blkid = blkid;
	hook->user_private = bio->bi_private;
	hook->user_endio = bio->bi_end_io;
	bio->bi_private = hook;
	if (rw == WRITE) {
		bio->bi_end_io = lbz_write_io_endio;
	} else {
		bio->bi_end_io = lbz_read_io_endio;
		atomic64_inc(&dev->user_read_inflight_io_cnt);
	}
	bio->bi_bdev = dev->phy_bdev;
}

static void __submit_read_io(struct lbz_io_scheduler *iosched, struct bio *bio)
{
	struct lbz_device *dev = iosched->host;
	unsigned int blkid = sector_to_blkid(bio->bi_iter.bi_sector), pbid = LBZ_INVALID_PBID;
	struct lbz_zone *zone;
	int ret = 0;

	ret = lbz_mapping_lookup(dev->mapping, &zone, &pbid, blkid);
	if (ret < 0) {
		LBZINFO_LIMIT("bio: %lx, blkid: %u read zero", (unsigned long)bio, blkid);
		bio_endio(bio);
		atomic64_inc(&dev->user_read_zero_cnt);
		return;
	}

	__hook_io(dev, bio, blkid, READ, zone);
	bio->bi_iter.bi_sector = blkid_to_sector(pbid);
	submit_bio(bio);
}

static void * __add_integrity(struct lbz_io_task *task, struct lbz_device *dev, enum lbz_log_type type)
{
	int len = dev->meta_bytes;
	struct bio *bio = task->bio;
	unsigned int seed = sector_to_blkid(bio->bi_iter.bi_sector);
	struct bio_integrity_payload *bip;
	int ret = 0;
	void *buf;
	struct lbz_disk_log *log;

	LBZ_ALLOC_MEM(buf, len, GFP_NOIO);

	bip = bio_integrity_alloc(bio, GFP_NOIO, 1);
	if (IS_ERR(bip)) {
		ret = PTR_ERR(bip);
		LBZERR("alloc integrity encounter error: %d", ret);
		goto out_free_meta;
	}

	bip->bip_iter.bi_size = len;
	bip->bip_iter.bi_sector = seed;
	ret = bio_integrity_add_page(bio, virt_to_page(buf), len,
			offset_in_page(buf));
	if (ret != len) {
		ret = -EINVAL;
		goto out_free_integ;
	}
	log = (struct lbz_disk_log *)buf;
	log->timestamp = lbz_dev_get_timestamp(dev);
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	switch (task->type) {
	case LBZ_TASK_WRITE_TX_FATHER:
		log->txid = lbz_dev_get_tx_id(dev);
		log->log_type = LBZ_LOG_TRANSACTION_FATHER;
		break;
	case LBZ_TASK_WRITE_TX_CHILD:
		log->txid = lbz_dev_read_tx_id(dev);
		log->log_type = LBZ_LOG_TRANSACTION_CHILD;
		log->free_blkid = lbz_nat_sit_to_free(dev->nat_sit_mgmt, task->blkid);
#ifdef CONFIG_LBZ_NAT_SIT_FREE_SUPPORT
		lbz_nat_sit_add_blkid(dev->nat_sit_mgmt, log->free_blkid);
#endif
		break;
	default:
		log->txid = 0;
		log->log_type = type;
	}
#else
	log->txid = 0;
	log->free_blkid = 0;
	log->log_type = type;
#endif
	log->blkid = task->blkid;
	log->crc = 0; /*TODO:*/
	return buf;

out_free_integ:
	/*
	 * bio_endio will call.
	 * bio_integrity_free(bio);
	 */
out_free_meta:
	LBZ_FREE_MEM(buf, len);
	return ERR_PTR(ret);
}

static int __submit_write_task(struct lbz_io_scheduler *iosched, struct lbz_io_task *task)
{
	struct lbz_device *dev = iosched->host;
	struct lbz_zone *zone = NULL;
	struct lbz_io_task *tk = NULL;
	struct bio *bio = task->bio;
	unsigned int blkid = sector_to_blkid(bio->bi_iter.bi_sector);
	int ret = 0, stream_id = 0;

	task->blkid = blkid;
	switch (task->status) {
	case LBZ_TASK_INIT:
		tk = insert_task_to_tree(iosched, task);
		if (tk != NULL) {
			LBZDEBUG("write IO conflict with task : %d", tk->type);
			ret = -EAGAIN;
			task_put(tk);
			goto out;
		}
		task->status = LBZ_TASK_ALLOC_RES;
	case LBZ_TASK_ALLOC_RES:
		/*if have pending gc task, we can borrow one block from reserved_blks_gc in case dead lock.*/
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		stream_id = lbz_nat_sit_get_stream_id(dev->nat_sit_mgmt, blkid);
		ret = lbz_zone_alloc_res(dev->zone_metadata, &zone,
				task->pending_gc_node == NULL ? LBZ_ALLOC_FLAG_USER : LBZ_ALLOC_FLAG_GC, stream_id);
#ifdef CONFIG_LBZ_NAT_SIT_STREAM_SUPPORT
		//if (ret < 0 && task->pending_gc_node != NULL) {
		if (ret < 0) {
			int i = 0;

			LBZDEBUG("alloc res encounter: %d, stream_id: %d", ret, stream_id);
			for (i = 0; i < LBZ_ZONE_MAX_STREAM; i++) {
				if (i != stream_id) {
					ret = lbz_zone_alloc_res(dev->zone_metadata, &zone,
							task->pending_gc_node == NULL ? LBZ_ALLOC_FLAG_USER : LBZ_ALLOC_FLAG_GC, i);
					if (0 == ret){
						atomic64_inc(&dev->write_alloc_encounter_eagain);
						break;
					}
				}
			}
		}
#endif
#else
		ret = lbz_zone_alloc_res(dev->zone_metadata, &zone,
				task->pending_gc_node == NULL ? LBZ_ALLOC_FLAG_USER : LBZ_ALLOC_FLAG_GC, stream_id);
#endif
		if (ret < 0) {
			LBZDEBUG("alloc res encounter: %d", ret);
			if (ret != -EAGAIN) {
				del_task_from_tree(iosched, task);
				task_put(task); /*insert_task_to_tree*/
			}
			lbz_trigger_gc_reclaim_emergency(dev->gc_ctx);
			goto out;
		}
		if (lbz_check_need_reclaim_high(dev->zone_metadata))
			lbz_trigger_gc_reclaim(dev->gc_ctx);
		task->zone = zone;
		__hook_io(dev, bio, blkid, WRITE, task);
		bio->bi_iter.bi_sector = zone->start_sector;
		bio->bi_opf |= REQ_OP_ZONE_APPEND;
		task->status = LBZ_TASK_IO_WRITING;
	case LBZ_TASK_IO_WRITING:
		task->integrity_buf = __add_integrity(task, dev, LBZ_LOG_USER_WRITE);
		if (IS_ERR(task->integrity_buf)) {
			ret = PTR_ERR(task->integrity_buf);
			lbz_dev_set_faulty(dev);
			BUG_ON(ret == -EAGAIN); /*bi_sector was modified.*/
			LBZERR("add integrity info encounter: %d", ret);
			goto out;
		}
		task->status = LBZ_TASK_DISPATCH;
		break;
	default:
		BUG();
	}
	submit_bio(bio);
	return 0;
out:
	return ret;
}

void __retry_submit_io_task(struct lbz_io_scheduler *iosched)
{
	struct lbz_device *dev = iosched->host;
	struct list_head task_list;
	unsigned long flag = 0;
	struct lbz_io_task *pos, *n;
	enum lbz_task_status status;
	int ret = 0;

	INIT_LIST_HEAD(&task_list);
	spin_lock_irqsave(&iosched->pending_lock, flag);
	list_splice_init(&iosched->lbz_user_pending, &task_list);
	iosched->pending_count = 0;
	spin_unlock_irqrestore(&iosched->pending_lock, flag);

	list_for_each_entry_safe(pos, n, &task_list, list) {
		list_del_init(&pos->list);
		ret = __submit_write_task(iosched, pos);
		switch(ret) {
		case 0:
			break;
		case -EAGAIN:
			__add_task_to_retry(iosched, pos);
			trigger_retry_work(iosched, LBZ_IO_WRITE_DELAY);
			break;
		default:
			pos->bio->bi_status = BLK_STS_IOERR;
			status = pos->status; /*pos may be freed after next statement.*/
			bio_endio(pos->bio);
			/*task will be destroy by lbz_write_io_endio when task->status > LBZ_TASK_ALLOC_RES.*/
			if (status == LBZ_TASK_ALLOC_RES) {
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
				if (pos->type != LBZ_TASK_USER_WRITE)
					lbz_nat_sit_complete_write(dev->nat_sit_mgmt);
#endif
				task_put(pos); /*__task_init*/
				atomic64_dec(&dev->user_write_inflight_io_cnt);
			}
			atomic64_inc(&dev->user_write_err_cnt);
			break;
		}
	}
}

/*
 * Only hanle read IO.
 */
static void __add_task_to_gc_writes(struct lbz_io_scheduler *iosched, struct lbz_io_task *task);
void __retry_submit_gc_task(struct lbz_io_scheduler *iosched)
{
	struct lbz_device *dev = iosched->host;
	struct list_head task_list;
	unsigned long flag = 0;
	struct lbz_io_task *pos, *n;
	int ret = 0;

	INIT_LIST_HEAD(&task_list);
	spin_lock_irqsave(&iosched->gc_write_lock, flag);
	list_splice_init(&iosched->lbz_gc_writes, &task_list);
	iosched->gc_write_count = 0;
	spin_unlock_irqrestore(&iosched->gc_write_lock, flag);

	list_for_each_entry_safe(pos, n, &task_list, list) {
		list_del_init(&pos->list);
		ret = __submit_gc_task(iosched, pos);
		switch(ret) {
		case 0:
			break;
		case -EAGAIN:
			__add_task_to_gc_writes(iosched, pos);
			break;
		case -ENOENT:
			/*write io will trigger __gc_task_callback.*/
			LBZDEBUG("gc skiped...");
			/*__gc_task_callback will be called by user io callback.*/
			/*__gc_task_callback(task);*/
			break;
		case -EEXIST:
			/*don't need to execute gc, just callback.*/
			LBZDEBUG("gc write encounter: %d", ret);
			__gc_task_callback(pos, LBZ_INVALID_PBID);
			lbz_dec_gc_inflight(dev);
			break;
		default:
			BUG();
		}
	}
		
}
static void retry_wk_fn(struct work_struct *work)
{
	struct lbz_io_scheduler *iosched = container_of(to_delayed_work(work), struct lbz_io_scheduler, retry_wk);
	struct lbz_device *dev = iosched->host;

	if (is_dev_faulty(dev) || !is_dev_ready(dev))
		return;

	__retry_submit_io_task(iosched);
	__retry_submit_gc_task(iosched);
	yield();
}

static void trigger_retry_work(struct lbz_io_scheduler *iosched, unsigned long delay)
{
	queue_delayed_work(iosched->retry_wq, &iosched->retry_wk, delay);
}

static void __add_task_to_retry(struct lbz_io_scheduler *iosched, struct lbz_io_task *task)
{
	unsigned long flag = 0;

	spin_lock_irqsave(&iosched->pending_lock, flag);
	iosched->pending_count++;
	list_add_tail(&task->list, &iosched->lbz_user_pending);
	spin_unlock_irqrestore(&iosched->pending_lock, flag);
}

static void __add_task_to_gc_writes(struct lbz_io_scheduler *iosched, struct lbz_io_task *task)
{
	unsigned long flag = 0;

	spin_lock_irqsave(&iosched->gc_write_lock, flag);
	iosched->gc_write_count++;
	list_add_tail(&task->list, &iosched->lbz_gc_writes);
	spin_unlock_irqrestore(&iosched->gc_write_lock, flag);
}

int lbz_submit_io_to_iosched(struct lbz_io_scheduler *iosched, struct bio *bio)
{
	struct lbz_device *dev = iosched->host;
	struct lbz_io_task *task;
	enum lbz_task_type type = LBZ_TASK_USER_WRITE;
	enum lbz_task_status status;
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
	unsigned int blkid = sector_to_blkid(bio->bi_iter.bi_sector);
#endif
	int ret = 0;

	if (bio_data_dir(bio) == WRITE) {
		atomic64_inc(&dev->user_write_blocks);
		atomic64_inc(&dev->user_write_inflight_io_cnt);
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		if (lbz_nat_sit_is_cp_block(dev->nat_sit_mgmt, blkid)) {
			if (op_is_flush(bio->bi_opf)) {
				type = LBZ_TASK_WRITE_TX_FATHER;
				lbz_nat_sit_add_write(dev->nat_sit_mgmt);
			}
			lbz_nat_sit_cp_write(dev->nat_sit_mgmt);
		} else if (lbz_nat_sit_is_dup_block(dev->nat_sit_mgmt, blkid)) {
			type = LBZ_TASK_WRITE_TX_CHILD;
			lbz_nat_sit_add_write(dev->nat_sit_mgmt);
			lbz_nat_sit_user_write(dev->nat_sit_mgmt);
		} else {
			lbz_nat_sit_ssa_write(dev->nat_sit_mgmt);
		}
#endif
		task = task_alloc(type, GFP_NOIO);
		task->bio = bio;
		/*
		* bio->bi_opf &= ~REQ_PREFLUSH;
		* bio->bi_opf &= ~REQ_FUA;
		* bio->bi_opf &= ~REQ_SYNC;
		*/
		ret = __submit_write_task(iosched, task);
		switch(ret) {
		case 0:
			break;
		case -EAGAIN:
			atomic64_inc(&dev->user_encounter_emergency);
			__add_task_to_retry(iosched, task);
			trigger_retry_work(iosched, LBZ_IO_WRITE_DELAY);
			break;
		default:
			status = task->status;
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			/*task will be destroy by lbz_write_io_endio when task->status > LBZ_TASK_ALLOC_RES.*/
			if (task->status == LBZ_TASK_ALLOC_RES) {
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
				if (task->type != LBZ_TASK_USER_WRITE)
					lbz_nat_sit_complete_write(dev->nat_sit_mgmt);
#endif
				task_put(task); /*__task_init*/
				atomic64_dec(&dev->user_write_inflight_io_cnt);
			}
			atomic64_inc(&dev->user_write_err_cnt);
			break;
		}
	} else {
		atomic64_inc(&dev->user_read_blocks);
		__submit_read_io(iosched, bio);
	}
	return 0;
}

/*
 * gc task callback handler.
 * consider task error for gc context, focus on dev faulty for pending_gc_node context.
 * task->error:
 *	-ENOENT: write IO triggered
 *	-EEXIST: write already completed.
 */
static void __gc_task_callback(struct lbz_io_task *task, unsigned int pbid)
{
	struct lbz_io_scheduler *iosched = task->iosched;
	struct lbz_device *dev = iosched->host;
	unsigned int old_pbid = LBZ_INVALID_PBID;

	/*any unexpected error of task will set dev as faulty.*/
	if (!is_dev_faulty(dev) && !task->error) {
		lbz_zone_update_reverse_map(dev->zone_metadata, task->zone, pbid, task->blkid);
		old_pbid = lbz_mapping_add(dev->mapping, task->blkid, pbid);
		if (old_pbid != task->pbid)
			BUG_ON(old_pbid != LBZ_INVALID_PBID);
		/*discarded and freed.*/
		if (old_pbid != LBZ_INVALID_PBID) {
			lbz_zone_update_reverse_map(dev->zone_metadata, task->read_zone, task->pbid, LBZ_INVALID_PBID);
			lbz_zone_release_global_res(dev->zone_metadata, task->read_zone);
		}
	}
	/*task->error == -EEXIST or task->error == -ENOENT don't need to do anything.*/
	switch (task->status) {
	case LBZ_TASK_DISPATCH:
	case LBZ_TASK_GC_WRITING:
		lbz_put_zone(task->zone); /*lbz_zone_alloc_res*/
	case LBZ_TASK_ALLOC_RES:
		del_task_from_tree(iosched, task);
		task_put(task); /*added: insert_task_to_tree*/
	case LBZ_TASK_GC_READING:
		__free_page(task->page);
	case LBZ_TASK_INIT:
		lbz_put_zone(task->read_zone); /*added by __gc_one_zone.*/
		task_put(task); /*init 1: __task_init*/
		break;
	default:
		BUG();
	}
}

static void lbz_gc_read_endio(struct bio *bio)
{
	struct lbz_io_task *task = bio->bi_private;
	struct lbz_io_scheduler *iosched = task->iosched;
	struct lbz_device *dev = iosched->host;
	int errno = blk_status_to_errno(bio->bi_status);

	if (errno == 0) {
		/*before task->read_zone are valid.*/
		INIT_LIST_HEAD(&task->list);
		__add_task_to_gc_writes(iosched, task);
		trigger_retry_work(iosched, LBZ_GC_WRITE_DELAY);
		BUG_ON(1 != atomic_read(&bio->__bi_cnt));
	} else {
		task->error = errno;
		__gc_task_callback(task, LBZ_INVALID_PBID);
		lbz_inc_gc_read_err(dev);
		lbz_dec_gc_inflight(dev);
		LBZERR("gc write IO encounter error: %d", errno);
		lbz_dev_set_faulty(dev);
	}
	bio_put(bio);

}

struct bio *__init_gc_read_block(struct lbz_io_task *task, struct lbz_device *dev)
{
	struct bio *bio = NULL;
	int ret = 0;
	struct page *page = NULL;

	bio = bio_alloc(GFP_NOIO, 1);

	bio->bi_private = task;
	bio_set_dev(bio, dev->phy_bdev);
	bio->bi_iter.bi_sector = blkid_to_sector(task->pbid); /*read pbid.*/
	bio->bi_opf |= REQ_OP_READ;
	bio->bi_end_io = lbz_gc_read_endio;

	page = alloc_pages(GFP_NOIO, 0);
	task->page = page;
	ret = bio_add_page(bio, page, PAGE_SIZE, 0);
	BUG_ON(ret < PAGE_SIZE);
	BUG_ON(bio->bi_vcnt != 1);

	return bio;
}

static void lbz_gc_write_endio(struct bio *bio)
{
	struct lbz_io_task *task = bio->bi_private;
	sector_t ret_sec = bio->bi_iter.bi_sector;
	unsigned int pbid = sector_to_blkid(ret_sec);
	int errno = blk_status_to_errno(bio->bi_status);
	struct lbz_io_scheduler *iosched = task->iosched;
	struct lbz_device *dev = iosched->host;

	task->error = errno;
	lbz_zone_complete_write(task->zone);
	__gc_task_callback(task, pbid);
	bio_put(bio);
	if (errno < 0) {
		lbz_inc_gc_write_err(dev);
		LBZERR("gc write IO encounter error: %d", errno);
		lbz_dev_set_faulty(dev);
	}
	lbz_dec_gc_inflight(dev);
}

struct bio *__init_gc_write_block(struct lbz_io_task *task, struct lbz_device *dev)
{
	struct bio *bio = NULL;
	int ret = 0;

	bio = bio_alloc(GFP_NOIO, 1);

	bio->bi_private = task;
	bio_set_dev(bio, dev->phy_bdev);
	bio->bi_iter.bi_sector = task->zone->start_sector; /*must be assigned because of bio_add_page.*/
	bio->bi_opf |= REQ_OP_WRITE;
	bio->bi_end_io = lbz_gc_write_endio;

	ret = bio_add_page(bio, task->page, PAGE_SIZE, 0);
	BUG_ON(bio->bi_vcnt != 1);

	return bio;
}

static int __submit_gc_task(struct lbz_io_scheduler *iosched, struct lbz_io_task *task)
{
	struct lbz_device *dev = iosched->host;
	struct lbz_io_task *tk = NULL;
	struct lbz_zone *zone;
	int ret = 0, stream_id = 0;
	unsigned int origin_pbid = 0;
	struct bio *read_bio = NULL, *write_bio = NULL;

	switch (task->status) {
	case LBZ_TASK_INIT:
		read_bio = __init_gc_read_block(task, dev);
		task->status = LBZ_TASK_GC_READING;
		task->iosched = iosched;
		task->bio = read_bio;
		atomic64_inc(&dev->gc_read_blocks);
		submit_bio(read_bio);
		goto read_out;
	case LBZ_TASK_GC_READING:
		/*save read zone and put it after write, and task->list will not be used.*/
		task->read_zone = task->zone; /*read_zone overwritten by retry submit.*/
		task->zone = NULL;
		task->bio = NULL; /*bio_put by lbz_gc_read_endio.*/
		tk = insert_task_to_tree(iosched, task);
		if (tk != NULL) {
			LBZDEBUG("write IO conflict with task : %d", tk->type);
			atomic64_inc(&dev->gc_write_agency_blocks);
			task->error = ret = -ENOENT;
			tk->pending_gc_node = task;
			smp_mb__before_atomic();
			task_put(tk);
			smp_mb__after_atomic();
			goto pending_out;
		}
		ret = lbz_mapping_lookup(dev->mapping, &zone, &origin_pbid, task->blkid);
		/*support internal discard operation.*/
		if (ret == -ENOENT) {
			atomic64_inc(&dev->gc_write_discarded_blocks);
			task->error = ret = -EEXIST; /*already discarded.*/
			LBZDEBUG("(%s) blkid: %u, pbid: %u already discarded don't need to gc",
					dev->devname, task->blkid, task->pbid);
			del_task_from_tree(iosched, task);
			task_put(task); /*insert_task_to_tree exec task_get.*/
			goto discarded;
		}
		lbz_put_zone(zone); /*lookup will inc zone, but it's not necessary for gc.*/
		if (origin_pbid != task->pbid) {
			atomic64_inc(&dev->gc_write_complete_blocks);
			task->error = ret = -EEXIST; /*already rewrite.*/
			LBZDEBUG("(%s) blkid: %u, pbid: %u, origin_pbid: %u don't need to gc",
					dev->devname, task->blkid, task->pbid, origin_pbid);
			del_task_from_tree(iosched, task);
			task_put(task); /*insert_task_to_tree exec task_get.*/
			goto skip_gc;
		}
		task->status = LBZ_TASK_ALLOC_RES;
	case LBZ_TASK_ALLOC_RES:
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		stream_id = lbz_nat_sit_get_stream_id(dev->nat_sit_mgmt, task->blkid);
		ret = lbz_zone_alloc_res(dev->zone_metadata, &zone, LBZ_ALLOC_FLAG_GC, stream_id);
#ifdef CONFIG_LBZ_NAT_SIT_STREAM_SUPPORT
		if (ret < 0) {
			int i = 0;

			LBZDEBUG("alloc res encounter: %d, stream_id: %d", ret, stream_id);
			for (i = 0; i < LBZ_ZONE_MAX_STREAM; i++) {
				if (i != stream_id) {
					ret = lbz_zone_alloc_res(dev->zone_metadata, &zone, LBZ_ALLOC_FLAG_GC, i);
					if (0 == ret) {
						atomic64_inc(&dev->gc_alloc_encounter_eagain);
						break;
					}
				}
			}
		}
#endif
#else
		ret = lbz_zone_alloc_res(dev->zone_metadata, &zone, LBZ_ALLOC_FLAG_GC, stream_id);
#endif
		if (ret < 0) {
			BUG_ON(ret == -EAGAIN); /*gc write may encounter -EAGAIN for more stream.*/
			task->error = ret;
			LBZDEBUG("alloc res encounter: %d", ret);
			goto alloc_err;
		}
		task->zone = zone; /*before: read zone, after: zone becomes write zone.*/
		write_bio = __init_gc_write_block(task, dev);
		write_bio->bi_opf |= REQ_OP_ZONE_APPEND;
		task->bio = write_bio;
		task->status = LBZ_TASK_GC_WRITING;
	case LBZ_TASK_GC_WRITING:
		task->integrity_buf = __add_integrity(task, dev, LBZ_LOG_GC_WRITE);
		if (IS_ERR(task->integrity_buf)) {
			ret = PTR_ERR(task->integrity_buf);
			lbz_dev_set_faulty(dev);
			bio_put(write_bio);
			task->error = ret;
			BUG_ON(ret == -EAGAIN); /*bi_sector was modified.*/
			LBZERR("add integrity info encounter: %d", ret);
			goto add_meta_err;
		}
		task->status = LBZ_TASK_DISPATCH;
		break;
	default:
		BUG();
	}
	atomic64_inc(&dev->gc_write_blocks);
	submit_bio(write_bio);
	return 0;
add_meta_err:
	lbz_zone_complete_write(task->zone);
	bio_put(write_bio);
alloc_err:
skip_gc:
discarded:
pending_out:
read_out:
	return ret;
}

int lbz_submit_gc_to_iosched(struct lbz_io_scheduler *iosched, struct lbz_zone *zone, unsigned int pbid, unsigned int blkid)
{
	struct lbz_io_task *task;
	int ret = 0;

	task = task_alloc(LBZ_TASK_GC, GFP_NOIO);
	task->pbid = pbid;
	/*save read zone in case that read error, and easy for error handle of __gc_task_callback.*/
	task->read_zone = task->zone = zone;
	task->blkid = blkid;
	ret = __submit_gc_task(iosched, task);
	BUG_ON(ret != 0);

	return 0;
}

void lbz_iosched_proc_read(struct lbz_io_scheduler *iosched, struct seq_file *seq)
{
	seq_printf(seq, "pending_count: %d\n"
					"gc_write_count: %d\n"
					"task_count: %lld\n",
					iosched->pending_count,
					iosched->gc_write_count,
					atomic64_read(&iosched->task_count));
}

static void __retry_timer_fn(struct timer_list *timer)
{
	struct lbz_io_scheduler *iosched = container_of(timer, struct lbz_io_scheduler, retry_timer);

	queue_delayed_work(iosched->retry_wq, &iosched->retry_wk, 0);
	mod_timer(&iosched->retry_timer, jiffies + iosched->retry_expire);
}

int lbz_iosched_init(struct lbz_io_scheduler *iosched, struct lbz_device *dev)
{
	iosched->task_tree.rb_node = NULL;
	rwlock_init(&iosched->task_lock);
	atomic64_set(&iosched->task_count, 0);
	spin_lock_init(&iosched->pending_lock);
	iosched->pending_count = 0;
	INIT_LIST_HEAD(&iosched->lbz_user_pending);

	spin_lock_init(&iosched->gc_write_lock);
	iosched->gc_write_count = 0;
	INIT_LIST_HEAD(&iosched->lbz_gc_writes);
	iosched->host = dev;

	snprintf(iosched->wq_name, LBZ_MAX_NAME_LEN, "%s_retry", dev->devname);
	iosched->retry_wq = create_singlethread_workqueue(iosched->wq_name);
	if (IS_ERR(iosched->retry_wq)) {
		LBZERR("alloc workqueue [%s] error:%ld", iosched->wq_name, PTR_ERR(iosched->retry_wq));
		return PTR_ERR(iosched->retry_wq);
	}
	INIT_DELAYED_WORK(&iosched->retry_wk, retry_wk_fn);
	timer_setup(&iosched->retry_timer, __retry_timer_fn, 0);
	iosched->retry_expire = HZ;
	iosched->retry_timer.expires = jiffies + iosched->retry_expire;
	add_timer(&iosched->retry_timer);

	return 0;
}

void lbz_iosched_destory(struct lbz_io_scheduler *iosched)
{
	del_timer_sync(&iosched->retry_timer);
	destroy_workqueue(iosched->retry_wq);
}
