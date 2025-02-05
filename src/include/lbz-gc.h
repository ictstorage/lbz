#ifndef _LBZ_GC_H_
#define _LBZ_GC_H_
#include "lbz-common.h"

#define LBZ_GC_EXPIRE (50 * HZ)

enum lbz_gc_state {
	LBZ_GC_STAT_NORMAL = 0,
	LBZ_GC_STAT_FAULTY,
	LBZ_GC_STAT_RECLAIMING,
	LBZ_GC_STAT_EMERGENCY,
	LBZ_GC_STAT_READY,
	LBZ_GC_STAT_EXIT,
	LBZ_GC_STAT_EXITED
};

struct lbz_device;
struct lbz_zone;

struct lbz_gc_context {
	struct task_struct *gc_thread;
	struct timer_list gc_timer;
	unsigned int gc_expire;
	unsigned long gc_state;
	unsigned long last_jiffies;
	unsigned long gc_times;
	struct lbz_zone *gc_zone;

	/*watermark*/

	void *host; /*struct lbz_device.*/
};
void lbz_complete_one_zone(struct lbz_gc_context *gc_ctx);
void lbz_trigger_gc_reclaim_emergency(struct lbz_gc_context *gc_ctx);
void lbz_trigger_gc_reclaim(struct lbz_gc_context *gc_ctx);
void lbz_gc_proc_read(struct lbz_gc_context *gc_ctx, struct seq_file *seq);
void lbz_destroy_gc_thread(struct lbz_gc_context *gc_ctx);
int lbz_create_gc_thread(struct lbz_gc_context *gc_ctx, struct lbz_device *dev);
#endif
