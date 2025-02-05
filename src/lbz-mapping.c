#include "lbz-mapping.h"
#include "lbz-zone-metadata.h"
#include "lbz-dev.h"

static void __find_specific_node(struct lbz_mapping *mapping, unsigned int blkid, struct mapping_leaf_node **leaf, int *index)
{
	int root_index, int_index, leaf_index;

	BUG_ON(blkid < 0 || blkid >= mapping->max_blkid);

	root_index = blkid / LBZ_INT_NODE_INDEXED_BLKS; 
	int_index = blkid / LBZ_LEAF_NODE_ENTIRES % LBZ_INT_NODE_ENTRIES;
	leaf_index = blkid % LBZ_LEAF_NODE_ENTIRES;

	*leaf = mapping->root[root_index]->childern[int_index];
	*index = leaf_index;
}

int lbz_mapping_lookup(struct lbz_mapping *mapping, struct lbz_zone **ret_zone, unsigned int *ret_pbid, unsigned int blkid)
{
	struct lbz_device *dev = mapping->host;
	struct mapping_leaf_node *leaf;
	unsigned int index, pbid;
	unsigned long flag;
	struct leaf_node_headr *header;
	struct lbz_zone *zone = NULL;

	__find_specific_node(mapping, blkid, &leaf, &index);
	header = &leaf->header;
	/*add zone reference, in case that zone becomes invalid.*/
	read_lock_irqsave(&header->lock, flag);
	pbid = leaf->pbids[index];
	if (pbid != LBZ_INVALID_PBID) {
		zone = get_zone_by_pbid(dev->zone_metadata, pbid);
		lbz_get_zone(zone);
	}
	read_unlock_irqrestore(&header->lock, flag);

	if (pbid == LBZ_INVALID_PBID)
		return -ENOENT;

	*ret_pbid = pbid;
	*ret_zone = zone;

	return 0;
}

unsigned int lbz_mapping_add(struct lbz_mapping *mapping, unsigned int blkid, unsigned int pbid)
{
	struct mapping_leaf_node *leaf;
	unsigned int index;
	unsigned long flag;
	struct leaf_node_headr *header;
	unsigned int old_pbid = LBZ_INVALID_PBID;

	__find_specific_node(mapping, blkid, &leaf, &index);
	header = &leaf->header;
	write_lock_irqsave(&header->lock, flag);
	old_pbid = leaf->pbids[index];
	leaf->pbids[index] = pbid;
	write_unlock_irqrestore(&header->lock, flag);

	return old_pbid;
}

unsigned int lbz_mapping_remove(struct lbz_mapping *mapping, unsigned int blkid)
{
	struct mapping_leaf_node *leaf;
	int index;
	unsigned long flag;
	struct leaf_node_headr *header;
	unsigned int old_pbid = LBZ_INVALID_PBID;

	__find_specific_node(mapping, blkid, &leaf, &index);
	header = &leaf->header;
	write_lock_irqsave(&header->lock, flag);
	old_pbid = leaf->pbids[index];
	leaf->pbids[index] = LBZ_INVALID_PBID;
	write_unlock_irqrestore(&header->lock, flag);

	return old_pbid;
}

static void * __alloc_mapping_node(void)
{
	void *buf;
	
retry:
	buf = (void *)__get_free_page(GFP_KERNEL);
	if (IS_ERR(buf)) {
		msleep(5);
		goto retry;
	}
	return buf;
}

static void __free_mapping_node(void *buf)
{
	free_page((unsigned long)buf);
}

static void __init_internal_node(struct mapping_internal_node *int_node, unsigned int blkid)
{
	int_node->internal_header.entries = 0;
	int_node->internal_header.blkid = blkid;
}

static void __init_leaf_node(struct mapping_leaf_node *leaf_node, unsigned int blkid)
{
	rwlock_init(&leaf_node->header.lock);
	leaf_node->header.blkid = blkid;
	memset(leaf_node->pbids, 0xff, LBZ_LEAF_NODE_ENTIRES * sizeof(unsigned int));
}

void lbz_mapping_init(struct lbz_mapping *mapping, struct lbz_device *dev)
{
	int nr_internal_node = 0, nr_leaf_node = 0, max_blkid = 0, i = 0, j = 0, count = 0, start_blkid = 0;

	max_blkid = mapping->max_blkid = dev->dev_size >> (LBZ_DATA_BLK_SHIFT - SECTOR_SHIFT);
	mapping->superblock_pbid = LBZ_INVALID_PBID;
	mapping->host = dev;

	nr_internal_node = (max_blkid + LBZ_INT_NODE_INDEXED_BLKS - 1) / LBZ_INT_NODE_INDEXED_BLKS;
	nr_leaf_node = (max_blkid + LBZ_LEAF_NODE_ENTIRES - 1) / LBZ_LEAF_NODE_ENTIRES;

	for (; i < nr_internal_node; i++) {
		mapping->root[i] = __alloc_mapping_node();
		__init_internal_node(mapping->root[i], start_blkid);
		for (j = 0; j < LBZ_INT_NODE_ENTRIES && count < nr_leaf_node; j++, count++) {
			mapping->root[i]->childern[j] = __alloc_mapping_node();
			mapping->root[i]->internal_header.entries++;
			__init_leaf_node(mapping->root[i]->childern[j], start_blkid);
			start_blkid += LBZ_LEAF_NODE_ENTIRES;
		}
	}
}

void lbz_mapping_destroy(struct lbz_mapping *mapping)
{
	int nr_internal_node = 0, nr_leaf_node = 0, max_blkid = 0, i = 0, j = 0, count = 0;

	max_blkid = mapping->max_blkid;

	nr_internal_node = (max_blkid + LBZ_INT_NODE_INDEXED_BLKS - 1) / LBZ_INT_NODE_INDEXED_BLKS;
	nr_leaf_node = (max_blkid + LBZ_LEAF_NODE_ENTIRES - 1) / LBZ_LEAF_NODE_ENTIRES;

	for (; i < nr_internal_node; i++) {
		for (j = 0; j < LBZ_INT_NODE_ENTRIES && count < nr_leaf_node; j++, count++) {
			__free_mapping_node(mapping->root[i]->childern[j]);
		}
		__free_mapping_node(mapping->root[i]);
	}
}
