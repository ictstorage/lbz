#ifndef _LBZ_NAT_SIT_H_
#define _LBZ_NAT_SIT_H_
#include "lbz-common.h"

struct nat_sit_args {
	unsigned int cp_blkaddr;
	unsigned int sit_blkaddr;
	unsigned int nat_blkaddr;
	unsigned int nat_blocks;
};

struct nat_sit_header {
	struct llist_node next;
	int cursor;
} __packed;

#define NAT_SIT_NODE_ENTRIES ((PAGE_SIZE - sizeof(struct nat_sit_header)) / sizeof(unsigned int))
struct nat_sit_node {
	struct nat_sit_header header;
	unsigned int to_free_blkids[NAT_SIT_NODE_ENTRIES];
} __packed;

struct lbz_nat_sit_mgmt {
	/*struct nat_sit_args*/
	unsigned int cp_blkaddr;
	unsigned int sit_blkaddr;
	unsigned int nat_blkaddr;
	unsigned int nat_blocks;

	unsigned int cp_blocks;
	unsigned int sit_blocks;

	unsigned int first_cp_blkaddr;
	unsigned int second_cp_blkaddr;
	unsigned int cp_length;

	unsigned int sit_segment_no;
	unsigned int nat_segment_no;

	unsigned int cur_cp_pack;
	unsigned int blocks_per_segment;

	atomic64_t nat_sit_inflight_writes; /*both sit-nat and cp.*/
	atomic64_t nat_sit_user_write; /*both sit-nat and cp.*/
	atomic64_t nat_sit_cp_write; /*both sit-nat and cp.*/
	atomic64_t nat_sit_ssa_write; /*both sit-nat and cp.*/

	struct llist_head head;
	struct nat_sit_node *cur_node;
	unsigned int to_free_count;
	spinlock_t lock;
	unsigned long total_freed_blks;

	void *host; /*struct lbz_device*/
};

static inline void lbz_nat_sit_cp_write(struct lbz_nat_sit_mgmt *mgmt)
{
	atomic64_inc(&mgmt->nat_sit_cp_write);
}

static inline void lbz_nat_sit_ssa_write(struct lbz_nat_sit_mgmt *mgmt)
{
	atomic64_inc(&mgmt->nat_sit_ssa_write);
}

static inline void lbz_nat_sit_user_write(struct lbz_nat_sit_mgmt *mgmt)
{
	atomic64_inc(&mgmt->nat_sit_user_write);
}

static inline void lbz_nat_sit_add_write(struct lbz_nat_sit_mgmt *mgmt)
{
	atomic64_inc(&mgmt->nat_sit_inflight_writes);
}

static inline void lbz_nat_sit_complete_write(struct lbz_nat_sit_mgmt *mgmt)
{
	atomic64_dec(&mgmt->nat_sit_inflight_writes);
}

#ifdef CONFIG_LBZ_NAT_SIT_STREAM_SUPPORT
static inline int lbz_nat_sit_get_stream_id(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (mgmt->nat_blkaddr != 0 && block >= mgmt->nat_blkaddr + mgmt->nat_blocks)
		return 1;
	return 0;
}
#else
static inline int lbz_nat_sit_get_stream_id(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	return 0;
}
#endif

static inline bool lbz_nat_sit_is_dup_block(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (block >= mgmt->sit_blkaddr && block < mgmt->nat_blkaddr + mgmt->nat_blocks)
		return true;
	return false;
}

static inline bool lbz_nat_sit_is_cp_block(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (block >= mgmt->cp_blkaddr && block < mgmt->cp_blkaddr + mgmt->cp_blocks)
		return true;
	return false;
}

static inline bool lbz_nat_sit_is_fisrt_cp_block(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (block >= mgmt->first_cp_blkaddr && block < mgmt->first_cp_blkaddr + mgmt->cp_length)
		return true;
	return false;
}

static inline bool lbz_nat_sit_is_second_cp_block(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (block >= mgmt->second_cp_blkaddr && block < mgmt->second_cp_blkaddr + mgmt->cp_length)
		return true;
	return false;
}

static inline void lbz_nat_sit_set_cur_cp(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	if (lbz_nat_sit_is_fisrt_cp_block(mgmt, block))
		mgmt->cur_cp_pack = 1;
	else
		mgmt->cur_cp_pack = 2;
}

static inline unsigned int lbz_nat_sit_to_free(struct lbz_nat_sit_mgmt *mgmt, unsigned block)
{
	unsigned int segment_no = block / mgmt->blocks_per_segment;
	unsigned int to_free_seg;

	if (segment_no % 2 == 1)
		to_free_seg = segment_no - 1;
	else
		to_free_seg = segment_no + 1;

	return to_free_seg * mgmt->blocks_per_segment + (block % mgmt->blocks_per_segment);
}

void lbz_nat_sit_wait_completed(struct lbz_nat_sit_mgmt *mgmt);
void lbz_nat_sit_add_blkid(struct lbz_nat_sit_mgmt *mgmt, unsigned int blkid);
void lbz_nat_sit_tx_complete(struct lbz_nat_sit_mgmt *mgmt);
bool lbz_nat_sit_add_rule(struct lbz_nat_sit_mgmt *mgmt, void *args, void *host);
void lbz_nat_sit_destroy(struct lbz_nat_sit_mgmt *mgmt);
void lbz_nat_sit_proc_read(struct lbz_nat_sit_mgmt *mgmt, struct seq_file *seq);
#endif
