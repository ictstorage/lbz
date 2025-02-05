#ifndef _LBZ_METADATA_H_
#define _LBZ_METADATA_H_
#include "lbz-common.h"
struct lbz_zone_metadata zone_metadata;
struct lbz_mapping mapping;
struct lbz_reverse_mapping reverse_mapping;

struct lbz_metadata {
	struct lbz_zone_metadata zone_metadata;
	struct lbz_mapping mapping;
	struct lbz_reverse_mapping reverse_mapping;
};
#endif
