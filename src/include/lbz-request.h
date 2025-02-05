#ifndef _LBZ_REQUESTH_
#define _LBZ_REQUESTH_
#include "lbz-common.h"

struct lbz_bio_hook {
	bio_end_io_t *user_endio;
	void *user_private;
};

struct lbz_req_hook {
	struct nvme_command *c;
	struct page *clone_pages;
	struct bio *user_bio;
};
blk_qc_t lbz_dev_submit_bio(struct bio *bio);
#endif
