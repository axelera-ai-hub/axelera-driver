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

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-pcie-hdma.h"
#include "axl-aipu-hdma-core.h"
#include "axl-aipu-version.h"

extern struct dentry *axl_aipu_debugfs_root;

static ssize_t axl_aipu_hdma_debugfs_info_read(struct file *file,
					       char __user *user_buf,
					       size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	struct pci_dev *pdev = axldev->pdev;
	char *strbuf;
	int i;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = min_t(size_t, count, 0x1000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
			 "\n\tDevice Information:\n\n");
	off += scnprintf(strbuf + off, size - off, "\tDevice Name: %s\n",
			 dev_name(&pdev->dev));
	off += scnprintf(strbuf + off, size - off, "\tDevice ID:   %04x:%04x\n",
			 pdev->vendor, pdev->device);
	off += scnprintf(strbuf + off, size - off,
			 "\tDriver Name: %s-%x:%x:%x\n",
			 axldev->dev_info->devname, pci_domain_nr(pdev->bus),
			 pdev->bus->number, PCI_SLOT(pdev->devfn));
	off += scnprintf(strbuf + off, size - off, "\tContext ID:  0x%llx\n",
			 axldev->glob_ctx_mask);
	mutex_lock(&axldev->mutex);
	for (i = 0; i < CONTEXT_COUNT; i++) {
		if (axldev->ctx_mask[i]) {
			off += scnprintf(strbuf + off, size - off,
					 "\t\tCTX %d: 0x%llx DMA %d MSI %d\n",
					 i, axldev->ctx_mask[i], i, i);
		}
	}
	mutex_unlock(&axldev->mutex);
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_hdma_debugfs_info_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_info_read,
};

#define SHOW_HDMA_SW_REG32(offset, channel, reg)                     \
	scnprintf(strbuf + off, size - off, "%40s  0x%08x 0x%08x\n", \
		  __stringify(HDMA_##offset##_##channel),            \
		  HDMA_##offset##_##channel, hdma->ch[channel].reg);

static ssize_t axl_aipu_hdma_debugfs_dbg_regs_read_0(struct file *file,
						     char __user *user_buf,
						     size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += SHOW_HDMA_SW_REG32(EN_OFF_WRCH, 0, wr.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_WRCH, 0, wr.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_WRCH, 0, wr.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_WRCH, 0, wr.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_WRCH, 0, wr.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_WRCH, 0, wr.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_WRCH, 0, wr.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_WRCH, 0, wr.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_WRCH, 0, wr.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_WRCH, 0, wr.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_WRCH, 0, wr.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_WRCH, 0, wr.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_WRCH, 0, wr.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_WRCH, 0, wr.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_WRCH, 0, wr.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_WRCH, 0, wr.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_WRCH, 0, wr.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_WRCH, 0, wr.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_WRCH, 0, wr.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_WRCH, 0, wr.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_WRCH, 0, wr.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_WRCH, 0, wr.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_WRCH, 0,
				  wr.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_WRCH, 0,
				  wr.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_WRCH, 0, wr.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_WRCH, 0, wr.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_WRCH, 0, wr.msi_msgdata);
	off += scnprintf(
		strbuf + off, size - off,
		"----------------------------------------------------\n");
	off += SHOW_HDMA_SW_REG32(EN_OFF_RDCH, 0, rd.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_RDCH, 0, rd.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_RDCH, 0, rd.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_RDCH, 0, rd.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_RDCH, 0, rd.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_RDCH, 0, rd.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_RDCH, 0, rd.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_RDCH, 0, rd.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_RDCH, 0, rd.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_RDCH, 0, rd.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_RDCH, 0, rd.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_RDCH, 0, rd.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_RDCH, 0, rd.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_RDCH, 0, rd.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_RDCH, 0, rd.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_RDCH, 0, rd.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_RDCH, 0, rd.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_RDCH, 0, rd.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_RDCH, 0, rd.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_RDCH, 0, rd.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_RDCH, 0, rd.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_RDCH, 0, rd.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_RDCH, 0,
				  rd.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_RDCH, 0,
				  rd.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_RDCH, 0, rd.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_RDCH, 0, rd.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_RDCH, 0, rd.msi_msgdata);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_hdma_debugfs_dbg_regs0_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_dbg_regs_read_0,
};

static ssize_t axl_aipu_hdma_debugfs_dbg_regs_read_1(struct file *file,
						     char __user *user_buf,
						     size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += SHOW_HDMA_SW_REG32(EN_OFF_WRCH, 1, wr.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_WRCH, 1, wr.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_WRCH, 1, wr.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_WRCH, 1, wr.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_WRCH, 1, wr.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_WRCH, 1, wr.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_WRCH, 1, wr.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_WRCH, 1, wr.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_WRCH, 1, wr.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_WRCH, 1, wr.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_WRCH, 1, wr.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_WRCH, 1, wr.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_WRCH, 1, wr.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_WRCH, 1, wr.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_WRCH, 1, wr.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_WRCH, 1, wr.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_WRCH, 1, wr.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_WRCH, 1, wr.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_WRCH, 1, wr.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_WRCH, 1, wr.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_WRCH, 1, wr.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_WRCH, 1, wr.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_WRCH, 1,
				  wr.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_WRCH, 1,
				  wr.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_WRCH, 1, wr.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_WRCH, 1, wr.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_WRCH, 1, wr.msi_msgdata);
	off += scnprintf(
		strbuf + off, size - off,
		"----------------------------------------------------\n");
	off += SHOW_HDMA_SW_REG32(EN_OFF_RDCH, 1, rd.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_RDCH, 1, rd.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_RDCH, 1, rd.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_RDCH, 1, rd.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_RDCH, 1, rd.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_RDCH, 1, rd.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_RDCH, 1, rd.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_RDCH, 1, rd.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_RDCH, 1, rd.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_RDCH, 1, rd.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_RDCH, 1, rd.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_RDCH, 1, rd.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_RDCH, 1, rd.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_RDCH, 1, rd.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_RDCH, 1, rd.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_RDCH, 1, rd.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_RDCH, 1, rd.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_RDCH, 1, rd.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_RDCH, 1, rd.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_RDCH, 1, rd.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_RDCH, 1, rd.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_RDCH, 1, rd.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_RDCH, 1,
				  rd.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_RDCH, 1,
				  rd.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_RDCH, 1, rd.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_RDCH, 1, rd.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_RDCH, 1, rd.msi_msgdata);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_hdma_debugfs_dbg_regs1_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_dbg_regs_read_1,
};

static ssize_t axl_aipu_hdma_debugfs_dbg_regs_read_2(struct file *file,
						     char __user *user_buf,
						     size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += SHOW_HDMA_SW_REG32(EN_OFF_WRCH, 2, wr.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_WRCH, 2, wr.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_WRCH, 2, wr.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_WRCH, 2, wr.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_WRCH, 2, wr.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_WRCH, 2, wr.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_WRCH, 2, wr.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_WRCH, 2, wr.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_WRCH, 2, wr.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_WRCH, 2, wr.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_WRCH, 2, wr.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_WRCH, 2, wr.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_WRCH, 2, wr.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_WRCH, 2, wr.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_WRCH, 2, wr.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_WRCH, 2, wr.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_WRCH, 2, wr.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_WRCH, 2, wr.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_WRCH, 2, wr.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_WRCH, 2, wr.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_WRCH, 2, wr.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_WRCH, 2, wr.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_WRCH, 2,
				  wr.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_WRCH, 2,
				  wr.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_WRCH, 2, wr.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_WRCH, 2, wr.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_WRCH, 2, wr.msi_msgdata);
	off += scnprintf(
		strbuf + off, size - off,
		"----------------------------------------------------\n");
	off += SHOW_HDMA_SW_REG32(EN_OFF_RDCH, 2, rd.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_RDCH, 2, rd.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_RDCH, 2, rd.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_RDCH, 2, rd.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_RDCH, 2, rd.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_RDCH, 2, rd.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_RDCH, 2, rd.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_RDCH, 2, rd.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_RDCH, 2, rd.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_RDCH, 2, rd.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_RDCH, 2, rd.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_RDCH, 2, rd.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_RDCH, 2, rd.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_RDCH, 2, rd.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_RDCH, 2, rd.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_RDCH, 2, rd.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_RDCH, 2, rd.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_RDCH, 2, rd.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_RDCH, 2, rd.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_RDCH, 2, rd.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_RDCH, 2, rd.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_RDCH, 2, rd.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_RDCH, 2,
				  rd.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_RDCH, 2,
				  rd.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_RDCH, 2, rd.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_RDCH, 2, rd.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_RDCH, 2, rd.msi_msgdata);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_hdma_debugfs_dbg_regs2_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_dbg_regs_read_2,
};

static ssize_t axl_aipu_hdma_debugfs_dbg_regs_read_3(struct file *file,
						     char __user *user_buf,
						     size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += SHOW_HDMA_SW_REG32(EN_OFF_WRCH, 3, wr.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_WRCH, 3, wr.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_WRCH, 3, wr.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_WRCH, 3, wr.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_WRCH, 3, wr.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_WRCH, 3, wr.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_WRCH, 3, wr.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_WRCH, 3, wr.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_WRCH, 3, wr.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_WRCH, 3, wr.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_WRCH, 3, wr.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_WRCH, 3, wr.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_WRCH, 3, wr.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_WRCH, 3, wr.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_WRCH, 3, wr.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_WRCH, 3, wr.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_WRCH, 3, wr.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_WRCH, 3, wr.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_WRCH, 3, wr.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_WRCH, 3, wr.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_WRCH, 3, wr.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_WRCH, 3, wr.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_WRCH, 3,
				  wr.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_WRCH, 3,
				  wr.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_WRCH, 3, wr.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_WRCH, 3, wr.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_WRCH, 3, wr.msi_msgdata);
	off += scnprintf(
		strbuf + off, size - off,
		"----------------------------------------------------\n");
	off += SHOW_HDMA_SW_REG32(EN_OFF_RDCH, 3, rd.ch_en);
	off += SHOW_HDMA_SW_REG32(DOORBELL_OFF_RDCH, 3, rd.doorbell);
	off += SHOW_HDMA_SW_REG32(ELEM_PF_OFF_RDCH, 3, rd.prefetch);
	off += SHOW_HDMA_SW_REG32(HANDSHAKE_OFF_RDCH, 3, rd.handshake);
	off += SHOW_HDMA_SW_REG32(LLP_LOW_OFF_RDCH, 3, rd.llp.lsb);
	off += SHOW_HDMA_SW_REG32(LLP_HIGH_OFF_RDCH, 3, rd.llp.msb);
	off += SHOW_HDMA_SW_REG32(CYCLE_OFF_RDCH, 3, rd.cycle_sync);
	off += SHOW_HDMA_SW_REG32(XFERSIZE_OFF_RDCH, 3, rd.transfer_size);
	off += SHOW_HDMA_SW_REG32(SAR_LOW_OFF_RDCH, 3, rd.sar.lsb);
	off += SHOW_HDMA_SW_REG32(SAR_HIGH_OFF_RDCH, 3, rd.sar.msb);
	off += SHOW_HDMA_SW_REG32(DAR_LOW_OFF_RDCH, 3, rd.dar.lsb);
	off += SHOW_HDMA_SW_REG32(DAR_HIGH_OFF_RDCH, 3, rd.dar.msb);
	off += SHOW_HDMA_SW_REG32(WATERMARK_EN_OFF_RDCH, 3, rd.watermark_en);
	off += SHOW_HDMA_SW_REG32(CONTROL1_OFF_RDCH, 3, rd.control1);
	off += SHOW_HDMA_SW_REG32(FUNC_NUM_OFF_RDCH, 3, rd.func_num);
	off += SHOW_HDMA_SW_REG32(QOS_OFF_RDCH, 3, rd.qos);
	off += SHOW_HDMA_SW_REG32(STATUS_OFF_RDCH, 3, rd.ch_stat);
	off += SHOW_HDMA_SW_REG32(INT_STATUS_OFF_RDCH, 3, rd.int_stat);
	off += SHOW_HDMA_SW_REG32(INT_SETUP_OFF_RDCH, 3, rd.int_setup);
	off += SHOW_HDMA_SW_REG32(INT_CLEAR_OFF_RDCH, 3, rd.int_clear);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_LOW_OFF_RDCH, 3, rd.msi_stop.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_STOP_HIGH_OFF_RDCH, 3, rd.msi_stop.msb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_LOW_OFF_RDCH, 3,
				  rd.msi_watermark.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_WATERMARK_HIGH_OFF_RDCH, 3,
				  rd.msi_watermark.msb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_LOW_OFF_RDCH, 3, rd.msi_abort.lsb);
	off += SHOW_HDMA_SW_REG32(MSI_ABORT_HIGH_OFF_RDCH, 3, rd.msi_abort.msb);
	off += SHOW_HDMA_SW_REG32(MSI_MSGD_OFF_RDCH, 3, rd.msi_msgdata);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_hdma_debugfs_dbg_regs3_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_dbg_regs_read_3,
};

static ssize_t
axl_aipu_hdma_debugfs_dbg_rd_chx_ll_write(struct file *file,
					  const char __user *ubuf, size_t size,
					  loff_t *offp, int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_lli *lli;
	volatile struct dw_hdma_ll_buf *lldch;

	lldch = (volatile struct dw_hdma_ll_buf *)(axldev->vl2base +
						   axldev->desc_offset);
	lli = (volatile struct dw_hdma_v0_lli *)__get_ll_base(
		lldch, DW_HDMA_DIR_WRITE, channel);
	lli->control = 0;
	dev_dbg(&axldev->pdev->dev, "Linked List Pointer reset\n");
	return size;
}

static ssize_t axl_aipu_hdma_debugfs_dbg_rd_chx_ll_read(struct file *file,
							char __user *user_buf,
							size_t count,
							loff_t *ppos,
							int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	volatile struct dw_hdma_ll_buf *hwlldch;
	volatile struct dw_hdma_ll_buf *lldch;
	volatile struct dw_hdma_v0_lli *lli;
	char *strbuf;
	size_t size, ret, off = 0;
	int i = 0;

	size = max_t(size_t, count, 0x2000U);

	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	if (channel >= HDMA_V0_MAX_NR_CH) {
		off = scnprintf(strbuf + off, size - off,
				"Wrong channel %d [ 0- 3 ]\n", channel);
		goto out_rd_chx_ll;
	}
	off = scnprintf(strbuf + off, size - off,
			"\n\n========= RD Channel %d ================\n",
			channel);
	off += scnprintf(strbuf + off, size - off, "llp         : 0x%08x%08x\n",
			 hdma->ch[channel].rd.llp.msb,
			 hdma->ch[channel].rd.llp.lsb);
	if (hdma->ch[channel].rd.llp.msb == 0 &&
	    hdma->ch[channel].rd.llp.lsb == 0) {
		off = scnprintf(strbuf + off, size - off,
				"Linked List Pointer not configured\n");
		goto out_rd_chx_ll;
	}

	hwlldch =
		(struct dw_hdma_ll_buf *)((uint64_t)hdma->ch[channel].rd.llp.msb
						  << 32 |
					  hdma->ch[channel].rd.llp.lsb);
	lldch = (volatile struct dw_hdma_ll_buf *)(axldev->vl2base +
						   axldev->desc_offset);
	lli = (volatile struct dw_hdma_v0_lli *)__get_ll_base(
		lldch, DW_HDMA_DIR_READ, channel);

	for (i = 0; (lli->control & (DW_HDMA_V0_CB)) && i < DW_HDMA_LL_MAX_NUM;
	     i++, lli++) {
		off += scnprintf(strbuf + off, size - off,
				 "Linked-list index %d\n", i);
		off += scnprintf(
			strbuf + off, size - off,
			"control       : 0x%016llx 0x%08x\n",
			(uint64_t)(&hwlldch->ch[channel].rd[i].control),
			lli->control);
		if (lli->control & DW_HDMA_V0_LLP) {
			off += scnprintf(
				strbuf + off, size - off,
				"llp           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].rd[i].sar.reg,
				lli->sar.reg);
		} else {
			off += scnprintf(strbuf + off, size - off,
					 "transfer_size : 0x%016llx 0x%08x\n",
					 (uint64_t)&hwlldch->ch[channel]
						 .rd[i]
						 .transfer_size,
					 lli->transfer_size);
			off += scnprintf(
				strbuf + off, size - off,
				"sar           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].rd[i].sar.reg,
				lli->sar.reg);
			off += scnprintf(
				strbuf + off, size - off,
				"dar           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].rd[i].dar.reg,
				lli->dar.reg);
		}
	}

out_rd_chx_ll:
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

#define TR_DBFS_RD_CHX_LL(_channel)                                                   \
	static ssize_t axl_aipu_hdma_debugfs_dbg_rd_ch##_channel##_ll_write(          \
		struct file *file, const char __user *user_buf, size_t count,         \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_hdma_debugfs_dbg_rd_chx_ll_write(                     \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static ssize_t axl_aipu_hdma_debugfs_dbg_rd_ch##_channel##_ll_read(           \
		struct file *file, char __user *user_buf, size_t count,               \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_hdma_debugfs_dbg_rd_chx_ll_read(                      \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static const struct file_operations                                           \
		axl_aipu_hdma_debugfs_dbg_rd_ch##_channel##_ll_fops = {               \
			.owner = THIS_MODULE,                                         \
			.open = simple_open,                                          \
			.read = axl_aipu_hdma_debugfs_dbg_rd_ch##_channel##_ll_read,  \
			.write =                                                      \
				axl_aipu_hdma_debugfs_dbg_rd_ch##_channel##_ll_write, \
		}
TR_DBFS_RD_CHX_LL(0);
TR_DBFS_RD_CHX_LL(1);
TR_DBFS_RD_CHX_LL(2);
TR_DBFS_RD_CHX_LL(3);

static ssize_t
axl_aipu_hdma_debugfs_dbg_wr_chx_ll_write(struct file *file,
					  const char __user *ubuf, size_t size,
					  loff_t *offp, int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	volatile struct dw_hdma_v0_lli *lli;
	volatile struct dw_hdma_ll_buf *lldch;

	if (hdma->ch[channel].wr.llp.msb == 0 &&
	    hdma->ch[channel].wr.llp.lsb == 0) {
		dev_err(&axldev->pdev->dev,
			"Linked List Pointer not configured\n");
		return size;
	}
	lldch = (volatile struct dw_hdma_ll_buf *)(axldev->vl2base +
						   axldev->desc_offset);
	lli = (volatile struct dw_hdma_v0_lli *)__get_ll_base(
		lldch, DW_HDMA_DIR_WRITE, channel);
	lli->control = 0;
	dev_dbg(&axldev->pdev->dev, "Linked List Pointer reset\n");
	return size;
}

static ssize_t axl_aipu_hdma_debugfs_dbg_wr_chx_ll_read(struct file *file,
							char __user *user_buf,
							size_t count,
							loff_t *ppos,
							int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	volatile struct dw_hdma_ll_buf *hwlldch;
	volatile struct dw_hdma_ll_buf *lldch;
	volatile struct dw_hdma_v0_lli *lli;
	char *strbuf;
	size_t size, ret, off = 0;
	int i = 0;

	size = max_t(size_t, count, 0x2000U);
	dev_dbg(&axldev->pdev->dev, "WR:channel %d count %ld size %ld\n",
		channel, count, size);

	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	if (channel >= EDMA_V0_MAX_NR_CH) {
		off = scnprintf(strbuf + off, size - off,
				"Wrong channel %d [ 0- 3 ]\n", channel);
		goto out_wr_chx_ll;
	}
	off = scnprintf(strbuf + off, size - off,
			"\n\n========= WR Channel %d ================\n",
			channel);
	off += scnprintf(strbuf + off, size - off, "llp         : 0x%08x%08x\n",
			 hdma->ch[channel].wr.llp.msb,
			 hdma->ch[channel].wr.llp.lsb);
	if (hdma->ch[channel].wr.llp.msb == 0 &&
	    hdma->ch[channel].wr.llp.lsb == 0) {
		off = scnprintf(strbuf + off, size - off,
				"Linked List Pointer not configured\n");
		goto out_wr_chx_ll;
	}

	hwlldch =
		(struct dw_hdma_ll_buf *)((uint64_t)hdma->ch[channel].wr.llp.msb
						  << 32 |
					  hdma->ch[channel].wr.llp.lsb);
	lldch = (volatile struct dw_hdma_ll_buf *)(axldev->vl2base +
						   axldev->desc_offset);
	lli = (volatile struct dw_hdma_v0_lli *)__get_ll_base(
		lldch, DW_HDMA_DIR_WRITE, channel);

	for (i = 0; (lli->control & (DW_HDMA_V0_CB)) && i < DW_HDMA_LL_MAX_NUM;
	     i++, lli++) {
		off += scnprintf(strbuf + off, size - off,
				 "Linked-list index %d\n", i);
		off += scnprintf(
			strbuf + off, size - off,
			"control       : 0x%016llx 0x%08x\n",
			(uint64_t)(&hwlldch->ch[channel].wr[i].control),
			lli->control);
		if (lli->control & DW_HDMA_V0_LLP) {
			off += scnprintf(
				strbuf + off, size - off,
				"llp           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].wr[i].sar.reg,
				lli->sar.reg);
		} else {
			off += scnprintf(strbuf + off, size - off,
					 "transfer_size : 0x%016llx 0x%08x\n",
					 (uint64_t)&hwlldch->ch[channel]
						 .wr[i]
						 .transfer_size,
					 lli->transfer_size);
			off += scnprintf(
				strbuf + off, size - off,
				"sar           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].wr[i].sar.reg,
				lli->sar.reg);
			off += scnprintf(
				strbuf + off, size - off,
				"dar           : 0x%016llx 0x%016llx\n",
				(uint64_t)&hwlldch->ch[channel].wr[i].dar.reg,
				lli->dar.reg);
		}
	}

out_wr_chx_ll:
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

#define TR_DBFS_WR_CHX_LL(_channel)                                                   \
	static ssize_t axl_aipu_hdma_debugfs_dbg_wr_ch##_channel##_ll_write(          \
		struct file *file, const char __user *user_buf, size_t count,         \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_hdma_debugfs_dbg_wr_chx_ll_write(                     \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static ssize_t axl_aipu_hdma_debugfs_dbg_wr_ch##_channel##_ll_read(           \
		struct file *file, char __user *user_buf, size_t count,               \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_hdma_debugfs_dbg_wr_chx_ll_read(                      \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static const struct file_operations                                           \
		axl_aipu_hdma_debugfs_dbg_wr_ch##_channel##_ll_fops = {               \
			.owner = THIS_MODULE,                                         \
			.open = simple_open,                                          \
			.read = axl_aipu_hdma_debugfs_dbg_wr_ch##_channel##_ll_read,  \
			.write =                                                      \
				axl_aipu_hdma_debugfs_dbg_wr_ch##_channel##_ll_write, \
		}
TR_DBFS_WR_CHX_LL(0);
TR_DBFS_WR_CHX_LL(1);
TR_DBFS_WR_CHX_LL(2);
TR_DBFS_WR_CHX_LL(3);

static ssize_t axl_aipu_hdma_debugfs_dma_stat_read(struct file *file,
						   char __user *user_buf,
						   size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	struct dma_queue_ctrl *dma_ctrl;
	int i, dcount, num_xfer, num_err, max_sgt;
	u32 speed;
	__u64 bytes_xfer;
	char *strbuf;
	size_t size, ret, off = 0, trsize;
	ktime_t max_duration, duration;

	size = max_t(size_t, count, 0x2000U);
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off = scnprintf(strbuf + off, size - off, "\nDMA Statistics\n");
	for (i = 0; i < EDMA_V0_MAX_NR_CH; i++) {
		dma_ctrl = &axldev->dma_wrqc[i];
		dcount = atomic_read(&dma_ctrl->count);
		num_xfer = dma_ctrl->num_xfer;
		num_err = dma_ctrl->num_err;
		bytes_xfer = dma_ctrl->bytes_xfer;
		max_sgt = dma_ctrl->max_sgt;
		max_duration = dma_ctrl->max_duration;
		duration = dma_ctrl->duration;
		trsize = dma_ctrl->size;
		speed = trsize / (ktime_to_us(duration) == 0 ?
					  1 :
					  ktime_to_us(duration));
		off += scnprintf(strbuf + off, size - off,
				 "\n========= RD Channel %d ================\n",
				 i);
		off += scnprintf(strbuf + off, size - off, "count : %d\n",
				 dcount);
		off += scnprintf(strbuf + off, size - off, "request : %d\n",
				 num_xfer);
		off += scnprintf(strbuf + off, size - off, "errors : %d\n",
				 num_err);
		off += scnprintf(strbuf + off, size - off, "bytes : %llu\n",
				 bytes_xfer);
		off += scnprintf(strbuf + off, size - off, "max sgt : %d\n",
				 max_sgt);
		off += scnprintf(strbuf + off, size - off,
				 "max max_duration : %lldus\n",
				 ktime_to_us(max_duration));
		off += scnprintf(strbuf + off, size - off,
				 "duration : %lld us\n", ktime_to_us(duration));
		off += scnprintf(strbuf + off, size - off, "size : %lu Bytes\n",
				 trsize);
		off += scnprintf(strbuf + off, size - off, "speed : %u MB/s\n",
				 speed);

		dma_ctrl = &axldev->dma_rdqc[i];
		dcount = atomic_read(&dma_ctrl->count);
		num_xfer = dma_ctrl->num_xfer;
		num_err = dma_ctrl->num_err;
		bytes_xfer = dma_ctrl->bytes_xfer;
		max_sgt = dma_ctrl->max_sgt;
		max_duration = dma_ctrl->max_duration;
		duration = dma_ctrl->duration;
		trsize = dma_ctrl->size;
		speed = trsize / (ktime_to_us(duration) == 0 ?
					  1 :
					  ktime_to_us(duration));
		off += scnprintf(strbuf + off, size - off,
				 "\n========= WR Channel %d ================\n",
				 i);
		off += scnprintf(strbuf + off, size - off, "count : %d\n",
				 dcount);
		off += scnprintf(strbuf + off, size - off, "request : %d\n",
				 num_xfer);
		off += scnprintf(strbuf + off, size - off, "errors : %d\n",
				 num_err);
		off += scnprintf(strbuf + off, size - off, "bytes : %llu\n",
				 bytes_xfer);
		off += scnprintf(strbuf + off, size - off, "max sgt : %d\n",
				 max_sgt);
		off += scnprintf(strbuf + off, size - off,
				 "max duration : %lldus\n",
				 ktime_to_us(max_duration));
		off += scnprintf(strbuf + off, size - off,
				 "duration : %lld us\n", ktime_to_us(duration));
		off += scnprintf(strbuf + off, size - off, "size : %lu Bytes\n",
				 trsize);
		off += scnprintf(strbuf + off, size - off, "speed : %u MB/s\n",
				 speed);
	}
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}
static ssize_t axl_aipu_hdma_debugfs_dma_stat_write(struct file *file,
						    const char __user *user_buf,
						    size_t size, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	int ret, i, val;

	ret = kstrtoint_from_user(user_buf, size, 0, &val);
	if (ret)
		return ret;

	switch (val) {
	case 1:
		for (i = 0; i < EDMA_V0_MAX_NR_CH; i++) {
			axldev->dma_wrqc[i].num_xfer = 0;
			axldev->dma_wrqc[i].num_err = 0;
			axldev->dma_wrqc[i].bytes_xfer = 0;
			axldev->dma_wrqc[i].max_sgt = 0;
			axldev->dma_wrqc[i].max_duration = 0;
			axldev->dma_wrqc[i].duration = 0;
			axldev->dma_wrqc[i].size = 0;

			axldev->dma_rdqc[i].num_xfer = 0;
			axldev->dma_rdqc[i].num_err = 0;
			axldev->dma_rdqc[i].bytes_xfer = 0;
			axldev->dma_rdqc[i].max_sgt = 0;
			axldev->dma_rdqc[i].max_duration = 0;
			axldev->dma_rdqc[i].duration = 0;
			axldev->dma_rdqc[i].size = 0;
		}
		dev_dbg(&axldev->pdev->dev, "DMA Statistics reset\n");
		break;
	case 2:
		for (i = 0; i < EDMA_V0_MAX_NR_CH; i++) {
			axldev->dma_wrqc[i].max_duration = 0;
			axldev->dma_wrqc[i].duration = 0;
			axldev->dma_wrqc[i].size = 0;

			axldev->dma_rdqc[i].max_duration = 0;
			axldev->dma_rdqc[i].duration = 0;
			axldev->dma_rdqc[i].size = 0;
		}
		break;
	default:
		dev_info(
			&axldev->pdev->dev,
			"Supported options: 1 reset all, 2 reset max duration\n");
	}

	return size;
}

static const struct file_operations axl_aipu_hdma_debugfs_dma_stat_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_hdma_debugfs_dma_stat_read,
	.write = axl_aipu_hdma_debugfs_dma_stat_write,
};

void axl_aipu_hdma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev)
{
	struct dentry *dentry;

	if (!axldev->dentry)
		return;
	dentry = axldev->dentry;

	dev_info(&axldev->pdev->dev, "Register directory %s-%s\n",
		 axldev->dev_info->devname, dev_name(&axldev->pdev->dev));
	debugfs_create_file("info", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_info_fops);
	debugfs_create_file("dma-regs-ch0", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_regs0_fops);
	debugfs_create_file("dma-regs-ch1", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_regs1_fops);
	debugfs_create_file("dma-regs-ch2", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_regs2_fops);
	debugfs_create_file("dma-regs-ch3", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_regs3_fops);
	debugfs_create_file("dma-rd-ch0-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_rd_ch0_ll_fops);
	debugfs_create_file("dma-rd-ch1-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_rd_ch1_ll_fops);
	debugfs_create_file("dma-rd-ch2-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_rd_ch2_ll_fops);
	debugfs_create_file("dma-rd-ch3-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_rd_ch3_ll_fops);
	debugfs_create_file("dma-wr-ch0-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_wr_ch0_ll_fops);
	debugfs_create_file("dma-wr-ch1-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_wr_ch1_ll_fops);
	debugfs_create_file("dma-wr-ch2-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_wr_ch2_ll_fops);
	debugfs_create_file("dma-wr-ch3-ll", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dbg_wr_ch3_ll_fops);
	debugfs_create_file("dma-statistics", 0444, dentry, axldev,
			    &axl_aipu_hdma_debugfs_dma_stat_fops);
}

void axl_aipu_hdma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->dentry) {
		debugfs_remove_recursive(axldev->dentry);
		dev_info(&axldev->pdev->dev, "Unregister directory %s\n",
			 dev_name(&axldev->pdev->dev));
	}
}
