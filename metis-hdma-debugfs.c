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

#include "metis-dmabuf.h"
#include "metis.h"
#include "metis-pcie-hdma.h"
#include "metis-hdma-core.h"
#include "metis-version.h"

extern struct dentry *axlaipu_debugfs_root;

static ssize_t hdma_debugfs_info_read(struct file *file, char __user *user_buf,
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

static const struct file_operations hdma_debugfs_info_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hdma_debugfs_info_read,
};

#define SHOW_HDMA_SW_REG32(offset, channel, reg)                     \
	scnprintf(strbuf + off, size - off, "%40s  0x%08x 0x%08x\n", \
		  __stringify(HDMA_##offset##_##channel),            \
		  HDMA_##offset##_##channel, hdma->ch[channel].reg);

static ssize_t hdma_debugfs_dbg_regs_read_0(struct file *file,
					    char __user *user_buf, size_t count,
					    loff_t *ppos)
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

static const struct file_operations hdma_debugfs_dbg_regs0_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hdma_debugfs_dbg_regs_read_0,
};

static ssize_t hdma_debugfs_dbg_regs_read_1(struct file *file,
					    char __user *user_buf, size_t count,
					    loff_t *ppos)
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

static const struct file_operations hdma_debugfs_dbg_regs1_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hdma_debugfs_dbg_regs_read_1,
};

static ssize_t hdma_debugfs_dbg_regs_read_2(struct file *file,
					    char __user *user_buf, size_t count,
					    loff_t *ppos)
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

static const struct file_operations hdma_debugfs_dbg_regs2_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hdma_debugfs_dbg_regs_read_2,
};

static ssize_t hdma_debugfs_dbg_regs_read_3(struct file *file,
					    char __user *user_buf, size_t count,
					    loff_t *ppos)
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

static const struct file_operations hdma_debugfs_dbg_regs3_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hdma_debugfs_dbg_regs_read_3,
};

void hdma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	struct dentry *dentry;
	char name[NAME_SIZE];

	if (!axlaipu_debugfs_root)
		return;

	snprintf(name, NAME_SIZE, "%s-%s", axldev->dev_info->devname,
		 dev_name(&pdev->dev));
	dentry = debugfs_create_dir(name, axlaipu_debugfs_root);
	if (IS_ERR(dentry)) {
		dev_err(&pdev->dev, "Failed to create debugfs directory %s\n",
			name);
		return;
	}
	axldev->dentry = dentry;
	dev_info(&pdev->dev, "Register directory %s\n", name);
	debugfs_create_file("info", 0444, dentry, axldev,
			    &hdma_debugfs_info_fops);
	debugfs_create_file("dma-regs-ch0", 0444, dentry, axldev,
			    &hdma_debugfs_dbg_regs0_fops);
	debugfs_create_file("dma-regs-ch1", 0444, dentry, axldev,
			    &hdma_debugfs_dbg_regs1_fops);
	debugfs_create_file("dma-regs-ch2", 0444, dentry, axldev,
			    &hdma_debugfs_dbg_regs2_fops);
	debugfs_create_file("dma-regs-ch3", 0444, dentry, axldev,
			    &hdma_debugfs_dbg_regs3_fops);
}

void hdma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->dentry) {
		debugfs_remove_recursive(axldev->dentry);
		dev_info(&axldev->pdev->dev, "Unregister directory %s\n",
			 dev_name(&axldev->pdev->dev));
	}
}
