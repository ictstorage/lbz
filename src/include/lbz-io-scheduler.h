#ifndef _LBZ_IO_SCHEDULER_H_
#define _LBZ_IO_SCHEDULER_H_
#include "lbz-common.h"

enum lbz_task_type {
	LBZ_TASK_USER_WRITE,
	LBZ_TASK_WRITE_TX_FATHER,
	LBZ_TASK_WRITE_TX_CHILD,
	LBZ_TASK_GC,
};

enum lbz_task_status {
	LBZ_TASK_INIT,
	LBZ_TASK_ALLOC_RES,
	LBZ_TASK_GC_READING,
	LBZ_TASK_GC_WRITING,
	LBZ_TASK_IO_WRITING,
	LBZ_TASK_DISPATCH,
	LBZ_TASK_RETRY
};

struct lbz_io_hook {
	struct lbz_device *dev;
	bio_end_io_t *user_endio;
	void *user_private;
	union {
		struct lbz_zone *zone; /*read io*/
		struct lbz_io_task *task; /*write and gc IO.*/
		void *priv; /*assgin.*/
	};
	unsigned int blkid;
};

struct lbz_io_task {
	struct rb_node node;
	union {
		struct list_head list; /*in lbz_user_pending or lbz_gc_writes list.*/
		struct lbz_zone *read_zone; /*invalid during gc reading.*/
	};
	struct bio *bio;
	unsigned int blkid;
	unsigned int pbid; /*write and gc read.*/
	struct page *page; /*gc read and write page.*/
	void *integrity_buf;
	struct lbz_zone *zone; /*target zone for write and gc read.*/
	atomic_t refcount;
	int error;
	enum lbz_task_status status;
	enum lbz_task_type type;
	union {
		struct lbz_io_task *pending_gc_node; /*may call gc handle func.*/
		struct lbz_io_scheduler *iosched; /*used for gc read callback.*/
	};
};

#define LBZ_RETRY_DELAY (HZ * 3)
//#define LBZ_IO_WRITE_DELAY (HZ / 200)
#define LBZ_IO_WRITE_DELAY (0)
#define LBZ_GC_WRITE_DELAY (HZ / 100)
struct lbz_io_scheduler {
	struct rb_root task_tree;
	rwlock_t task_lock;
	atomic64_t task_count;

	/*Only user io need to retry.*/
	spinlock_t pending_lock;
	int pending_count;
	struct list_head lbz_user_pending;

	spinlock_t gc_write_lock;
	int gc_write_count;
	struct list_head lbz_gc_writes;

	char wq_name[LBZ_MAX_NAME_LEN];
	struct workqueue_struct *retry_wq;
	struct delayed_work retry_wk;
	struct timer_list retry_timer;
	unsigned long retry_delay;
	unsigned int retry_expire;
	/* 
	 * instead of queue_delay_work
	 * struct timer_list_retry_timer;
	 */

	void *host; /*struct lbz_device*/
};

enum lbz_log_type {
	LBZ_LOG_TRANSACTION_CHILD, /*1 phase commit.*/
	LBZ_LOG_TRANSACTION_FATHER, /*2 phase commit.*/
	LBZ_LOG_SUPERBLOCK, /*record device info.*/
	LBZ_LOG_USER_WRITE,
	LBZ_LOG_GC_WRITE,
	LBZ_LOG_DISCARD_IO,/*reserved.*/
};

struct lbz_disk_log {
	long timestamp;
	unsigned long txid;
	unsigned int blkid;
	unsigned int free_blkid;
	unsigned int log_type; /*enum lbz_log_type.*/
	unsigned int crc;
	char reserved[32];
};

int lbz_submit_io_to_iosched(struct lbz_io_scheduler *iosched, struct bio *bio);
int lbz_submit_gc_to_iosched(struct lbz_io_scheduler *iosched, struct lbz_zone *zone, unsigned int pbid, unsigned int blkid);
void lbz_iosched_proc_read(struct lbz_io_scheduler *iosched, struct seq_file *seq);
int lbz_iosched_init(struct lbz_io_scheduler *iosched, struct lbz_device *dev);
void lbz_iosched_destory(struct lbz_io_scheduler *iosched);
#endif
