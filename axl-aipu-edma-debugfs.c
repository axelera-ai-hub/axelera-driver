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
#include <linux/uio_driver.h>
#include <linux/dma-mapping.h>
#include <linux/aer.h>
#include <linux/msi.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/poll.h>
#include <linux/circ_buf.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-pcie-edma.h"
#include "axl-aipu-edma-core.h"
#include "axl-aipu-fwtrace.h"
#include "axl-aipu-version.h"

extern struct dentry *axl_aipu_debugfs_root;
static ssize_t axl_aipu_edma_debugfs_info_read(struct file *file,
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
	for (i = 0; i < axldev->dev_info->aicore_count; i++) {
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

static const struct file_operations axl_aipu_edma_debugfs_info_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_edma_debugfs_info_read,
};

#define SHOW_EDMA_SW_CH32(offset, reg, channel)                      \
	scnprintf(strbuf + off, size - off, "%40s  0x%08x 0x%08x\n", \
		  __stringify(DMA_##offset##_##channel),             \
		  DMA_##offset##_##channel,                          \
		  edma->type.unroll.ch[channel].reg);

static ssize_t axl_aipu_edma_debugfs_dbg_ch_regs_read(struct file *file,
						      char __user *user_buf,
						      size_t count,
						      loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= WR Channel %d ================\n\n", 0);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_WRCH, wr.ch_control1, 0);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_WRCH, wr.transfer_size, 0);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_WRCH, wr.sar.lsb, 0);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_WRCH, wr.sar.msb, 0);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_WRCH, wr.dar.lsb, 0);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_WRCH, wr.dar.msb, 0);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_WRCH, wr.llp.lsb, 0);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_WRCH, wr.llp.msb, 0);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= RD Channel %d ================\n", 0);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_RDCH, rd.ch_control1, 0);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_RDCH, rd.transfer_size, 0);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_RDCH, rd.sar.lsb, 0);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_RDCH, rd.sar.msb, 0);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_RDCH, rd.dar.lsb, 0);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_RDCH, rd.dar.msb, 0);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_RDCH, rd.llp.lsb, 0);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_RDCH, rd.llp.msb, 0);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= WR Channel %d ================\n\n", 1);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_WRCH, wr.ch_control1, 1);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_WRCH, wr.transfer_size, 1);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_WRCH, wr.sar.lsb, 1);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_WRCH, wr.sar.msb, 1);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_WRCH, wr.dar.lsb, 1);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_WRCH, wr.dar.msb, 1);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_WRCH, wr.llp.lsb, 1);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_WRCH, wr.llp.msb, 1);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= RD Channel %d ================\n", 1);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_RDCH, rd.ch_control1, 1);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_RDCH, rd.transfer_size, 1);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_RDCH, rd.sar.lsb, 1);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_RDCH, rd.sar.msb, 1);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_RDCH, rd.dar.lsb, 1);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_RDCH, rd.dar.msb, 1);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_RDCH, rd.llp.lsb, 1);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_RDCH, rd.llp.msb, 1);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= WR Channel %d ================\n\n", 2);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_WRCH, wr.ch_control1, 2);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_WRCH, wr.transfer_size, 2);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_WRCH, wr.sar.lsb, 2);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_WRCH, wr.sar.msb, 2);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_WRCH, wr.dar.lsb, 2);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_WRCH, wr.dar.msb, 2);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_WRCH, wr.llp.lsb, 2);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_WRCH, wr.llp.msb, 2);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= RD Channel %d ================\n", 2);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_RDCH, rd.ch_control1, 2);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_RDCH, rd.transfer_size, 2);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_RDCH, rd.sar.lsb, 2);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_RDCH, rd.sar.msb, 2);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_RDCH, rd.dar.lsb, 2);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_RDCH, rd.dar.msb, 2);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_RDCH, rd.llp.lsb, 2);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_RDCH, rd.llp.msb, 2);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= WR Channel %d ================\n\n", 3);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_WRCH, wr.ch_control1, 3);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_WRCH, wr.transfer_size, 3);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_WRCH, wr.sar.lsb, 3);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_WRCH, wr.sar.msb, 3);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_WRCH, wr.dar.lsb, 3);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_WRCH, wr.dar.msb, 3);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_WRCH, wr.llp.lsb, 3);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_WRCH, wr.llp.msb, 3);

	off += scnprintf(strbuf + off, size - off,
			 "\n\n========= RD Channel %d ================\n", 3);
	off += SHOW_EDMA_SW_CH32(CH_CONTROL1_OFF_RDCH, rd.ch_control1, 3);
	off += SHOW_EDMA_SW_CH32(TRANSFER_SIZE_OFF_RDCH, rd.transfer_size, 3);
	off += SHOW_EDMA_SW_CH32(SAR_LOW_OFF_RDCH, rd.sar.lsb, 3);
	off += SHOW_EDMA_SW_CH32(SAR_HIGH_OFF_RDCH, rd.sar.msb, 3);
	off += SHOW_EDMA_SW_CH32(DAR_LOW_OFF_RDCH, rd.dar.lsb, 3);
	off += SHOW_EDMA_SW_CH32(DAR_HIGH_OFF_RDCH, rd.dar.msb, 3);
	off += SHOW_EDMA_SW_CH32(LLP_LOW_OFF_RDCH, rd.llp.lsb, 3);
	off += SHOW_EDMA_SW_CH32(LLP_HIGH_OFF_RDCH, rd.llp.msb, 3);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_edma_debugfs_dbg_ch_regs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_edma_debugfs_dbg_ch_regs_read,
};

#define SHOW_EDMA_SW_REG32(offset, reg)                                \
	scnprintf(strbuf + off, size - off, "%40s  0x%08x 0x%08x\n",   \
		  __stringify(DMA_##offset##_OFF), DMA_##offset##_OFF, \
		  edma->reg);

static ssize_t axl_aipu_edma_debugfs_dbg_regs_read(struct file *file,
						   char __user *user_buf,
						   size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	char *strbuf;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = max_t(size_t, count, 0x2000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	off += SHOW_EDMA_SW_REG32(CTRL_DATA_ARB_PRIOR, ctrl_data_arb_prior);
	off += SHOW_EDMA_SW_REG32(CTRL, ctrl);
	off += SHOW_EDMA_SW_REG32(WRITE_ENGINE_EN, wr_engine_en);
	off += SHOW_EDMA_SW_REG32(WRITE_DOORBELL, wr_doorbell);
	off += SHOW_EDMA_SW_REG32(WRITE_CHANNEL_ARB_WEIGHT_LOW,
				  wr_ch_arb_weight.lsb);
	off += SHOW_EDMA_SW_REG32(WRITE_CHANNEL_ARB_WEIGHT_HIGH,
				  wr_ch_arb_weight.msb);
	off += SHOW_EDMA_SW_REG32(READ_ENGINE_EN, rd_engine_en);
	off += SHOW_EDMA_SW_REG32(READ_DOORBELL, rd_doorbell);
	off += SHOW_EDMA_SW_REG32(READ_CHANNEL_ARB_WEIGHT_LOW,
				  rd_ch_arb_weight.lsb);
	off += SHOW_EDMA_SW_REG32(READ_CHANNEL_ARB_WEIGHT_HIGH,
				  rd_ch_arb_weight.msb);
	off += SHOW_EDMA_SW_REG32(WRITE_INT_STATUS, wr_int_status);
	off += SHOW_EDMA_SW_REG32(WRITE_INT_MASK, wr_int_mask);
	off += SHOW_EDMA_SW_REG32(WRITE_INT_CLEAR, wr_int_clear);
	off += SHOW_EDMA_SW_REG32(WRITE_ERR_STATUS, wr_err_status);
	off += SHOW_EDMA_SW_REG32(WRITE_DONE_IMWR_LOW, wr_done_imwr.lsb);
	off += SHOW_EDMA_SW_REG32(WRITE_DONE_IMWR_HIGH, wr_done_imwr.msb);
	off += SHOW_EDMA_SW_REG32(WRITE_ABORT_IMWR_LOW, wr_abort_imwr.lsb);
	off += SHOW_EDMA_SW_REG32(WRITE_ABORT_IMWR_HIGH, wr_abort_imwr.msb);
	off += SHOW_EDMA_SW_REG32(WRITE_CH01_IMWR_DATA, wr_ch01_imwr_data);
	off += SHOW_EDMA_SW_REG32(WRITE_CH23_IMWR_DATA, wr_ch23_imwr_data);
	off += SHOW_EDMA_SW_REG32(WRITE_CH45_IMWR_DATA, wr_ch45_imwr_data);
	off += SHOW_EDMA_SW_REG32(WRITE_CH67_IMWR_DATA, wr_ch67_imwr_data);
	off += SHOW_EDMA_SW_REG32(WRITE_LINKED_LIST_ERR_EN,
				  wr_linked_list_err_en);
	off += SHOW_EDMA_SW_REG32(READ_INT_STATUS, rd_int_status);
	off += SHOW_EDMA_SW_REG32(READ_INT_MASK, rd_int_mask);
	off += SHOW_EDMA_SW_REG32(READ_INT_CLEAR, rd_int_clear);
	off += SHOW_EDMA_SW_REG32(READ_ERR_STATUS_LOW, rd_err_status.lsb);
	off += SHOW_EDMA_SW_REG32(READ_ERR_STATUS_HIGH, rd_err_status.msb);
	off += SHOW_EDMA_SW_REG32(READ_LINKED_LIST_ERR_EN,
				  rd_linked_list_err_en);
	off += SHOW_EDMA_SW_REG32(READ_DONE_IMWR_LOW, rd_done_imwr.lsb);
	off += SHOW_EDMA_SW_REG32(READ_DONE_IMWR_HIGH, rd_done_imwr.msb);
	off += SHOW_EDMA_SW_REG32(READ_ABORT_IMWR_LOW, rd_abort_imwr.lsb);
	off += SHOW_EDMA_SW_REG32(READ_ABORT_IMWR_HIGH, rd_abort_imwr.msb);
	off += SHOW_EDMA_SW_REG32(READ_CH01_IMWR_DATA, rd_ch01_imwr_data);
	off += SHOW_EDMA_SW_REG32(READ_CH23_IMWR_DATA, rd_ch23_imwr_data);
	off += SHOW_EDMA_SW_REG32(READ_CH45_IMWR_DATA, rd_ch45_imwr_data);
	off += SHOW_EDMA_SW_REG32(READ_CH67_IMWR_DATA, rd_ch67_imwr_data);

	off += SHOW_EDMA_SW_REG32(WRITE_CH0_PWR_EN, type.unroll.wr_ch0_pwr_en);
	off += SHOW_EDMA_SW_REG32(WRITE_CH1_PWR_EN, type.unroll.wr_ch1_pwr_en);
	off += SHOW_EDMA_SW_REG32(WRITE_CH2_PWR_EN, type.unroll.wr_ch2_pwr_en);
	off += SHOW_EDMA_SW_REG32(WRITE_CH3_PWR_EN, type.unroll.wr_ch3_pwr_en);
	off += SHOW_EDMA_SW_REG32(READ_CH0_PWR_EN, type.unroll.rd_ch0_pwr_en);
	off += SHOW_EDMA_SW_REG32(READ_CH1_PWR_EN, type.unroll.rd_ch1_pwr_en);
	off += SHOW_EDMA_SW_REG32(READ_CH2_PWR_EN, type.unroll.rd_ch2_pwr_en);
	off += SHOW_EDMA_SW_REG32(READ_CH3_PWR_EN, type.unroll.rd_ch3_pwr_en);

	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

static const struct file_operations axl_aipu_edma_debugfs_dbg_regs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_edma_debugfs_dbg_regs_read,
};

static ssize_t
axl_aipu_edma_debugfs_dbg_rd_chx_ll_write(struct file *file,
					  const char __user *ubuf, size_t size,
					  loff_t *offp, int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_v0_lli *lli;

	if (edma->type.unroll.ch[channel].rd.llp.msb == 0 &&
	    edma->type.unroll.ch[channel].rd.llp.lsb == 0) {
		dev_err(&axldev->pdev->dev,
			"Linked List Pointer not configured\n");
		return size;
	}
	lli = axldev->vbase +
	      (((uint64_t)edma->type.unroll.ch[channel].rd.llp.msb << 32) +
	       edma->type.unroll.ch[channel].rd.llp.lsb) -
	      EDMA_L2_BASE;
	lli->control = 0;
	dev_dbg(&axldev->pdev->dev, "Linked List Pointer reset\n");
	return size;
}

static ssize_t axl_aipu_edma_debugfs_dbg_rd_chx_ll_read(struct file *file,
							char __user *user_buf,
							size_t count,
							loff_t *ppos,
							int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_ll_buf *hwlldch;
	volatile struct dw_edma_v0_lli *lli;
	char *strbuf;
	size_t size, ret, off = 0;
	int i = 0;

	size = max_t(size_t, count, 0x2000U);

	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	if (channel >= EDMA_V0_MAX_NR_CH) {
		off = scnprintf(strbuf + off, size - off,
				"Wrong channel %d [ 0- 3 ]\n", channel);
		goto out_rd_chx_ll;
	}
	off = scnprintf(strbuf + off, size - off,
			"\n\n========= RD Channel %d ================\n",
			channel);
	off += scnprintf(strbuf + off, size - off, "llp         : 0x%08x%08x\n",
			 edma->type.unroll.ch[channel].rd.llp.msb,
			 edma->type.unroll.ch[channel].rd.llp.lsb);
	if (edma->type.unroll.ch[channel].rd.llp.msb == 0 &&
	    edma->type.unroll.ch[channel].rd.llp.lsb == 0) {
		off = scnprintf(strbuf + off, size - off,
				"Linked List Pointer not configured\n");
		goto out_rd_chx_ll;
	}

	hwlldch = (struct dw_edma_ll_buf
			   *)((uint64_t)edma->type.unroll.ch[channel].rd.llp.msb
				      << 32 |
			      edma->type.unroll.ch[channel].rd.llp.lsb);
	lli = axldev->vbase +
	      (((uint64_t)edma->type.unroll.ch[channel].rd.llp.msb << 32) +
	       edma->type.unroll.ch[channel].rd.llp.lsb) -
	      EDMA_L2_BASE;

	for (i = 0; (lli->control & (DW_EDMA_V0_CB | DW_EDMA_V0_TCB)) &&
		    i < DW_EDMA_LL_NUM;
	     i++, lli++) {
		off += scnprintf(strbuf + off, size - off,
				 "Linked-list index %d\n", i);
		off += scnprintf(
			strbuf + off, size - off,
			"control       : 0x%016llx 0x%08x\n",
			(uint64_t)(&hwlldch->ch[channel].rd[i].control),
			lli->control);
		off += scnprintf(
			strbuf + off, size - off,
			"transfer_size : 0x%016llx 0x%08x\n",
			(uint64_t)&hwlldch->ch[channel].rd[i].transfer_size,
			lli->transfer_size);
		off += scnprintf(strbuf + off, size - off,
				 "sar           : 0x%016llx 0x%016llx\n",
				 (uint64_t)&hwlldch->ch[channel].rd[i].sar.reg,
				 lli->sar.reg);
		off += scnprintf(strbuf + off, size - off,
				 "dar           : 0x%016llx 0x%016llx\n",
				 (uint64_t)&hwlldch->ch[channel].rd[i].dar.reg,
				 lli->dar.reg);
	}

out_rd_chx_ll:
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

#define TR_DBFS_RD_CHX_LL(_channel)                                                   \
	static ssize_t axl_aipu_edma_debugfs_dbg_rd_ch##_channel##_ll_write(          \
		struct file *file, const char __user *user_buf, size_t count,         \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_edma_debugfs_dbg_rd_chx_ll_write(                     \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static ssize_t axl_aipu_edma_debugfs_dbg_rd_ch##_channel##_ll_read(           \
		struct file *file, char __user *user_buf, size_t count,               \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_edma_debugfs_dbg_rd_chx_ll_read(                      \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static const struct file_operations                                           \
		axl_aipu_edma_debugfs_dbg_rd_ch##_channel##_ll_fops = {               \
			.owner = THIS_MODULE,                                         \
			.open = simple_open,                                          \
			.read = axl_aipu_edma_debugfs_dbg_rd_ch##_channel##_ll_read,  \
			.write =                                                      \
				axl_aipu_edma_debugfs_dbg_rd_ch##_channel##_ll_write, \
		}
TR_DBFS_RD_CHX_LL(0);
TR_DBFS_RD_CHX_LL(1);
TR_DBFS_RD_CHX_LL(2);
TR_DBFS_RD_CHX_LL(3);

static ssize_t
axl_aipu_edma_debugfs_dbg_wr_chx_ll_write(struct file *file,
					  const char __user *ubuf, size_t size,
					  loff_t *offp, int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_v0_lli *lli;

	if (edma->type.unroll.ch[channel].wr.llp.msb == 0 &&
	    edma->type.unroll.ch[channel].wr.llp.lsb == 0) {
		dev_err(&axldev->pdev->dev,
			"Linked List Pointer not configured\n");
		return size;
	}
	lli = axldev->vbase +
	      (((uint64_t)edma->type.unroll.ch[channel].wr.llp.msb << 32) +
	       edma->type.unroll.ch[channel].wr.llp.lsb) -
	      EDMA_L2_BASE;
	lli->control = 0;
	dev_dbg(&axldev->pdev->dev, "Linked List Pointer reset\n");
	return size;
}

static ssize_t axl_aipu_edma_debugfs_dbg_wr_chx_ll_read(struct file *file,
							char __user *user_buf,
							size_t count,
							loff_t *ppos,
							int channel)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_ll_buf *hwlldch;
	volatile struct dw_edma_v0_lli *lli;
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
			 edma->type.unroll.ch[channel].wr.llp.msb,
			 edma->type.unroll.ch[channel].wr.llp.lsb);
	if (edma->type.unroll.ch[channel].wr.llp.msb == 0 &&
	    edma->type.unroll.ch[channel].wr.llp.lsb == 0) {
		off = scnprintf(strbuf + off, size - off,
				"Linked List Pointer not configured\n");
		goto out_wr_chx_ll;
	}

	hwlldch = (struct dw_edma_ll_buf
			   *)((uint64_t)edma->type.unroll.ch[channel].wr.llp.msb
				      << 32 |
			      edma->type.unroll.ch[channel].wr.llp.lsb);
	lli = axldev->vbase +
	      (((uint64_t)edma->type.unroll.ch[channel].wr.llp.msb << 32) +
	       edma->type.unroll.ch[channel].wr.llp.lsb) -
	      EDMA_L2_BASE;

	for (i = 0; (lli->control & (DW_EDMA_V0_CB | DW_EDMA_V0_TCB)) &&
		    i < DW_EDMA_LL_NUM;
	     i++, lli++) {
		off += scnprintf(strbuf + off, size - off,
				 "Linked-list index %d\n", i);
		off += scnprintf(
			strbuf + off, size - off,
			"control       : 0x%016llx 0x%08x\n",
			(uint64_t)(&hwlldch->ch[channel].wr[i].control),
			lli->control);
		off += scnprintf(
			strbuf + off, size - off,
			"transfer_size : 0x%016llx 0x%08x\n",
			(uint64_t)&hwlldch->ch[channel].wr[i].transfer_size,
			lli->transfer_size);
		off += scnprintf(strbuf + off, size - off,
				 "sar           : 0x%016llx 0x%016llx\n",
				 (uint64_t)&hwlldch->ch[channel].wr[i].sar.reg,
				 lli->sar.reg);
		off += scnprintf(strbuf + off, size - off,
				 "dar           : 0x%016llx 0x%016llx\n",
				 (uint64_t)&hwlldch->ch[channel].wr[i].dar.reg,
				 lli->dar.reg);
	}

out_wr_chx_ll:
	ret = simple_read_from_buffer(user_buf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}

#define TR_DBFS_WR_CHX_LL(_channel)                                                   \
	static ssize_t axl_aipu_edma_debugfs_dbg_wr_ch##_channel##_ll_write(          \
		struct file *file, const char __user *user_buf, size_t count,         \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_edma_debugfs_dbg_wr_chx_ll_write(                     \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static ssize_t axl_aipu_edma_debugfs_dbg_wr_ch##_channel##_ll_read(           \
		struct file *file, char __user *user_buf, size_t count,               \
		loff_t *ppos)                                                         \
	{                                                                             \
		return axl_aipu_edma_debugfs_dbg_wr_chx_ll_read(                      \
			file, user_buf, count, ppos, _channel);                       \
	}                                                                             \
	static const struct file_operations                                           \
		axl_aipu_edma_debugfs_dbg_wr_ch##_channel##_ll_fops = {               \
			.owner = THIS_MODULE,                                         \
			.open = simple_open,                                          \
			.read = axl_aipu_edma_debugfs_dbg_wr_ch##_channel##_ll_read,  \
			.write =                                                      \
				axl_aipu_edma_debugfs_dbg_wr_ch##_channel##_ll_write, \
		}
TR_DBFS_WR_CHX_LL(0);
TR_DBFS_WR_CHX_LL(1);
TR_DBFS_WR_CHX_LL(2);
TR_DBFS_WR_CHX_LL(3);

static ssize_t axl_aipu_edma_debugfs_dma_stat_read(struct file *file,
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
static ssize_t axl_aipu_edma_debugfs_dma_stat_write(struct file *file,
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
static const struct file_operations axl_aipu_edma_debugfs_dma_stat_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_edma_debugfs_dma_stat_read,
	.write = axl_aipu_edma_debugfs_dma_stat_write,
};

/**
 * dma_trace_read - Read DMA traces from circular buffer
 *
 * Output format (one line per entry):
 * [timestamp] MODE CH# dur=<ns> req=<ns> wq=<ns> start=<ns> end=<ns> size=<bytes> sgt=<count>
 */
static ssize_t axl_aipu_dma_trace_read(struct file *file, char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	struct dma_trace_buffer *tb = axldev->trace_buf;
	char *buf;
	size_t buf_size, ret = 0;
	unsigned long head, tail, i;
	unsigned long flags;
	unsigned int entries_to_read;

	if (!tb)
		return -ENXIO;

	/* Allocate temporary buffer sized for all entries
	 * Estimate ~150 bytes per entry line + 2KB for header */
	buf_size = (tb->size * 150) + 2048;
	/* Cap at 16MB for safety */
	buf_size = min_t(size_t, buf_size, 16 * 1024 * 1024);
	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	spin_lock_irqsave(&tb->lock, flags);

	/* Snapshot head/tail */
	head = tb->head;
	tail = tb->tail;

	/* Calculate number of valid entries */
	entries_to_read = CIRC_CNT(head, tail, tb->size);

	/* Add header with stats */
	ret += scnprintf(buf + ret, buf_size - ret,
			 "DMA Trace Buffer Status:\n");
	ret += scnprintf(buf + ret, buf_size - ret, "  Enabled: %s\n",
			 atomic_read(&tb->enabled) ? "yes" : "no");
	ret += scnprintf(buf + ret, buf_size - ret, "  Total traces: %llu\n",
			 atomic64_read(&tb->total_traces));
	ret += scnprintf(buf + ret, buf_size - ret, "  Overruns: %llu\n",
			 atomic64_read(&tb->overruns));
	ret += scnprintf(buf + ret, buf_size - ret,
			 "  Buffer size: %u entries\n", tb->size);
	ret += scnprintf(buf + ret, buf_size - ret, "  Current entries: %u\n\n",
			 entries_to_read);

	/* Iterate through valid entries from tail to head */
	for (i = 0; i < entries_to_read && ret < buf_size - 256; i++) {
		unsigned long idx = (tail + i) & tb->size_mask;
		struct dma_trace_entry *e = &tb->entries[idx];

		ret += scnprintf(buf + ret, buf_size - ret,
				 "%s, CH%d, %llu, %llu, %llu, %llu, %lu\n",
				 e->mode, e->channel, e->ktime_ns,
				 e->ktime_wq_ns, e->ktime_dma_start_ns,
				 e->ktime_dma_end_ns, e->transfer_size);
	}

	if (i < entries_to_read) {
		ret += scnprintf(buf + ret, buf_size - ret,
				 "... %lu more entries (buffer too small)\n",
				 entries_to_read - i);
	}

	spin_unlock_irqrestore(&tb->lock, flags);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);
	kfree(buf);

	return ret;
}

/**
 * axl_aipu_dma_trace_write - Control DMA trace buffer
 *
 * Commands:
 *   "clear"   - Clear buffer (reset head/tail)
 *   "enable"  - Enable tracing
 *   "disable" - Disable tracing
 *   "reset"   - Clear buffer and reset counters
 */
static ssize_t axl_aipu_dma_trace_write(struct file *file,
					const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	struct dma_trace_buffer *tb = axldev->trace_buf;
	char cmd[16];
	unsigned long flags;
	int len;

	if (!tb)
		return -ENXIO;

	len = min_t(size_t, count, sizeof(cmd) - 1);
	if (copy_from_user(cmd, user_buf, len))
		return -EFAULT;
	cmd[len] = '\0';

	/* Trim newline */
	if (len > 0 && cmd[len - 1] == '\n')
		cmd[len - 1] = '\0';

	if (strcmp(cmd, "clear") == 0) {
		spin_lock_irqsave(&tb->lock, flags);
		tb->head = 0;
		tb->tail = 0;
		spin_unlock_irqrestore(&tb->lock, flags);
		dev_info(&axldev->pdev->dev, "DMA trace buffer cleared\n");

	} else if (strcmp(cmd, "enable") == 0) {
		atomic_set(&tb->enabled, 1);
		dev_info(&axldev->pdev->dev, "DMA tracing enabled\n");

	} else if (strcmp(cmd, "disable") == 0) {
		atomic_set(&tb->enabled, 0);
		dev_info(&axldev->pdev->dev, "DMA tracing disabled\n");

	} else if (strcmp(cmd, "reset") == 0) {
		spin_lock_irqsave(&tb->lock, flags);
		tb->head = 0;
		tb->tail = 0;
		atomic64_set(&tb->overruns, 0);
		atomic64_set(&tb->total_traces, 0);
		spin_unlock_irqrestore(&tb->lock, flags);
		dev_info(&axldev->pdev->dev, "DMA trace buffer reset\n");

	} else {
		dev_warn(
			&axldev->pdev->dev,
			"Unknown command. Use: clear, enable, disable, reset\n");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations axl_aipu_dma_trace_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_dma_trace_read,
	.write = axl_aipu_dma_trace_write,
};

void axl_aipu_edma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev)
{
	struct dentry *dentry;

	if (!axldev->dentry)
		return;
	dentry = axldev->dentry;

	dev_info(&axldev->pdev->dev, "Register directory %s-%s\n",
		 axldev->dev_info->devname, dev_name(&axldev->pdev->dev));
	debugfs_create_file("info", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_info_fops);
	debugfs_create_file("dma-ch-regs", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_ch_regs_fops);
	debugfs_create_file("dma-regs", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_regs_fops);
	debugfs_create_file("dma-rd-ch0-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_rd_ch0_ll_fops);
	debugfs_create_file("dma-rd-ch1-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_rd_ch1_ll_fops);
	debugfs_create_file("dma-rd-ch2-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_rd_ch2_ll_fops);
	debugfs_create_file("dma-rd-ch3-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_rd_ch3_ll_fops);
	debugfs_create_file("dma-wr-ch0-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_wr_ch0_ll_fops);
	debugfs_create_file("dma-wr-ch1-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_wr_ch1_ll_fops);
	debugfs_create_file("dma-wr-ch2-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_wr_ch2_ll_fops);
	debugfs_create_file("dma-wr-ch3-ll", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dbg_wr_ch3_ll_fops);
	debugfs_create_file("dma-statistics", 0444, dentry, axldev,
			    &axl_aipu_edma_debugfs_dma_stat_fops);
	debugfs_create_file("dma-trace", 0644, dentry, axldev,
			    &axl_aipu_dma_trace_fops);
}
void axl_aipu_edma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->dentry) {
		debugfs_remove_recursive(axldev->dentry);
		dev_info(&axldev->pdev->dev, "Unregister directory %s\n",
			 dev_name(&axldev->pdev->dev));
	}
}
