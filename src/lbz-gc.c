#include "lbz-gc.h"
#include "lbz-io-scheduler.h"
#include "lbz-zone-metadata.h"
#include "lbz-dev.h"
#include "lbz-mapping.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-gc"

#define LBZ_GC_DAEMON_SCHEDULE_DELAY (30 * HZ)
#define LBZ_GC_RECLAIM_MIN_INTERVAL (60 * HZ)

void lbz_complete_one_zone(struct lbz_gc_context *gc_ctx)
{
	clear_bit(LBZ_GC_STAT_RECLAIMING, &gc_ctx->gc_state);
	smp_mb__after_atomic();
	wake_up_process(gc_ctx->gc_thread);
}

/*
 * valid block maybe rewrite before submit gc write, so gc write will execute as follows:
 * 1. read.
 * 2. add task to tree, and check pbid.
 * 3. dispatch write.
 * */
int __gc_one_zone(struct lbz_zone *zone, struct lbz_device *dev)
{
	unsigned int iter = 0, iter1 = 0, iter2 = 0, pos = 0, pbid = sector_to_blkid(zone->start_sector);
	int ret = 0;

	/*don't need to get zone, lbz_find_victim_zone have already get it.*/
	/*lbz_get_zone(zone);*/
	LBZ_REVERSE_MAP_FOR_EACH_ENTRY(pos, dev->zone_metadata, zone, iter, iter1, iter2) {
		if (pos == LBZ_INVALID_PBID) {
			pbid++;
			continue;
		}
		lbz_get_zone(zone);
		lbz_inc_gc_inflight(dev);
		ret = lbz_submit_gc_to_iosched(dev->iosched, zone, pbid, pos);
		BUG_ON(ret != 0);
		/* if (ret < 0) {
		 * 	LBZERR("blkid: %u, pbid: %u, gc encounter error: %d", pos, pbid, ret);
		 * 	if (ret != -EEXIST)
		 * 		break;
		 * }
		 */
		pbid++;
	}
	atomic64_add(zone->wp_block, &dev->gc_reset_blocks);
	lbz_set_zone_state(LBZ_ZONE_GC_COMPLETED, zone);
	lbz_put_zone(zone);
	return ret;
}

static int lbz_gc_thread(void *data)
{
	int ret = 0;
	unsigned long last_jiffies = jiffies;
	struct lbz_gc_context *gc_ctx = (struct lbz_gc_context *)data;
	struct lbz_device *dev = (struct lbz_device *)gc_ctx->host;
	struct lbz_zone_metadata *zmd = dev->zone_metadata;
	struct lbz_zone *zone = NULL;
	enum lbz_victim_mod mod;

	while (!kthread_should_stop()) {
try_reclaim:
		if (is_dev_faulty(dev) || !is_dev_ready(dev))
			goto sleep;

		mod = LBZ_VICTIM_DEFAULT;
		/*only reclaim few valid blocks for regular mode.*/
		if ((jiffies - gc_ctx->last_jiffies) > LBZ_GC_RECLAIM_MIN_INTERVAL ||
				test_bit(LBZ_GC_STAT_RECLAIMING, &gc_ctx->gc_state))
			mod = LBZ_VICTIM_REGULAR;
		if (lbz_check_need_reclaim_low(zmd) ||
				test_bit(LBZ_GC_STAT_EMERGENCY, &gc_ctx->gc_state))
			mod = LBZ_VICTIM_EMERGENCY;
		if (mod == LBZ_VICTIM_DEFAULT)
			goto sleep;

		if (is_dev_faulty(dev) || test_bit(LBZ_GC_STAT_EXIT, &gc_ctx->gc_state))
			goto check_exited;

		if (gc_ctx->gc_zone != NULL && is_lbz_zone_state(LBZ_ZONE_GC, gc_ctx->gc_zone))
			goto sleep;

		zone = lbz_find_victim_zone(dev->zone_metadata, mod);
		if (!zone) {
			LBZDEBUG_LIMIT("(%s) find victim zone encounter %d", dev->devname, -ENOENT);
			goto sleep;
		}

#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		LBZINFO("zone(%d) will gc %u blocks, stream: %d, time: %u, awrites0: %lld, "
				"awrites1: %lld, bssawrite:%lld, ssawrite: %lld, userwrites: %lld",
				zone->id, atomic_read(&zone->weight), zone->stream, jiffies_to_msecs(jiffies),
				atomic64_read(&dev->zone_metadata->active_zone_writes[0]),
				atomic64_read(&dev->zone_metadata->active_zone_writes[1]),
				atomic64_read(&dev->nat_sit_mgmt->nat_sit_cp_write) + atomic64_read(&dev->nat_sit_mgmt->nat_sit_user_write),
				atomic64_read(&dev->nat_sit_mgmt->nat_sit_ssa_write),
				atomic64_read(&dev->user_write_blocks));
#else
		LBZINFO("zone(%d) will gc %u blocks, stream: %d time: %u, active_zone_writes0: %lld, active_zone_writes1: %lld, "
				"userwrites: %lld",
				zone->id, atomic_read(&zone->weight), zone->stream, jiffies_to_msecs(jiffies),
				atomic64_read(&dev->zone_metadata->active_zone_writes[0]),
				atomic64_read(&dev->zone_metadata->active_zone_writes[1]),
				atomic64_read(&dev->user_write_blocks));
#endif
		gc_ctx->gc_zone = zone;
		ret = __gc_one_zone(zone, dev);
		if (ret < 0) {
			set_bit(LBZ_GC_STAT_FAULTY, &gc_ctx->gc_state);
			LBZERR("(%s) gc zone: %lx encounter %d", dev->devname, (unsigned long)zone, ret);
			goto sleep;
		}
		gc_ctx->gc_times++;
		gc_ctx->last_jiffies = last_jiffies = jiffies;
		clear_bit(LBZ_GC_STAT_EMERGENCY, &gc_ctx->gc_state);
		goto try_reclaim;

check_exited:
		if (test_bit(LBZ_GC_STAT_EXIT, &gc_ctx->gc_state)) {
			LBZINFO("%s exiting...", dev->devname);
			goto sleep;
		}
sleep:
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			break;
		}
		schedule_timeout_uninterruptible(LBZ_GC_DAEMON_SCHEDULE_DELAY);
	}
	del_timer_sync(&gc_ctx->gc_timer);
	smp_mb__before_atomic();
	clear_bit(LBZ_GC_STAT_EXIT, &gc_ctx->gc_state);
	set_bit(LBZ_GC_STAT_EXITED, &gc_ctx->gc_state);
	smp_mb__after_atomic();

	LBZINFO("(%s)gc daemon exited!", dev->devname);
	wake_up_bit(&gc_ctx->gc_state, LBZ_GC_STAT_EXIT);

	return 0;
}

static void __gc_timer_fn(struct timer_list *timer)
{
	struct lbz_gc_context *gc_ctx = container_of(timer, struct lbz_gc_context, gc_timer);

	wake_up_process(gc_ctx->gc_thread);
	mod_timer(&gc_ctx->gc_timer, jiffies + gc_ctx->gc_expire);
}

void lbz_trigger_gc_reclaim_emergency(struct lbz_gc_context *gc_ctx)
{
	set_bit(LBZ_GC_STAT_EMERGENCY, &gc_ctx->gc_state);
	wake_up_process(gc_ctx->gc_thread);
}

void lbz_trigger_gc_reclaim(struct lbz_gc_context *gc_ctx)
{
	set_bit(LBZ_GC_STAT_RECLAIMING, &gc_ctx->gc_state);
	wake_up_process(gc_ctx->gc_thread);
}

void lbz_gc_proc_read(struct lbz_gc_context *gc_ctx, struct seq_file *seq)
{
	unsigned long now = jiffies;

	seq_printf(seq, "gc_expire: %u\n"
					"gc_state: %lu\n"
					"gc_times: %lu\n"
					"last_jiffies: %lu\n"
					"jiffies(now): %lu\n"
					"duration: %lu\n",
					gc_ctx->gc_expire,
					gc_ctx->gc_state,
					gc_ctx->gc_times,
					gc_ctx->last_jiffies,
					now, now - gc_ctx->last_jiffies);

}

void lbz_destroy_gc_thread(struct lbz_gc_context *gc_ctx)
{
	set_bit(LBZ_GC_STAT_EXIT, &gc_ctx->gc_state);
	kthread_stop(gc_ctx->gc_thread);
	wake_up_process(gc_ctx->gc_thread);
	wait_on_bit_io(&gc_ctx->gc_state, LBZ_GC_STAT_EXIT, TASK_UNINTERRUPTIBLE);
}

int lbz_create_gc_thread(struct lbz_gc_context *gc_ctx, struct lbz_device *dev)
{
	int ret = 0;

	gc_ctx->gc_times = 0;
	gc_ctx->host = dev;
	gc_ctx->gc_thread = kthread_run(lbz_gc_thread, gc_ctx, "%s_gc", dev->devname);
	if (IS_ERR(gc_ctx->gc_thread)) {
		ret = PTR_ERR(gc_ctx->gc_thread);
		LBZERR("create gc daemon error:%d", ret);
		goto out;
	}
	timer_setup(&gc_ctx->gc_timer, __gc_timer_fn, 0);
	gc_ctx->gc_expire = LBZ_GC_EXPIRE;
	gc_ctx->gc_timer.expires = jiffies + gc_ctx->gc_expire;
	add_timer(&gc_ctx->gc_timer);
	gc_ctx->gc_state = (1 << LBZ_GC_STAT_READY);

out:
	return ret;
}
