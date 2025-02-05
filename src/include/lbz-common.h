#ifndef _LBZ_COMMON_H_
#define _LBZ_COMMON_H_
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/blk_types.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/stacktrace.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/llist.h>
#include <linux/nvme.h>
#include <linux/nvme_ioctl.h>
#include <linux/blk-mq.h>
#include <linux/sched/sysctl.h>

#define LBZ_NAME "lbz"
#define LBZ_FMT LBZ_NAME ": " LBZ_MSG_PREFIX  ": "
#ifdef CONFIG_LBZ_DEBUG
#define lbz_pr(pr_func, fmt, ...)\
do {\
	pr_func(LBZ_FMT "%d(%s)[%s,%s:%d] " fmt, current->pid, current->comm,\
			(strrchr(__FILE__, '/') + 1), __FUNCTION__, __LINE__, ##__VA_ARGS__);\
} while(0)
#else
#define lbz_pr(pr_func, fmt, ...)\
do {\
	pr_func(LBZ_FMT "%d(%s)[%s,%s:%d] " fmt, current->pid, current->comm,\
			(strrchr(__FILE__, '/') + 1), __FUNCTION__, __LINE__, ##__VA_ARGS__);\
} while(0)
#endif

#define LBZ_RATELIMIT(pr_func, fmt, ...)\
do {\
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);\
\
	if (__ratelimit(&rs))\
		lbz_pr(pr_func, fmt, ##__VA_ARGS__);\
} while (0)

#define LBZCRIT(fmt, ...) pr_crit(fmt, ##__VA_ARGS__)

#define LBZALERT(fmt, ...) lbz_pr(pr_alert, fmt, ##__VA_ARGS__)
#define LBZERR(fmt, ...) lbz_pr(pr_err, fmt, ##__VA_ARGS__)
#define LBZERR_LIMIT(fmt, ...) LBZ_RATELIMIT(pr_err, fmt, ##__VA_ARGS__)
#define LBZWARN(fmt, ...) lbz_pr(pr_warn, fmt, ##__VA_ARGS__)
#define LBZWARN_LIMIT(fmt, ...) LBZ_RATELIMIT(pr_warn, fmt, ##__VA_ARGS__)
#define LBZINFO(fmt, ...) lbz_pr(pr_info, fmt, ##__VA_ARGS__)
#define LBZINFO_LIMIT(fmt, ...) LBZ_RATELIMIT(pr_info, fmt, ##__VA_ARGS__)

#ifdef CONFIG_LBZ_DEBUG
#define LBZDEBUG(fmt, ...) lbz_pr(pr_alert, fmt, ##__VA_ARGS__)
#define LBZDEBUG_LIMIT(fmt, ...) LBZ_RATELIMIT(pr_debug, fmt, ##__VA_ARGS__)
#else
#define LBZDEBUG(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#define LBZDEBUG_LIMIT(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

/*Global config.*/
#define LBZ_DATA_BLK_SHIFT 12
#define LBZ_DATA_BLK_SIZE (1 << LBZ_DATA_BLK_SHIFT)
#define LBZ_MAX_DEV_SIZE 64 /*GiB*/
#define LBZ_MAX_NAME_LEN (BDEVNAME_SIZE * 2)

/*memory related definitions.*/
extern atomic64_t lbz_mem_bytes;
#define LBZ_CLEAR_MEM(x, y) memset(x, 0x99, y)
#define	LBZ_ALLOC_MEM(x, y, z) do { \
	x = kzalloc((y), (z)); 	\
	if (x) 			\
	atomic64_add(y, &lbz_mem_bytes); 	\
} while (0)
#define	LBZ_FREE_MEM(x, y) do { \
	atomic64_sub(y, &lbz_mem_bytes); 	\
	LBZ_CLEAR_MEM(x, y);	\
	kfree(x); 		\
} while (0)
/*-------------------------------*/
static inline unsigned int sector_to_blkid(sector_t sector)
{
	return sector >> (LBZ_DATA_BLK_SHIFT - SECTOR_SHIFT);
}

static inline sector_t blkid_to_sector(unsigned int blkid)
{
	return blkid << (LBZ_DATA_BLK_SHIFT - SECTOR_SHIFT);
}
#endif
