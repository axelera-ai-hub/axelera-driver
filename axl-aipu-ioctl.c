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

#include <linux/version.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/aer.h>
#include <linux/msi.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/poll.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-fwtrace.h"
#include "axl-aipu-version.h"

unsigned int enable_dmabuf_sync = 1;
module_param(enable_dmabuf_sync, uint, 0644);
extern unsigned int dma_timeout;
#define MSI_DMA_WR_CH0 PMSI_DMA_RD_CH0
#define MSI_DMA_RD_CH0 PMSI_DMA_WR_CH0
static int axl_aipu_ch2id(int ch, int flags)
{
	if (flags & DMABUF_XFER_FLAG_READ)
		return MSI_DMA_RD_CH0 + ch;
	else if (flags & DMABUF_XFER_FLAG_WRITE)
		return MSI_DMA_WR_CH0 + ch;
	return MSI_DMA_WR_CH0 + ch;
}

static uint64_t axl_aipu_alloc_ctx(struct axl_pcie_aipu_dev *axldev,
				   int num_aicores)
{
	int pos, aicore_count = axldev->dev_info->aicore_count;
	uint64_t mask;

	if ((num_aicores <= 0) || (num_aicores > aicore_count))
		return 0;

	mask = (1 << num_aicores) - 1;
	for (pos = 0; pos < aicore_count - num_aicores + 1; ++pos) {
		if ((axldev->glob_ctx_mask & (mask << pos)) == 0) {
			// num_aicores consecutive cores free at pos
			return mask << pos;
		}
	}

	return 0;
}

void axl_aipu_dma_wrk_release(struct kref *kref)
{
	struct dma_wrk *dma_wrk = container_of(kref, struct dma_wrk, refcount);
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;

	dev_dbg(&pdev->dev, "dma_wrk %p released and freed\n", dma_wrk);

	/* Release dmabuf reference if still held */
	if (dma_wrk->dmabuf) {
		dev_dbg(&pdev->dev, "Release dmabuf ref from dma_wrk %p\n",
			dma_wrk);
		dma_buf_put(dma_wrk->dmabuf);
	}

	kfree(dma_wrk);
}

static void axl_aipu_dma_wrk_func(struct work_struct *work)
{
	struct dma_wrk *dma_wrk = container_of(work, struct dma_wrk, work);
	struct dma_queue_ctrl *dma_ctrl = dma_wrk->qctrl;
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx = dma_wrk->sctx;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	if (dma_wrk->flags & DMA_XFER_FLAG_P2P)
		axl_aipu_dma_p2p_job_submit(axldev, dma_wrk);
	else
		axl_aipu_dma_job_submit(axldev, dma_wrk);
	dev_dbg(&pdev->dev, "DMA %s CH%d done (%p)\n", mode, dma_wrk->channel,
		dma_wrk);
	if (atomic_read(&sctx->async_dma_xfer) == ASYNC_XFER_PENDING)
		atomic_set(&sctx->async_dma_xfer, ASYNC_XFER_DONE);

	complete_all(&dma_wrk->done);
	atomic_dec(&dma_ctrl->count);

	atomic_inc(&sctx->poll_event_cnt);
	wake_up_interruptible_poll(&sctx->poll_wait_queue, EPOLLIN);

	/* Release sys_ctx reference */
	dev_dbg(&pdev->dev, "Work %p releasing sys_ctx ref\n", dma_wrk);
	kref_put(&sctx->refcount, axl_aipu_sys_ctx_release);
}

static long sysctl_ioctl_import_attach(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_buf *dmabuf;
	struct dmabuf_imp *di = &sys_ctx->di;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	int fd;

	if (copy_from_user(&fd, (void __user *)arg, sizeof(int)))
		return -EFAULT;

	dev_dbg(&pdev->dev, "fd %d\n", fd);
	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attachment = dma_buf_attach(dmabuf, &axldev->pdev->dev);
	if (IS_ERR(attachment)) {
		dev_err(&pdev->dev, "dma_buf_attach() failed\n");
		return PTR_ERR(attachment);
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		dev_err(&pdev->dev, "dma_buf_map_attachment() failed\n");
		dma_buf_detach(dmabuf, attachment);
		return PTR_ERR(table);
	}
	di->attachment = attachment;
	di->table = table;
	di->phys = sg_dma_address(table->sgl);
	di->size = sg_dma_len(table->sgl);
	di->dmabuf = dmabuf;

	return 0;
}

static long sysctl_ioctl_get_free_ctx(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int pos;
	mutex_lock(&axldev->mutex);
	pos = first_set_bit(sys_ctx->ctx_mask);
	axldev->glob_ctx_mask &= ~(sys_ctx->ctx_mask);
	axldev->ctx_mask[pos] = 0;
	sys_ctx->ctx_mask = 0;
	dev_dbg(&pdev->dev, "Release (%d) 0x%llx by %d\n", pos,
		axldev->glob_ctx_mask, current->pid);
	mutex_unlock(&axldev->mutex);
	return 0;
}

static long sysctl_ioctl_get_dev_property(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	uint64_t property;
	uint64_t value = 0;

	if (copy_from_user(&property, (void __user *)arg, sizeof(uint64_t)))
		return -EFAULT;

	switch (property) {
	case AXL_PROPERTY_HW_GEN: value = axldev->dev_info->hw_gen; break;
	case AXL_PROPERTY_AICORE_COUNT:
		value = axldev->dev_info->aicore_count;
		break;
	case AXL_PROPERTY_PVE_CORE_COUNT:
		value = axldev->dev_info->pve_core_count;
		break;
	case AXL_PROPERTY_PES_GROUP: value = axldev->pes_group; break;
	default:
		dev_err(&pdev->dev, "Unknown device property %llu\n", property);
		return -EINVAL;
	}

	if (copy_to_user((uint64_t __user *)arg, &value, sizeof(uint64_t)))
		return -EFAULT;

	return 0;
}

static long sysctl_ioctl_get_ctx_aicore_msk(struct file *file,
					    unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	mutex_lock(&axldev->mutex);
	if (copy_to_user((int __user *)arg, &sys_ctx->ctx_mask,
			 sizeof(sys_ctx->ctx_mask))) {
		mutex_unlock(&axldev->mutex);
		return -EFAULT;
	}
	mutex_unlock(&axldev->mutex);
	return 0;
}

static long sysctl_ioctl_ctx_alloc(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	uint64_t loc_ctx_mask = 0;
	unsigned int pos, naicore;
	long ret = 0;

	if (copy_from_user(&naicore, (void __user *)arg, sizeof(int)))
		return -EFAULT;

	mutex_lock(&axldev->mutex);

	if (sys_ctx->ctx_mask) {
		dev_err(&pdev->dev,
			"File descriptor already allocated context (%p) ctx mask %llx\n",
			file->private_data, sys_ctx->ctx_mask);
		mutex_unlock(&axldev->mutex);
		return -EINVAL;
	}

	loc_ctx_mask = axl_aipu_alloc_ctx(axldev, naicore);
	if (loc_ctx_mask != 0) {
		axldev->glob_ctx_mask |= loc_ctx_mask;
		sys_ctx->ctx_mask = loc_ctx_mask;
		pos = first_set_bit(loc_ctx_mask);
		axldev->ctx_mask[pos] = loc_ctx_mask;
		ret = pos;

		dev_dbg(&pdev->dev,
			"Allocated context %d 0x%llx (0x%llx %p %d)\n", pos,
			axldev->glob_ctx_mask, sys_ctx->ctx_mask,
			file->private_data, current->pid);
	} else {
		dev_warn(
			&pdev->dev,
			"Failed to allocate context: failed to find %d free consecutive AI cores\n",
			naicore);
		ret = -EINVAL;
	}

	mutex_unlock(&axldev->mutex);
	return ret;
}

static long sysctl_ioctl_msg_lock(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int mode = (int)arg;
	dev_dbg(&pdev->dev, "AXL_IOCTL_MSG_LOCK %s\n",
		mode == 1 ? "LOCK" : "UNLOCK");
	if (mode) {
		if (mutex_lock_interruptible(&axldev->msg_mutex)) {
			dev_err(&pdev->dev, "Int LOCK by %d\n", current->pid);
			return -ERESTARTSYS;
		}
		sys_ctx->msg_flag = 1;
	} else {
		sys_ctx->msg_flag = 0;
		mutex_unlock(&axldev->msg_mutex);
	}
	return 0;
}

static long sysctl_ioctl_detach(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct dmabuf_imp *di = &sys_ctx->di;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	dev_dbg(&sys_ctx->axldev->pdev->dev, "Detach dmabuf %p\n", di->dmabuf);
	/* Check if DMA transfer is in progress */
	if (atomic_read(&sys_ctx->async_dma_xfer) == ASYNC_XFER_PENDING) {
		return -EBUSY;
	}

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
static long sysctl_ioctl_clean_msi(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct irq_wrk *irq_wrk = axldev->irq_wrk;
	struct msi_info msinfo;

	if (copy_from_user(&msinfo, (void __user *)arg,
			   sizeof(struct msi_info)))
		return -EFAULT;

	if (msinfo.num >= axldev->max_msi || msinfo.num < 0)
		return -EFAULT;

	dev_dbg(&pdev->dev, "clean msi %d\n", msinfo.num);
	reinit_completion(&irq_wrk[msinfo.num].irq_done);
	return 0;
}
static long sysctl_ioctl_msi(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct msi_info msinfo;
	int ret, timeout, nmsi;

	if (copy_from_user(&msinfo, (void __user *)arg,
			   sizeof(struct msi_info)))
		return -EFAULT;

	if ((msinfo.num > axldev->max_msi) || (msinfo.num < 0) ||
	    (msinfo.timeout < 0))
		return -EFAULT;

	nmsi = msinfo.num;
	dev_dbg(&pdev->dev, "Wait msi %d (timeout %d)\n", nmsi, msinfo.timeout);
	if (msinfo.timeout == 0) {
		ret = wait_for_completion_interruptible(
			&axldev->irq_wrk[nmsi].irq_done);
		if (ret) {
			dev_dbg(&pdev->dev, "IRQ MSI interrupted (%d)\n", nmsi);
			return -EINTR;
		}
	} else {
		timeout = msecs_to_jiffies(msinfo.timeout * 1000);
		ret = wait_for_completion_interruptible_timeout(
			&axldev->irq_wrk[nmsi].irq_done, timeout);
		if (ret > 0) {
			ret = 0;
		} else if (ret == 0) {
			if (nmsi != (PMSI_MSG + axldev->irq_vec))
				dev_err(&pdev->dev, "IRQ MSI timeout (%d %d)\n",
					nmsi, msinfo.timeout);
			else
				dev_dbg(&pdev->dev, "IRQ MSI timeout (%d %d)\n",
					nmsi, msinfo.timeout);
			ret = -ETIMEDOUT;
		} else {
			dev_dbg(&pdev->dev, "IRQ MSI interrupted (%d %d)\n",
				nmsi, msinfo.timeout);
			ret = -EINTR;
		}
	}
	return ret;
}

static long sysctl_ioctl_msi_attach(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct irq_wrk *irq_wrk = axldev->irq_wrk;
	int msi;

	if (copy_from_user(&msi, (void __user *)arg, sizeof(msi)))
		return -EFAULT;

	if (msi >= axldev->max_msi || msi < 0)
		return -EFAULT;

	dev_dbg(&pdev->dev, "attach msi %d\n", msi);

	spin_lock_irq(&axldev->msi_lock);
	list_add(&sys_ctx->node, &irq_wrk[msi].sctx_list);
	spin_unlock_irq(&axldev->msi_lock);

	return 0;
}

static long sysctl_ioctl_clear_poll_event(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int count;

	count = atomic_read(&sys_ctx->poll_event_cnt);
	if (count > 0) {
		atomic_dec(&sys_ctx->poll_event_cnt);
		dev_dbg(&pdev->dev, "Consumed poll event count %d\n", count);
		return 1; // Event consumed
	}

	return 0; // No events
}

static long sysctl_ioctl_usr_dma_xfer(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_queue_ctrl *dma_ctrl = NULL;

	struct dma_wrk *dma_wrk;
	struct dma_xfer xfer;
	struct page **page_list;
	unsigned long nr_pages;
	struct sg_table *sgt;
	unsigned int pg_off;
	int ret, i, channel, status, err;
	enum dma_data_direction dir;

	if (copy_from_user(&xfer, (void __user *)arg, sizeof(struct dma_xfer)))
		return -EFAULT;

	if ((xfer.size == 0) || (xfer.size > SIZE_MAX))
		return -EINVAL;

	channel = axl_aipu_find_free_dma_channel(axldev, xfer.flags);

	pg_off = offset_in_page(xfer.virt);
	nr_pages = (xfer.size + pg_off + PAGE_SIZE - 1) >> PAGE_SHIFT;
	page_list = kmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	ret = get_user_pages_fast(xfer.virt, nr_pages, 1, page_list);
	if (ret < 0) {
		ret = -ENOMEM;
		goto free_page_list;
	}
	if (ret != nr_pages) {
		nr_pages = ret;
		ret = -EFAULT;
		goto put_pages;
	}

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto put_pages;
	}

	ret = sg_alloc_table_from_pages(sgt, page_list, nr_pages, pg_off,
					xfer.size, GFP_KERNEL);
	if (ret) {
		ret = -ENOMEM;
		goto free_sgt;
	}

	ret = dma_map_sgtable(&pdev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret) {
		ret = -ENOMEM;
		goto free_table;
	}

	dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_ATOMIC);
	if (!dma_wrk)
		dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_KERNEL);
	if (!dma_wrk) {
		ret = -ENOMEM;
		goto free_table;
	}
	kref_init(&dma_wrk->refcount);
	dma_wrk->axldev = axldev;
	dma_wrk->id = axl_aipu_ch2id(channel, xfer.flags);
	init_completion(&dma_wrk->done);
	dma_wrk->timeout = get_timeout_ms(dma_timeout);
	dma_wrk->axi = xfer.phy;
	dma_wrk->table = sgt;
	dma_wrk->offset = 0;
	dma_wrk->num_sgt = 0;
	dma_wrk->size = xfer.size;
	dma_wrk->channel = channel;
	dma_wrk->flags = xfer.flags;
	dma_wrk->sctx = sys_ctx;
	dma_wrk->dmabuf = NULL;

	if (sys_ctx->dma_wrk)
		kref_put(&sys_ctx->dma_wrk->refcount, axl_aipu_dma_wrk_release);
	sys_ctx->dma_wrk = dma_wrk;
	dma_wrk->ktime = ktime_get();

	if (xfer.flags & DMABUF_XFER_FLAG_READ) {
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_rdqc[channel];
		dev_dbg(&pdev->dev, "queue RD %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	} else if (xfer.flags & DMABUF_XFER_FLAG_WRITE) {
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_wrqc[channel];
		dev_dbg(&pdev->dev, "queue WR %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	}
	dma_wrk->qctrl = dma_ctrl;
	atomic_inc(&dma_ctrl->count);
	dma_ctrl->num_xfer++;
	dma_ctrl->bytes_xfer += xfer.size;
	dma_ctrl->max_sgt = max_t(int, dma_ctrl->max_sgt, sgt->nents);
	dma_ctrl->size = xfer.size;
	dma_wrk->ktime = ktime_get();

	kref_get(&sys_ctx->refcount);
	queue_work(dma_ctrl->wq, &dma_wrk->work);
	err = wait_for_completion_timeout(&dma_wrk->done, dma_wrk->timeout);

	if (err == 0) {
		dev_err(&pdev->dev,
			"DMA %s CH%d queue timeout (%p status %d)\n",
			xfer.flags & DMABUF_XFER_FLAG_WRITE ? "WR" : "RD",
			channel, dma_wrk, dma_wrk->status);
		flush_work(&dma_wrk->work);
		err = -ETIMEDOUT;
	}
	status = dma_wrk->status;
	ret = err > 0 ? status : err;
	if (ret < 0) {
		dma_ctrl->num_err++;
		dev_err(&pdev->dev, "DMA %s CH%d status %d %d\n",
			xfer.flags & DMABUF_XFER_FLAG_WRITE ? "WR" : "RD",
			channel, status, err);
	}

	/*
	 * Clear pointer from sys_ctx and release our reference.
	 * The xchg ensures only one thread releases this reference.
	 */
	if (xchg(&sys_ctx->dma_wrk, NULL) != NULL)
		kref_put(&dma_wrk->refcount, axl_aipu_dma_wrk_release);

	dma_unmap_sgtable(&pdev->dev, sgt, dir, 0);
free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
put_pages:
	for (i = 0; i < nr_pages; i++) {
		if (page_list[i]) {
			if (dir == DMA_TO_DEVICE)
				set_page_dirty_lock(page_list[i]);
			put_page(page_list[i]);
		}
	}
free_page_list:
	kfree(page_list);
	return ret;
}

static long sysctl_ioctl_dma_xfer(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_queue_ctrl *dma_ctrl = NULL;

	struct dmabuf_imp *di = &sys_ctx->di;
	struct sg_table *sgt = di->table;
	struct dmabuf_xfer dxfer;
	int err = 0, channel, i, status,
	    max_dma_ch = axldev->dev_info->dma_rd_ch;

	struct scatterlist *sg;
	unsigned long size = 0;

	struct dma_wrk *dma_wrk;

	if (sys_ctx &&
	    atomic_read(&sys_ctx->async_dma_xfer) == ASYNC_XFER_PENDING) {
		dev_err(&pdev->dev,
			"Previous async DMA transfer not cleaned up (state=%d)\n",
			atomic_read(&sys_ctx->async_dma_xfer));
		return -EBUSY;
	}

	if (copy_from_user(&dxfer, (void __user *)arg,
			   sizeof(struct dmabuf_xfer)))
		return -EFAULT;

	if (IS_ERR(di) || !di->dmabuf || !di->table || !di->table->sgl) {
		dev_err(&pdev->dev,
			"No dmabuf attached or table invalid (dmabuf=%p, table=%p, sgl=%p)\n",
			di->dmabuf, di->table,
			di->table ? di->table->sgl : NULL);
		return -EINVAL;
	}

	err = validate_dma_xfer(&dxfer, pdev);
	if (err)
		return err;

	for_each_sgtable_dma_sg(sgt, sg, i)
	{
		unsigned int len = sg_dma_len(sg);
		if (!len)
			break;
		size += len;
	}
	if (size < (dxfer.size + dxfer.offset)) {
		dev_err(&pdev->dev,
			"Invalid size dmabuf %lx | req 0x%lx | offset 0x%x \n",
			size, dxfer.size, dxfer.offset);
		return -EINVAL;
	}

	if (dxfer.channel < 0) {
		dxfer.channel =
			axl_aipu_find_free_dma_channel(axldev, dxfer.flags);
	} else if (dxfer.channel >= max_dma_ch) {
		return -EINVAL;
	}
	channel = dxfer.channel;

	dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_ATOMIC);
	if (!dma_wrk)
		dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_KERNEL);
	if (!dma_wrk)
		return -ENOMEM;

	kref_init(&dma_wrk->refcount);
	dma_wrk->axldev = axldev;
	dma_wrk->id = axl_aipu_ch2id(channel, dxfer.flags);
	init_completion(&dma_wrk->done);
	dma_wrk->timeout = get_timeout_ms(dma_timeout);
	dma_wrk->axi = dxfer.phy;
	dma_wrk->table = sgt;
	dma_wrk->num_sgt = 0;
	dma_wrk->offset = dxfer.offset;
	dma_wrk->size = dxfer.size;
	dma_wrk->channel = channel;
	dma_wrk->flags = dxfer.flags;
	dma_wrk->sctx = sys_ctx;
	dma_wrk->ktime = ktime_get();
	dma_wrk->dmabuf = NULL;

	if (sys_ctx->dma_wrk)
		kref_put(&sys_ctx->dma_wrk->refcount, axl_aipu_dma_wrk_release);
	sys_ctx->dma_wrk = dma_wrk;

	if (dxfer.flags & DMABUF_XFER_FLAG_READ) {
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_rdqc[channel];
		dev_dbg(&pdev->dev, "queue RD %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	} else if (dxfer.flags & DMABUF_XFER_FLAG_WRITE) {
		if (enable_dmabuf_sync)
			dma_sync_sg_for_device(&pdev->dev, sgt->sgl, sgt->nents,
					       DMA_TO_DEVICE);
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_wrqc[channel];
		dev_dbg(&pdev->dev, "queue WR %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	}
	dma_wrk->qctrl = dma_ctrl;
	atomic_inc(&dma_ctrl->count);
	dma_ctrl->num_xfer++;
	dma_ctrl->bytes_xfer += dxfer.size;
	dma_ctrl->max_sgt = max_t(int, dma_ctrl->max_sgt, sgt->nents);
	dma_ctrl->size = dxfer.size;
	dma_wrk->ktime = ktime_get();

	/* Verify table is still valid before taking references */
	if (!di->dmabuf || !di->table || !di->table->sgl) {
		dev_err(&pdev->dev,
			"Table became invalid before queuing work\n");
		atomic_dec(&dma_ctrl->count);
		kfree(dma_wrk);
		return -EINVAL;
	}

	/* Take reference on dmabuf to keep it alive during transfer */
	get_dma_buf(di->dmabuf);
	dma_wrk->dmabuf = di->dmabuf;
	dev_dbg(&pdev->dev,
		"Acquired dmabuf ref %p for work %p (sctx %p, table %p)\n",
		di->dmabuf, dma_wrk, sys_ctx, di->table);

	kref_get(&sys_ctx->refcount);
	queue_work(dma_ctrl->wq, &dma_wrk->work);
	if (dxfer.flags & DMABUF_XFER_FLAG_SYNC) {
		err = wait_for_completion_timeout(&dma_wrk->done,
						  dma_wrk->timeout);
		if (err == 0) {
			dev_err(&pdev->dev,
				"DMA %s CH%d queue timeout (%p status %d)\n",
				dxfer.flags & DMABUF_XFER_FLAG_WRITE ? "WR" :
								       "RD",
				channel, dma_wrk, dma_wrk->status);
			flush_work(&dma_wrk->work);
			dma_ctrl->num_err++;
			err = -ETIMEDOUT;
		}
		status = dma_wrk->status;
		return err > 0 ? status : err;
	}

	if (dxfer.flags & DMABUF_XFER_FLAG_ASYNC) {
		atomic_set(&sys_ctx->async_dma_xfer, ASYNC_XFER_PENDING);
		return 0;
	}

	return err > 0 ? dma_wrk->status : err;
}

static long sysctl_ioctl_dma_p2p_xfer(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_queue_ctrl *dma_ctrl = NULL;
	struct dma_p2p_xfer xfer;

	int err, channel, status, max_dma_ch = axldev->dev_info->dma_rd_ch;
	struct dma_wrk *dma_wrk;

	if (sys_ctx &&
	    atomic_read(&sys_ctx->async_dma_xfer) == ASYNC_XFER_PENDING) {
		dev_err(&pdev->dev,
			"Previous async DMA transfer not cleaned up (state=%d)\n",
			atomic_read(&sys_ctx->async_dma_xfer));
		return -EBUSY;
	}

	if (copy_from_user(&xfer, (void __user *)arg,
			   sizeof(struct dma_p2p_xfer)))
		return -EFAULT;

	if (xfer.channel < 0) {
		xfer.channel =
			axl_aipu_find_free_dma_channel(axldev, xfer.flags);
	} else if (xfer.channel >= max_dma_ch) {
		return -EINVAL;
	}
	channel = xfer.channel;

	dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_ATOMIC);
	if (!dma_wrk)
		dma_wrk = kmalloc(sizeof(*dma_wrk), GFP_KERNEL);
	if (!dma_wrk)
		return -ENOMEM;

	kref_init(&dma_wrk->refcount);
	dma_wrk->axldev = axldev;
	dma_wrk->id = axl_aipu_ch2id(channel, xfer.flags);
	init_completion(&dma_wrk->done);
	dma_wrk->timeout = get_timeout_ms(dma_timeout);
	dma_wrk->axi = xfer.axi;
	dma_wrk->p2pphy = xfer.p2pphy;
	dma_wrk->size = xfer.size;
	dma_wrk->channel = channel;
	dma_wrk->flags = xfer.flags;
	dma_wrk->sctx = sys_ctx;
	dma_wrk->dmabuf = NULL;
	dma_wrk->ktime = ktime_get();

	if (sys_ctx->dma_wrk)
		kref_put(&sys_ctx->dma_wrk->refcount, axl_aipu_dma_wrk_release);
	sys_ctx->dma_wrk = dma_wrk;

	if (xfer.flags & DMABUF_XFER_FLAG_READ) {
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_rdqc[channel];
		dev_dbg(&pdev->dev, "queue RD %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	} else if (xfer.flags & DMABUF_XFER_FLAG_WRITE) {
		INIT_WORK(&dma_wrk->work, axl_aipu_dma_wrk_func);
		dma_ctrl = &axldev->dma_wrqc[channel];
		dev_dbg(&pdev->dev, "queue WR %d irq %d work (%p %p)\n",
			channel, dma_wrk->id, dma_ctrl->wq, dma_wrk);
	}
	dma_wrk->qctrl = dma_ctrl;
	atomic_inc(&dma_ctrl->count);
	dma_ctrl->num_xfer++;
	dma_ctrl->bytes_xfer += xfer.size;
	dma_ctrl->size = xfer.size;
	dma_wrk->ktime = ktime_get();

	kref_get(&sys_ctx->refcount);
	queue_work(dma_ctrl->wq, &dma_wrk->work);
	if (xfer.flags & DMABUF_XFER_FLAG_SYNC) {
		err = wait_for_completion_timeout(&dma_wrk->done,
						  dma_wrk->timeout);
		if (err == 0) {
			dev_err(&pdev->dev,
				"DMA %s CH%d queue timeout (%p status %d)\n",
				xfer.flags & DMABUF_XFER_FLAG_WRITE ? "WR" :
								      "RD",
				channel, dma_wrk, dma_wrk->status);
			flush_work(&dma_wrk->work);
			dma_ctrl->num_err++;
			err = -ETIMEDOUT;
		}
		status = dma_wrk->status;
		return err > 0 ? status : err;
	}

	if (xfer.flags & DMABUF_XFER_FLAG_ASYNC) {
		atomic_set(&sys_ctx->async_dma_xfer, ASYNC_XFER_PENDING);
		return 0;
	}

	return err > 0 ? dma_wrk->status : err;
}

static long sysctl_ioctl_dynmem_load(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;

	axl_aipu_config_dev_dma(axldev);
	axl_aipu_config_dev_msi(axldev);
	axl_aipu_dev_dynmem_init(axldev);
	axl_aipu_fwtrace_refresh(axldev);

	return 0;
}

static long sysctl_ioctl_dma_get_xfer_sync_status(struct file *file,
						  unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dmabuf_xfer dxfer;
	struct dma_wrk *dma_wrk;
	int err, status;

	if (copy_from_user(&dxfer, (void __user *)arg,
			   sizeof(struct dmabuf_xfer)))
		return -EFAULT;

	dev_dbg(&pdev->dev, "sys_ctx %p work %p sync_dma_xfer %d\n", sys_ctx,
		sys_ctx->dma_wrk, atomic_read(&sys_ctx->async_dma_xfer));

	if (!(dxfer.flags & DMABUF_XFER_FLAG_SYNC)) {
		dev_err(&pdev->dev, "Invalid flags %d\n", dxfer.flags);
		return -EINVAL;
	}
	if (!atomic_read(&sys_ctx->async_dma_xfer)) {
		dev_err(&pdev->dev, "Invalid async_dma_xfer %d\n",
			atomic_read(&sys_ctx->async_dma_xfer));
		return -EINVAL;
	}

	dma_wrk = sys_ctx->dma_wrk;
	if (!dma_wrk) {
		dev_err(&pdev->dev, "No DMA work in progress\n");
		return -EINVAL;
	}
	err = wait_for_completion_timeout(&dma_wrk->done, dma_wrk->timeout);
	if (err == 0) {
		dev_err(&pdev->dev,
			"DMA %s CH%d qeueue timeout (%p status %d)\n",
			dxfer.flags & DMABUF_XFER_FLAG_WRITE ? "WR" : "RD",
			dma_wrk->channel, dma_wrk, dma_wrk->status);
		flush_work(&dma_wrk->work);
		err = -ETIMEDOUT;
	}
	status = dma_wrk->status;
	if (err < 0)
		dma_wrk->qctrl->num_err++;

	return err > 0 ? status : err;
}
static long sysctl_ioctl_dma_get_xfer_async_status(struct file *file,
						   unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	dev_dbg(&pdev->dev, "sys_ctx %p work %p async_dma_xfer %d\n", sys_ctx,
		sys_ctx->dma_wrk, atomic_read(&sys_ctx->async_dma_xfer));
	return atomic_read(&sys_ctx->async_dma_xfer);
}

static long sysctl_ioctl_get_dma_stats(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_stats stats;
	struct dma_queue_ctrl *dma_ctrl;
	int i;

	dev_dbg(&pdev->dev, "Getting DMA statistics\n");

	/* Clear the stats structure */
	memset(&stats, 0, sizeof(stats));

	/* Populate read channel statistics */
	for (i = 0; i < axldev->dev_info->dma_rd_ch; i++) {
		dma_ctrl = &axldev->dma_rdqc[i];

		stats.rd_channels[i].count = atomic_read(&dma_ctrl->count);
		stats.rd_channels[i].num_xfer = dma_ctrl->num_xfer;
		stats.rd_channels[i].num_err = dma_ctrl->num_err;
		stats.rd_channels[i].bytes_xfer = dma_ctrl->bytes_xfer;
		stats.rd_channels[i].max_sgt = dma_ctrl->max_sgt;
		stats.rd_channels[i].max_duration_us =
			ktime_to_us(dma_ctrl->max_duration);
		stats.rd_channels[i].duration_us =
			ktime_to_us(dma_ctrl->duration);
		stats.rd_channels[i].size = dma_ctrl->size;

		/* Calculate speed in MB/s (avoid division by zero) */
		if (ktime_to_us(dma_ctrl->duration) > 0) {
			stats.rd_channels[i].speed_mbps =
				dma_ctrl->size /
				(ktime_to_us(dma_ctrl->duration));
		} else {
			stats.rd_channels[i].speed_mbps = 0;
		}
	}

	/* Populate write channel statistics */
	for (i = 0; i < axldev->dev_info->dma_wr_ch; i++) {
		dma_ctrl = &axldev->dma_wrqc[i];

		stats.wr_channels[i].count = atomic_read(&dma_ctrl->count);
		stats.wr_channels[i].num_xfer = dma_ctrl->num_xfer;
		stats.wr_channels[i].num_err = dma_ctrl->num_err;
		stats.wr_channels[i].bytes_xfer = dma_ctrl->bytes_xfer;
		stats.wr_channels[i].max_sgt = dma_ctrl->max_sgt;
		stats.wr_channels[i].max_duration_us =
			ktime_to_us(dma_ctrl->max_duration);
		stats.wr_channels[i].duration_us =
			ktime_to_us(dma_ctrl->duration);
		stats.wr_channels[i].size = dma_ctrl->size;

		/* Calculate speed in MB/s (avoid division by zero) */
		if (ktime_to_us(dma_ctrl->duration) > 0) {
			stats.wr_channels[i].speed_mbps =
				dma_ctrl->size /
				(ktime_to_us(dma_ctrl->duration));
		} else {
			stats.wr_channels[i].speed_mbps = 0;
		}
	}

	/* Copy statistics to user space */
	if (copy_to_user((void __user *)arg, &stats,
			 sizeof(struct dma_stats))) {
		dev_err(&pdev->dev,
			"Failed to copy DMA statistics to user space\n");
		return -EFAULT;
	}

	return 0;
}

static long sysctl_ioctl_get_resource_info(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	if (copy_to_user((void __user *)arg, axldev->res_info,
			 sizeof(struct dev_res_info))) {
		dev_err(&pdev->dev,
			"Failed to copy device resource info to user space\n");
		return -EFAULT;
	}
	return 0;
}

static long sysctl_ioctl_get_pcie_windows(struct file *file, unsigned long arg)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	if (copy_to_user((void __user *)arg, axldev->mem_win,
			 sizeof(struct dev_mem_window))) {
		dev_err(&pdev->dev,
			"Failed to copy device pcie mem windows to user space\n");
		return -EFAULT;
	}
	return 0;
}

long sysctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;
	switch (cmd) {
	case AXL_IOCTL_MSG_LOCK: ret = sysctl_ioctl_msg_lock(file, arg); break;
	case AXL_IOCTL_CTX_ALLOC:
		ret = sysctl_ioctl_ctx_alloc(file, arg);
		break;
	case AXL_IOCTL_CTX_FREE:
		ret = sysctl_ioctl_get_free_ctx(file, arg);
		break;
	case AXL_IOCTL_GET_DEV_PROPERTY:
		ret = sysctl_ioctl_get_dev_property(file, arg);
		break;
	case AXL_IOCTL_GET_CTX_AICORE_MASK:
		ret = sysctl_ioctl_get_ctx_aicore_msk(file, arg);
		break;
	case AXL_IOCTL_DMA_ATTACH:
		ret = sysctl_ioctl_import_attach(file, arg);
		break;
	case AXL_IOCTL_DMA_DETACH: ret = sysctl_ioctl_detach(file, arg); break;
	case AXL_IOCTL_DMA_XFER: ret = sysctl_ioctl_dma_xfer(file, arg); break;
	case AXL_IOCTL_DMA_GET_XFER_ASYNC_STATUS:
		ret = sysctl_ioctl_dma_get_xfer_async_status(file, arg);
		break;
	case AXL_IOCTL_DMA_GET_XFER_SYNC_STATUS:
		ret = sysctl_ioctl_dma_get_xfer_sync_status(file, arg);
		break;
	case AXL_IOCTL_USR_DMA_XFER:
		ret = sysctl_ioctl_usr_dma_xfer(file, arg);
		break;
	case AXL_IOCTL_MSI: ret = sysctl_ioctl_msi(file, arg); break;
	case AXL_IOCTL_CLEAN_MSI:
		ret = sysctl_ioctl_clean_msi(file, arg);
		break;
	case AXL_IOCTL_MSI_ATTACH:
		ret = sysctl_ioctl_msi_attach(file, arg);
		break;
	case AXL_IOCTL_CLEAR_POLL_EVENT:
		ret = sysctl_ioctl_clear_poll_event(file, arg);
		break;
	case AXL_IOCTL_GET_DMA_STATS:
		ret = sysctl_ioctl_get_dma_stats(file, arg);
		break;
	case AXL_IOCTL_GET_RESOURCE_INFO:
		ret = sysctl_ioctl_get_resource_info(file, arg);
		break;
	case AXL_IOCTL_GET_PCIE_WINDOWS:
		ret = sysctl_ioctl_get_pcie_windows(file, arg);
		break;
	case AXL_IOCTL_DMA_P2P_XFER:
		ret = sysctl_ioctl_dma_p2p_xfer(file, arg);
		break;
	case AXL_IOCTL_DYNMEM_LOAD:
		ret = sysctl_ioctl_dynmem_load(file, arg);
		break;
	/* Firmware trace IOCTLs */
	case AXL_IOCTL_FWTRACE_OPEN_SESSION: {
		uint32_t source;
		if (copy_from_user(&source, (void __user *)arg,
				   sizeof(source))) {
			ret = -EFAULT;
			break;
		}
		ret = axl_fwtrace_open_session(file, source);
		break;
	}
	case AXL_IOCTL_FWTRACE_CLOSE_SESSION:
		ret = axl_fwtrace_close_session(file);
		break;
	case AXL_IOCTL_FWTRACE_GET_STATS: {
		struct fwtrace_stats stats;
		ret = axl_fwtrace_get_stats(file, &stats);
		if (ret == 0) {
			if (copy_to_user((void __user *)arg, &stats,
					 sizeof(stats)))
				ret = -EFAULT;
		}
		break;
	}
	case AXL_IOCTL_FWTRACE_ENABLE: {
		struct sysctrl_ctx *sys_ctx = file->private_data;
		uint32_t source;
		if (copy_from_user(&source, (void __user *)arg,
				   sizeof(source))) {
			ret = -EFAULT;
			break;
		}
		ret = axl_fwtrace_enable(sys_ctx->axldev, source);
		break;
	}
	case AXL_IOCTL_FWTRACE_DISABLE: {
		struct sysctrl_ctx *sys_ctx = file->private_data;
		uint32_t source;
		if (copy_from_user(&source, (void __user *)arg,
				   sizeof(source))) {
			ret = -EFAULT;
			break;
		}
		ret = axl_fwtrace_disable(sys_ctx->axldev, source);
		break;
	}
	case AXL_IOCTL_FWTRACE_CLEAR_BUFFER: {
		uint32_t source;
		if (copy_from_user(&source, (void __user *)arg,
				   sizeof(source))) {
			ret = -EFAULT;
			break;
		}
		ret = axl_fwtrace_clear_buffer(file, source);
		break;
	}
	default: ret = -ENOTTY; break;
	}
	return ret;
}
