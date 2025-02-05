#include "lbz-proc.h"
#include "lbz-dev.h"
#include "lbz-nat-sit.h"

#define LBZ_MSG_PREFIX "lbz-proc"

#define MESSSAGE_LENGTH 80
struct proc_dir_entry *lbz_proc_file;
#define PROC_FILE_ENTRY_FILENAME "lbz"

ssize_t module_input(struct file *filp, const char *buff, size_t len, loff_t *off)
{
	int i = 0;
	int rc = 0;
	int id;
	int cnt;
	long dev_size;
	char Message[MESSSAGE_LENGTH];
	char dev_path[BDEVNAME_SIZE];
	struct block_device *bd = NULL;

	for (i=0;i < MESSSAGE_LENGTH - 1 && i < len;i++)
		get_user(Message[i], buff+i);

	switch (Message[0]) {
		case 'd':
			cnt = sscanf((Message + 1), "%d", &id);
			if (cnt < 1) {
				LBZERR("input error %s", Message);
				rc = -EPERM;
				goto out;
			}
			break;
		case 'r':
			cnt = sscanf((Message + 1), "%d", &id);
			if (cnt < 1) {
				LBZERR("input error %s", Message);
				rc = -EPERM;
				goto out;
			}
			break;
		case 'c':
			cnt = sscanf((Message + 1), "%ld,%s", &dev_size, dev_path);
			if (cnt < 2) {
				LBZERR("input error %s", Message);
				rc = -EPERM;
				goto out;
			}
			bd = blkdev_get_by_path(dev_path, FMODE_READ | FMODE_WRITE | FMODE_EXCL, lbz_proc_file);
			if (IS_ERR(bd)) {
				rc = PTR_ERR(bd);
				LBZERR("open %s error:%d", dev_path, rc);
				return rc;
			}
			rc = lbz_create_device(PAGE_SIZE, dev_size << 21, bd);
			if (rc) {
				LBZERR("create device error:%d", rc);
				goto out;
			}
			LBZINFO("added 1 new lbz device, size: %ld", dev_size);
			break;
#ifdef CONFIG_LBZ_NAT_SIT_SUPPORT
		case 's':
			struct nat_sit_args args;
			int dev_minor;
			struct lbz_device *dev;

			cnt = sscanf((Message + 1), "%d,%u,%u,%u,%u",
					&dev_minor, &args.cp_blkaddr, &args.sit_blkaddr, &args.nat_blkaddr, &args.nat_blocks);
			if (cnt < 5) {
				LBZERR("input error %s", Message);
				rc = -EINVAL;
				goto out;
			}
			dev = lbz_dev_find_by_minor(dev_minor);
			if (NULL == dev) {
				LBZERR("dev not found, minor: %d", dev_minor);
				rc = -EINVAL;
				goto out;
			}
			lbz_nat_sit_add_rule(dev->nat_sit_mgmt, &args, dev);
			break;
#endif
	}
out:
	if (rc)
		return rc;
	return i;
}

ssize_t module_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
	int len = 0;
	char *buf = NULL;

	if (*offset)
		return 0;

	*offset = 1;

	if (!(buf = (void *)__get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	len = sprintf(buf, "LBZ INFO as fllows:\n");

	if (copy_to_user(buffer, buf, len))
		len = -EFAULT;

	free_page((unsigned long) buf);
	return len;
}

int module_open(struct inode *inode, struct file *file)
{
	try_module_get(THIS_MODULE);
	return 0;
}

int module_close(struct inode *inode,struct file *file)
{
	module_put(THIS_MODULE);
	return 0;
}

const struct proc_ops lbz_proc_mgmt_ops = {
	.proc_write = module_input,
	.proc_open = module_open,
	.proc_release = module_close,
	.proc_read = module_read
};

int lbz_proc_init(void)
{
	lbz_proc_file = proc_create_data(PROC_FILE_ENTRY_FILENAME, 0644, NULL, &lbz_proc_mgmt_ops, NULL);
	if (!lbz_proc_file)
		return -ENOENT;
	else
		return 0;
	LBZINFO("Hello lbz proc entry");
}

void lbz_proc_exit(void)
{
	remove_proc_entry(PROC_FILE_ENTRY_FILENAME, NULL);
	LBZINFO("Goodbye, cruel world");
}

static int __init lbz_module_init(void)
{
	int ret = 0;

	ret = lbz_proc_init();
	if (ret < 0) {
		LBZERR("init proc mgmt dir failed: %d", ret);
		return ret;
	}
	ret = lbz_dev_init();
	if (ret < 0) {
		lbz_proc_exit();
		LBZERR("init dev failed: %d", ret);
		return ret;
	}
	return 0;
}

static void lbz_module_exit(void)
{
	lbz_dev_exit();
	lbz_proc_exit();
}

module_exit(lbz_module_exit);
module_init(lbz_module_init);
MODULE_DESCRIPTION("LBZ: a Log-structured block device for zns");
MODULE_AUTHOR("Yang Yongpeng <yangyongpeng18b@ict.ac.cn>");
MODULE_LICENSE("GPL");
