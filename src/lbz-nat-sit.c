#include "lbz-dev.h"
#include "lbz-io-scheduler.h"
#include "lbz-zone-metadata.h"
#include "lbz-mapping.h"
#include "lbz-gc.h"
#include "lbz-request.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-nat-sit"

void lbz_nat_sit_wait_completed(struct lbz_nat_sit_mgmt *mgmt)
{
	wait_queue_head_t wq;

	init_waitqueue_head(&wq);
	/*wait nat and sit io complete.*/
	do {
		wait_event_timeout(wq, atomic64_read(&mgmt->nat_sit_inflight_writes) == 0, HZ);
	} while (atomic64_read(&mgmt->nat_sit_inflight_writes) > 0);
}

void lbz_nat_sit_add_blkid(struct lbz_nat_sit_mgmt *mgmt, unsigned int blkid)
{
	unsigned long flag = 0;
	struct nat_sit_node *new;

retry:
	spin_lock_irqsave(&mgmt->lock, flag);
	if (llist_empty(&mgmt->head) || mgmt->cur_node->header.cursor == NAT_SIT_NODE_ENTRIES) {
		new = (struct nat_sit_node *)__get_free_page(GFP_ATOMIC);
		if (!new) {
			spin_unlock_irqrestore(&mgmt->lock, flag);
			cond_resched();
			goto retry;
		}
		new->header.next.next = NULL;
		new->header.cursor = 0;
		if (llist_empty(&mgmt->head)) {
			mgmt->head.first = &new->header.next;
		} else {
			mgmt->cur_node->header.next.next = &new->header.next;
		}
		mgmt->cur_node = new;
	}
	mgmt->cur_node->to_free_blkids[mgmt->cur_node->header.cursor++] = blkid;
	mgmt->to_free_count++;
	spin_unlock_irqrestore(&mgmt->lock, flag);
}

void lbz_nat_sit_tx_start(struct lbz_nat_sit_mgmt *mgmt)
{
}

void lbz_nat_sit_tx_complete(struct lbz_nat_sit_mgmt *mgmt)
{
	struct lbz_device *dev = mgmt->host;
	struct nat_sit_node *node = (struct nat_sit_node *)mgmt->head.first, *tmp;
	unsigned int old_pbid;
	int i = 0, count = 0;

	while (node != NULL) {
		count = node->header.cursor;
		for (i = 0; i < count; i++) {
			old_pbid = lbz_mapping_remove(dev->mapping, node->to_free_blkids[i]);
			if (old_pbid != LBZ_INVALID_PBID) {
				struct lbz_zone *old_zone = get_zone_by_pbid(dev->zone_metadata, old_pbid);

				lbz_zone_update_reverse_map(dev->zone_metadata,
						old_zone, old_pbid, LBZ_INVALID_PBID);
				lbz_zone_release_global_res(dev->zone_metadata, old_zone);
				mgmt->total_freed_blks++;
			}
		}
		tmp = (struct nat_sit_node *)node->header.next.next;
		free_page((unsigned long)node);
		node = tmp;
	}
	init_llist_head(&mgmt->head);
	mgmt->cur_node = NULL;
	mgmt->to_free_count = 0;
}

/*init by invalid args 0 for default, nat and sit will do nothing.*/
bool lbz_nat_sit_add_rule(struct lbz_nat_sit_mgmt *mgmt, void *args, void *host)
{
	memcpy(mgmt, args, sizeof(struct nat_sit_args));
	mgmt->cur_cp_pack = 0; /*1 or 2 in runtime.*/
	mgmt->blocks_per_segment = 512; /*4k * 512 = 2M for default.*/
	atomic64_set(&mgmt->nat_sit_inflight_writes, 0);
	atomic64_set(&mgmt->nat_sit_cp_write, 0);
	atomic64_set(&mgmt->nat_sit_ssa_write, 0);
	atomic64_set(&mgmt->nat_sit_user_write, 0);

	mgmt->cp_blocks = mgmt->sit_blkaddr - mgmt->cp_blkaddr;
	mgmt->sit_blocks = mgmt->nat_blkaddr - mgmt->sit_blkaddr;

	mgmt->first_cp_blkaddr = mgmt->cp_blkaddr; 
	mgmt->cp_length = mgmt->cp_blocks / 2;
	mgmt->second_cp_blkaddr = mgmt->cp_blkaddr + mgmt->cp_length; 

	BUG_ON(mgmt->sit_blkaddr % mgmt->blocks_per_segment != 0);
	mgmt->sit_segment_no = mgmt->sit_blkaddr / mgmt->blocks_per_segment;
	BUG_ON(mgmt->nat_blkaddr % mgmt->blocks_per_segment != 0);
	mgmt->nat_segment_no = mgmt->nat_blkaddr / mgmt->blocks_per_segment;

	init_llist_head(&mgmt->head);
	mgmt->cur_node = NULL;
	mgmt->to_free_count = 0;
	spin_lock_init(&mgmt->lock);
	mgmt->total_freed_blks = 0;

	mgmt->host = host;

	return true;
}

void lbz_nat_sit_destroy(struct lbz_nat_sit_mgmt *mgmt)
{
	struct nat_sit_node *node = (struct nat_sit_node *)mgmt->head.first, *tmp;

	while (node != NULL) {
		tmp = (struct nat_sit_node *)node->header.next.next;
		free_page((unsigned long)node);
		node = tmp;
	}
	init_llist_head(&mgmt->head);
	mgmt->cur_node = NULL;
	mgmt->to_free_count = 0;
}

void lbz_nat_sit_proc_read(struct lbz_nat_sit_mgmt *mgmt, struct seq_file *seq)
{
	seq_printf(seq, "cp_blkaddr: %u\n"
					"sit_blkaddr: %u\n"
					"nat_blkaddr: %u\n"
					"nat_blocks: %u\n"
					"cp_blocks: %u\n"
					"sit_blocks: %u\n"
					"first_cp_blkaddr: %u\n"
					"second_cp_blkaddr: %u\n"
					"cp_length: %u\n"
					"sit_segment_no: %u\n"
					"nat_segment_no: %u\n"
					"cur_cp_pack: %u\n"
					"blocks_per_segment: %u\n"
					"nat_sit_inflight_writes: %lld\n"
					"nat_sit_cp_write: %lld\n"
					"nat_sit_ssa_write: %lld\n"
					"nat_sit_user_write: %lld\n"
					"to_free_count: %u\n"
					"total_freed_blks: %lu\n",
					mgmt->cp_blkaddr,
					mgmt->sit_blkaddr,
					mgmt->nat_blkaddr,
					mgmt->nat_blocks,
					mgmt->cp_blocks,
					mgmt->sit_blocks,
					mgmt->first_cp_blkaddr,
					mgmt->second_cp_blkaddr,
					mgmt->cp_length,
					mgmt->sit_segment_no,
					mgmt->nat_segment_no,
					mgmt->cur_cp_pack,
					mgmt->blocks_per_segment,
					atomic64_read(&mgmt->nat_sit_inflight_writes),
					atomic64_read(&mgmt->nat_sit_cp_write),
					atomic64_read(&mgmt->nat_sit_ssa_write),
					atomic64_read(&mgmt->nat_sit_user_write),
					mgmt->to_free_count,
					mgmt->total_freed_blks);
}
