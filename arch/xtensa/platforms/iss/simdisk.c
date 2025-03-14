/*
 * arch/xtensa/platforms/iss/simdisk.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001-2013 Tensilica Inc.
 *   Authors	Victor Prupis
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <platform/simcall.h>

#define SIMDISK_MAJOR 240
#define SIMDISK_MINORS 1
#define MAX_SIMDISK_COUNT 10

struct simdisk {
	const char *filename;
	spinlock_t lock;
	struct gendisk *gd;
	struct proc_dir_entry *procfile;
	int users;
	unsigned long size;
	int fd;
};


static int simdisk_count = CONFIG_BLK_DEV_SIMDISK_COUNT;
module_param(simdisk_count, int, S_IRUGO);
MODULE_PARM_DESC(simdisk_count, "Number of simdisk units.");

static int n_files;
static const char *filename[MAX_SIMDISK_COUNT] = {
#ifdef CONFIG_SIMDISK0_FILENAME
	CONFIG_SIMDISK0_FILENAME,
#ifdef CONFIG_SIMDISK1_FILENAME
	CONFIG_SIMDISK1_FILENAME,
#endif
#endif
};

static int simdisk_param_set_filename(const char *val,
		const struct kernel_param *kp)
{
	if (n_files < ARRAY_SIZE(filename))
		filename[n_files++] = val;
	else
		return -EINVAL;
	return 0;
}

static const struct kernel_param_ops simdisk_param_ops_filename = {
	.set = simdisk_param_set_filename,
};
module_param_cb(filename, &simdisk_param_ops_filename, &n_files, 0);
MODULE_PARM_DESC(filename, "Backing storage filename.");

static int simdisk_major = SIMDISK_MAJOR;

static void simdisk_transfer(struct simdisk *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector << SECTOR_SHIFT;
	unsigned long nbytes = nsect << SECTOR_SHIFT;

	if (offset > dev->size || dev->size - offset < nbytes) {
		pr_notice("Beyond-end %s (%ld %ld)\n",
				str_write_read(write), offset, nbytes);
		return;
	}

	spin_lock(&dev->lock);
	while (nbytes > 0) {
		unsigned long io;

		simc_lseek(dev->fd, offset, SEEK_SET);
		READ_ONCE(*buffer);
		if (write)
			io = simc_write(dev->fd, buffer, nbytes);
		else
			io = simc_read(dev->fd, buffer, nbytes);
		if (io == -1) {
			pr_err("SIMDISK: IO error %d\n", errno);
			break;
		}
		buffer += io;
		offset += io;
		nbytes -= io;
	}
	spin_unlock(&dev->lock);
}

static void simdisk_submit_bio(struct bio *bio)
{
	struct simdisk *dev = bio->bi_bdev->bd_disk->private_data;
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter) {
		char *buffer = bvec_kmap_local(&bvec);
		unsigned len = bvec.bv_len >> SECTOR_SHIFT;

		simdisk_transfer(dev, sector, len, buffer,
				bio_data_dir(bio) == WRITE);
		sector += len;
		kunmap_local(buffer);
	}

	bio_endio(bio);
}

static int simdisk_open(struct gendisk *disk, blk_mode_t mode)
{
	struct simdisk *dev = disk->private_data;

	spin_lock(&dev->lock);
	++dev->users;
	spin_unlock(&dev->lock);
	return 0;
}

static void simdisk_release(struct gendisk *disk)
{
	struct simdisk *dev = disk->private_data;
	spin_lock(&dev->lock);
	--dev->users;
	spin_unlock(&dev->lock);
}

static const struct block_device_operations simdisk_ops = {
	.owner		= THIS_MODULE,
	.submit_bio	= simdisk_submit_bio,
	.open		= simdisk_open,
	.release	= simdisk_release,
};

static struct simdisk *sddev;
static struct proc_dir_entry *simdisk_procdir;

static int simdisk_attach(struct simdisk *dev, const char *filename)
{
	int err = 0;

	filename = kstrdup(filename, GFP_KERNEL);
	if (filename == NULL)
		return -ENOMEM;

	spin_lock(&dev->lock);

	if (dev->fd != -1) {
		err = -EBUSY;
		goto out;
	}
	dev->fd = simc_open(filename, O_RDWR, 0);
	if (dev->fd == -1) {
		pr_err("SIMDISK: Can't open %s: %d\n", filename, errno);
		err = -ENODEV;
		goto out;
	}
	dev->size = simc_lseek(dev->fd, 0, SEEK_END);
	set_capacity(dev->gd, dev->size >> SECTOR_SHIFT);
	dev->filename = filename;
	pr_info("SIMDISK: %s=%s\n", dev->gd->disk_name, dev->filename);
out:
	if (err)
		kfree(filename);
	spin_unlock(&dev->lock);

	return err;
}

static int simdisk_detach(struct simdisk *dev)
{
	int err = 0;

	spin_lock(&dev->lock);

	if (dev->users != 0) {
		err = -EBUSY;
	} else if (dev->fd != -1) {
		if (simc_close(dev->fd)) {
			pr_err("SIMDISK: error closing %s: %d\n",
					dev->filename, errno);
			err = -EIO;
		} else {
			pr_info("SIMDISK: %s detached from %s\n",
					dev->gd->disk_name, dev->filename);
			dev->fd = -1;
			kfree(dev->filename);
			dev->filename = NULL;
		}
	}
	spin_unlock(&dev->lock);
	return err;
}

static ssize_t proc_read_simdisk(struct file *file, char __user *buf,
			size_t size, loff_t *ppos)
{
	struct simdisk *dev = pde_data(file_inode(file));
	const char *s = dev->filename;
	if (s) {
		ssize_t len = strlen(s);
		char *temp = kmalloc(len + 2, GFP_KERNEL);

		if (!temp)
			return -ENOMEM;

		len = scnprintf(temp, len + 2, "%s\n", s);
		len = simple_read_from_buffer(buf, size, ppos,
					      temp, len);

		kfree(temp);
		return len;
	}
	return simple_read_from_buffer(buf, size, ppos, "\n", 1);
}

static ssize_t proc_write_simdisk(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	char *tmp = memdup_user_nul(buf, count);
	struct simdisk *dev = pde_data(file_inode(file));
	int err;

	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	err = simdisk_detach(dev);
	if (err != 0)
		goto out_free;

	if (count > 0 && tmp[count - 1] == '\n')
		tmp[count - 1] = 0;

	if (tmp[0])
		err = simdisk_attach(dev, tmp);

	if (err == 0)
		err = count;
out_free:
	kfree(tmp);
	return err;
}

static const struct proc_ops simdisk_proc_ops = {
	.proc_read	= proc_read_simdisk,
	.proc_write	= proc_write_simdisk,
	.proc_lseek	= default_llseek,
};

static int __init simdisk_setup(struct simdisk *dev, int which,
		struct proc_dir_entry *procdir)
{
	struct queue_limits lim = {
		.features		= BLK_FEAT_ROTATIONAL,
	};
	char tmp[2] = { '0' + which, 0 };
	int err;

	dev->fd = -1;
	dev->filename = NULL;
	spin_lock_init(&dev->lock);
	dev->users = 0;

	dev->gd = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(dev->gd)) {
		err = PTR_ERR(dev->gd);
		goto out;
	}
	dev->gd->major = simdisk_major;
	dev->gd->first_minor = which;
	dev->gd->minors = SIMDISK_MINORS;
	dev->gd->fops = &simdisk_ops;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 32, "simdisk%d", which);
	set_capacity(dev->gd, 0);
	err = add_disk(dev->gd);
	if (err)
		goto out_cleanup_disk;

	dev->procfile = proc_create_data(tmp, 0644, procdir, &simdisk_proc_ops, dev);

	return 0;

out_cleanup_disk:
	put_disk(dev->gd);
out:
	return err;
}

static int __init simdisk_init(void)
{
	int i;

	if (register_blkdev(simdisk_major, "simdisk") < 0) {
		pr_err("SIMDISK: register_blkdev: %d\n", simdisk_major);
		return -EIO;
	}
	pr_info("SIMDISK: major: %d\n", simdisk_major);

	if (n_files > simdisk_count)
		simdisk_count = n_files;
	if (simdisk_count > MAX_SIMDISK_COUNT)
		simdisk_count = MAX_SIMDISK_COUNT;

	sddev = kmalloc_array(simdisk_count, sizeof(*sddev), GFP_KERNEL);
	if (sddev == NULL)
		goto out_unregister;

	simdisk_procdir = proc_mkdir("simdisk", 0);
	if (simdisk_procdir == NULL)
		goto out_free_unregister;

	for (i = 0; i < simdisk_count; ++i) {
		if (simdisk_setup(sddev + i, i, simdisk_procdir) == 0) {
			if (filename[i] != NULL && filename[i][0] != 0 &&
					(n_files == 0 || i < n_files))
				simdisk_attach(sddev + i, filename[i]);
		}
	}

	return 0;

out_free_unregister:
	kfree(sddev);
out_unregister:
	unregister_blkdev(simdisk_major, "simdisk");
	return -ENOMEM;
}
module_init(simdisk_init);

static void simdisk_teardown(struct simdisk *dev, int which,
		struct proc_dir_entry *procdir)
{
	char tmp[2] = { '0' + which, 0 };

	simdisk_detach(dev);
	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}
	remove_proc_entry(tmp, procdir);
}

static void __exit simdisk_exit(void)
{
	int i;

	for (i = 0; i < simdisk_count; ++i)
		simdisk_teardown(sddev + i, i, simdisk_procdir);
	remove_proc_entry("simdisk", 0);
	kfree(sddev);
	unregister_blkdev(simdisk_major, "simdisk");
}
module_exit(simdisk_exit);

MODULE_ALIAS_BLOCKDEV_MAJOR(SIMDISK_MAJOR);

MODULE_LICENSE("GPL");
