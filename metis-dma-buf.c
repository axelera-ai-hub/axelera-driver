/*
 * Copyright (c) 2025 Axelera AI. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "metis-dmabuf.h"

#define IMP_DRV_NAME  "dmabuf_triton_importer"
#define IMP_INFO_NAME "triton_importer"

#define DMABUF_IOCTL_IMP_BASE	'I'
#define DMABUF_IOCTL_IMP_TEST	_IOWR(DMABUF_IOCTL_IMP_BASE, 0, int)
#define DMABUF_IOCTL_IMP_IMPORT _IOWR(DMABUF_IOCTL_IMP_BASE, 1, int)
#define DMABUF_IOCTL_IMP_ATTACH _IOWR(DMABUF_IOCTL_IMP_BASE, 2, int)
#define DMABUF_IOCTL_IMP_DETACH _IOWR(DMABUF_IOCTL_IMP_BASE, 3, int)
#define DMABUF_IOCTL_IMP_INFO \
	_IOWR(DMABUF_IOCTL_IMP_BASE, 4, struct dmabuf_import)
#define DMABUF_IOCTL_FULL_INFO \
	_IOWR(DMABUF_IOCTL_IMP_BASE, 5, struct dmabuf_full_import)
#define DMABUF_IOCTL_IMP_IMPORT_ATTACH _IOWR(DMABUF_IOCTL_IMP_BASE, 6, int)

static struct miscdevice imp_dev;

static long dmabuf_ioctl_import(struct file *filp, unsigned long arg)
{
	int fd;
	struct dma_buf *dmabuf;
	struct dmabuf_imp *di;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(int)))
		return -EFAULT;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	di = filp->private_data;
	di->dmabuf = dmabuf;
	return 0;
}

static long dmabuf_ioctl_attach(struct file *filp, unsigned long arg)
{
	struct dmabuf_imp *di;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	di = filp->private_data;
	dmabuf = di->dmabuf;
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attachment = dma_buf_attach(dmabuf, imp_dev.this_device);
	if (IS_ERR(attachment)) {
		pr_err("dma_buf_attach() failed\n");
		return PTR_ERR(attachment);
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		pr_err("dma_buf_map_attachment() failed\n");
		dma_buf_detach(dmabuf, attachment);
		return PTR_ERR(table);
	}
	di->attachment = attachment;
	di->table = table;
	di->phys = sg_dma_address(table->sgl);
	di->size = sg_dma_len(table->sgl);
	return 0;
}

static long dmabuf_ioctl_import_attach(struct file *filp, unsigned long arg)
{
	int fd;
	struct dma_buf *dmabuf;
	struct dmabuf_imp *di;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(int)))
		return -EFAULT;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	di = filp->private_data;
	di->dmabuf = dmabuf;

	attachment = dma_buf_attach(dmabuf, imp_dev.this_device);
	if (IS_ERR(attachment)) {
		pr_err("dma_buf_attach() failed\n");
		return PTR_ERR(attachment);
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		pr_err("dma_buf_map_attachment() failed\n");
		dma_buf_detach(dmabuf, attachment);
		return PTR_ERR(table);
	}
	di->attachment = attachment;
	di->table = table;
	di->phys = sg_dma_address(table->sgl);
	di->size = sg_dma_len(table->sgl);
	return 0;
}

static int dmabuf_get_contiguous_size(struct sg_table *sgt,
				      unsigned long *csize, int index,
				      uint64_t *phy)
{
	dma_addr_t expected = sg_dma_address(sgt->sgl);
	struct scatterlist *sg = sgt->sgl;
	unsigned long size = 0;
	unsigned int len = 0;
	int i;

	for (i = 0; i < index; i++, sg = sg_next(sg))
		pr_debug("%d index %d %p %llx %x\n", i, index, sg,
			 sg_dma_address(sg), sg_dma_len(sg));

	len = sg_dma_len(sg);
	expected = sg_dma_address(sg);
	*phy = expected;

	for (i = index; i < sgt->nents; i++, sg = sg_next(sg)) {
		len = sg_dma_len(sg);
		if (!len)
			break;
		if (sg_dma_address(sg) != expected) {
			break;
		}
		size += len;
		expected += len;
	}
	*csize = size;
	return i;
}

static long dmabuf_ioctl_info(struct file *filp, unsigned long arg)
{
	struct dmabuf_import info_import;
	struct scatterlist *sg;
	struct dmabuf_imp *di;
	int imp_index = 0, i;
	unsigned long contiguous_size;
	unsigned long total_size = 0;
	di = filp->private_data;

	pr_debug("phy addr = 0x%0llx, size = 0x%08lx (%d)\n", di->phys,
		 di->size, di->table->nents);

	if (copy_from_user(&info_import, (void __user *)arg,
			   sizeof(info_import)))
		return -EFAULT;
	if (info_import.nents == -1)
		return di->table->nents;
	if (info_import.nents > di->table->nents)
		return -EINVAL;

	sg = di->table->sgl;
	for (i = 0; i < di->table->nents; i++, sg = sg_next(sg)) {
		pr_debug("DEBUG: %d, 0x%p, pg 0x%p,%u+%u, dma 0x%llx,%u.\n", i,
			 sg, sg_page(sg), sg->offset, sg->length,
			 sg_dma_address(sg), sg_dma_len(sg));
		total_size += sg_dma_len(sg);
	}

	imp_index = dmabuf_get_contiguous_size(di->table, &contiguous_size,
					       info_import.nents,
					       &info_import.phy);
	pr_debug("DEBUG: Size Total 0x%08lx : phy 0x%llx Contiguous 0x%08lx\n",
		 total_size, info_import.phy, contiguous_size);

	info_import.size = contiguous_size;
	info_import.nents = imp_index;

	if (copy_to_user((int __user *)arg, &info_import, sizeof(info_import)))
		return -EFAULT;

	return di->table->nents - info_import.nents;
}

static long dmabuf_ioctl_full_info(struct file *filp, unsigned long arg)
{
	struct dmabuf_full_import fii;
	struct dmabuf_imp *di;
	struct dmabuf_sgl *dsgl;
	int i, max_nvecs = 0;
	struct scatterlist *sg;
	di = filp->private_data;

	if (copy_from_user(&fii, (void __user *)arg, sizeof(fii)))
		return -EFAULT;

	if (fii.nbase > di->table->nents)
		return -EINVAL;

	pr_debug("DEBUG: nvecs %d nents %d nbase %d dsgl %p\n", fii.nvecs,
		 di->table->nents, fii.nbase, fii.dsgl);
	if (fii.nvecs < di->table->nents - fii.nbase)
		max_nvecs = fii.nvecs;
	else
		max_nvecs = di->table->nents - fii.nbase;

	dsgl = kmalloc_array(max_nvecs, sizeof(struct dmabuf_sgl), GFP_KERNEL);
	if (!dsgl)
		return -ENOMEM;

	fii.nimp = di->table->nents;
	sg = di->table->sgl;
	for (i = 0; i < fii.nbase; i++, sg = sg_next(sg))
		;

	for (i = 0; i < max_nvecs; i++, sg = sg_next(sg)) {
		pr_debug(
			"DEBUG: %d (%d), 0x%p, pg 0x%p,%u+%u, dma 0x%llx,%u.\n",
			i, fii.nbase + i, sg, sg_page(sg), sg->offset,
			sg->length, sg_dma_address(sg), sg_dma_len(sg));
		dsgl[i].phy = sg_dma_address(sg);
		dsgl[i].size = sg_dma_len(sg);
	}
	fii.nbase += max_nvecs;

	if (_copy_to_user((int __user *)fii.dsgl, dsgl,
			  max_nvecs * sizeof(struct dmabuf_sgl))) {
		kfree(dsgl);
		return -EFAULT;
	}
	kfree(dsgl);
	if (copy_to_user((int __user *)arg, &fii, sizeof(fii)))
		return -EFAULT;
	if (max_nvecs < di->table->nents - fii.nbase)
		return di->table->nents - fii.nbase - max_nvecs;
	else
		return 0;
}

static long dmabuf_ioctl_detach(struct file *filp, unsigned long arg)
{
	struct dmabuf_imp *di;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	di = filp->private_data;
	dmabuf = di->dmabuf;
	attachment = di->attachment;
	table = di->table;

	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);
	dma_buf_put(dmabuf);
	di->dmabuf = NULL;

	return 0;
}
static long dmabuf_importer_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long ret = 0;

	pr_debug("DEBUG: cmd 0x%x, arg 0x%lx.\n", cmd, arg);
	switch (cmd) {
	case DMABUF_IOCTL_IMP_IMPORT:
		ret = dmabuf_ioctl_import(file, arg);
		break;

	case DMABUF_IOCTL_IMP_ATTACH:
		ret = dmabuf_ioctl_attach(file, arg);
		break;

	case DMABUF_IOCTL_IMP_IMPORT_ATTACH:
		ret = dmabuf_ioctl_import_attach(file, arg);
		break;

	case DMABUF_IOCTL_IMP_DETACH:
		ret = dmabuf_ioctl_detach(file, arg);
		break;

	case DMABUF_IOCTL_IMP_INFO: ret = dmabuf_ioctl_info(file, arg); break;

	case DMABUF_IOCTL_FULL_INFO:
		ret = dmabuf_ioctl_full_info(file, arg);
		break;

	default: ret = -ENOTTY; break;
	}

	return ret;
}

static int dmabuf_imp_open(struct inode *inode, struct file *file)
{
	struct dmabuf_imp *priv;
	int ret = 0;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	file->private_data = priv;

	return ret;
}

static int dmabuf_imp_release(struct inode *inode, struct file *file)
{
	struct dmabuf_imp *priv = file->private_data;
	int ret = 0;

	if (priv->dmabuf) {
		dma_buf_unmap_attachment(priv->attachment, priv->table,
					 DMA_BIDIRECTIONAL);
		dma_buf_detach(priv->dmabuf, priv->attachment);
		dma_buf_put(priv->dmabuf);
	}
	kfree(priv);

	return ret;
}

static struct file_operations importer_fops = {
	.owner = THIS_MODULE,
	.open = dmabuf_imp_open,
	.release = dmabuf_imp_release,
	.unlocked_ioctl = dmabuf_importer_ioctl,
};

static struct miscdevice imp_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = IMP_DRV_NAME,
	.fops = &importer_fops,
};

int __init metis_importer_init(void)
{
	int ret = misc_register(&imp_dev);
	if (ret < 0) {
		pr_err("Fail to register driver %s\n", IMP_DRV_NAME);
		return ret;
	}

	ret = dma_coerce_mask_and_coherent(imp_dev.this_device,
					   DMA_BIT_MASK(64));
	if (ret < 0) {
		pr_err("Fail to set dma mask and coherent dma msk %s (%d)\n",
		       IMP_DRV_NAME, ret);
		misc_deregister(&imp_dev);
	}

	return ret;
}

void __exit metis_importer_exit(void)
{
	misc_deregister(&imp_dev);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
MODULE_IMPORT_NS(DMA_BUF);
#endif
