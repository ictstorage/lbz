#ifndef _LBZ_MAPPING_H_
#define _LBZ_MAPPING_H_
#include "lbz-common.h"

struct leaf_node_headr {
	rwlock_t lock;
	unsigned int blkid;
};

struct lbz_zone;
struct lbz_io_scheduler;
struct lbz_device;

struct mapping_leaf_node {
	struct leaf_node_headr header;
	unsigned int pbids[];
};

struct internal_node_headr {
	unsigned int entries;
	unsigned int blkid;
};

struct mapping_internal_node {
	struct internal_node_headr internal_header;
	
	struct mapping_leaf_node *childern[];
};

#define LBZ_MAPPING_BLK_SHIFT (12)
#define LBZ_MAPPING_BLK_SIZE (1 << (LBZ_MAPPING_BLK_SHIFT))

#define LBZ_LEAF_NODE_ENTIRES ((LBZ_MAPPING_BLK_SIZE - sizeof(struct leaf_node_headr)) / sizeof(unsigned int))

#define LBZ_INT_NODE_ENTRIES ((LBZ_MAPPING_BLK_SIZE - sizeof(struct internal_node_headr)) / sizeof(struct mapping_leaf_node *))
#define LBZ_INT_NODE_INDEXED_BLKS (LBZ_LEAF_NODE_ENTIRES * LBZ_INT_NODE_ENTRIES)

#define MAX_INTERNAL_NODES (((LBZ_MAX_DEV_SIZE << (30 - LBZ_DATA_BLK_SHIFT)) + LBZ_INT_NODE_INDEXED_BLKS - 1)\
	   	/ LBZ_INT_NODE_INDEXED_BLKS)

struct lbz_mapping {
	/*three level tree.*/
	struct mapping_internal_node *root[MAX_INTERNAL_NODES];
	/*Indicate the size of block device.*/
	unsigned int max_blkid;
	unsigned int superblock_pbid; 

	void *host; /*struct lbz_device.*/
};

#define UINT_MAX (~0U)
#define LBZ_INVALID_PBID UINT_MAX /*stand for unmapped mapping.*/

int lbz_mapping_lookup(struct lbz_mapping *mapping, struct lbz_zone **ret_zone, unsigned int *ret_pbid, unsigned int blkid);
unsigned int lbz_mapping_add(struct lbz_mapping *mapping, unsigned int blkid, unsigned int pbid);
unsigned int lbz_mapping_remove(struct lbz_mapping *mapping, unsigned int blkid);
void lbz_mapping_init(struct lbz_mapping *mapping, struct lbz_device *dev);
void lbz_mapping_destroy(struct lbz_mapping *mapping);
#endif
