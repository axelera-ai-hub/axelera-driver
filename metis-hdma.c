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

/* Module params */
extern unsigned int dma_poll;
extern unsigned int enable_dmabuf_sync;

static int hdma_dma_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	pr_warn("not implemented\n");
	return -1;
}

static void hdma_enable_ctrl(struct axl_pcie_aipu_dev *axldev)
{
	struct dw_hdma_v0_regs *hdma = axldev->dma;
	int i;

	for (i = 0; i < HDMA_V0_MAX_NR_CH; i++) {
		writel(BIT(0), &hdma->ch[i].rd.ch_en);
		writel(BIT(0), &hdma->ch[i].wr.ch_en);
	}
}
static void hdma_init_imwr(struct axl_pcie_aipu_dev *axldev)
{
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	int i;
	enum dw_hdma_dir dir;
	u32 setup;
	u64 tmp;
	volatile struct dw_hdma_ll_buf *hwlldch =
		(struct dw_hdma_ll_buf *)(HDMA_LINKED_LIST_DESC_BASE);

	for (i = 0; i < HDMA_V0_MAX_NR_CH; i++) {
		dir = DW_HDMA_DIR_READ;
		SET_RW_32_CH(hdma, dir, msi_stop.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_stop.msb, i,
			     axldev->irq_msi.address_hi);
		SET_RW_32_CH(hdma, dir, msi_abort.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_abort.msb, i,
			     axldev->irq_msi.address_hi);
		SET_RW_32_CH(hdma, dir, msi_watermark.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_watermark.msb, i,
			     axldev->irq_msi.address_hi);

		if (axldev->nmsi == 1) {
			SET_RW_32_CH(hdma, dir, msi_msgdata, i,
				     axldev->irq_msi.data);
		} else {
			SET_RW_32_CH(hdma, dir, msi_msgdata, i, MSI_RD_CH0 + i);
		}
		setup = GET_RW_32_CH(hdma, dir, int_setup, i);
		setup &= ~(HDMA_V0_STOP_INT_MASK | HDMA_V0_ABORT_INT_MASK);
		setup |= HDMA_V0_REMOTE_STOP_INT_EN |
			 HDMA_V0_REMOTE_ABORT_INT_EN;
		SET_RW_32_CH(hdma, dir, int_setup, i, setup);
		SET_RW_32_CH(hdma, dir, control1, i, HDMA_V0_LINKLIST_EN);

		tmp = __get_ll_base(hwlldch, dir, i);
		SET_RW_32_CH(hdma, dir, llp.lsb, i, lower_32_bits(tmp));
		SET_RW_32_CH(hdma, dir, llp.msb, i, upper_32_bits(tmp));
	}
	for (i = 0; i < HDMA_V0_MAX_NR_CH; i++) {
		dir = DW_HDMA_DIR_WRITE;
		SET_RW_32_CH(hdma, dir, msi_stop.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_stop.msb, i,
			     axldev->irq_msi.address_hi);
		SET_RW_32_CH(hdma, dir, msi_abort.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_abort.msb, i,
			     axldev->irq_msi.address_hi);
		SET_RW_32_CH(hdma, dir, msi_watermark.lsb, i,
			     axldev->irq_msi.address_lo);
		SET_RW_32_CH(hdma, dir, msi_watermark.msb, i,
			     axldev->irq_msi.address_hi);

		if (axldev->nmsi == 1) {
			SET_RW_32_CH(hdma, dir, msi_msgdata, i,
				     axldev->irq_msi.data);
		} else {
			SET_RW_32_CH(hdma, dir, msi_msgdata, i, MSI_WR_CH0 + i);
		}
		setup = GET_RW_32_CH(hdma, dir, int_setup, i);
		setup &= ~(HDMA_V0_STOP_INT_MASK | HDMA_V0_ABORT_INT_MASK);
		setup |= HDMA_V0_REMOTE_STOP_INT_EN |
			 HDMA_V0_REMOTE_ABORT_INT_EN;
		SET_RW_32_CH(hdma, dir, int_setup, i, setup);
		SET_RW_32_CH(hdma, dir, control1, i, HDMA_V0_LINKLIST_EN);

		tmp = __get_ll_base(hwlldch, dir, i);
		SET_RW_32_CH(hdma, dir, llp.lsb, i, lower_32_bits(tmp));
		SET_RW_32_CH(hdma, dir, llp.msb, i, upper_32_bits(tmp));
	}
}

static inline int dma_wait_irq(struct axl_pcie_aipu_dev *axldev,
			       struct dma_wrk *dma_wrk)
{
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx = dma_wrk->sctx;
	int err;
	u32 ch_stat;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	enum dw_hdma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_HDMA_DIR_WRITE :
				       DW_HDMA_DIR_READ;

	ch_stat = GET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel);

	dev_dbg(&pdev->dev, "DMA %s CH%d (irq %d, status 0x%x)\n", mode,
		dma_wrk->channel, dma_wrk->id, ch_stat);

	SET_RW_32_CH(hdma, dir, doorbell, dma_wrk->channel,
		     HDMA_V0_DOORBELL_START);
	err = wait_for_completion_timeout(
		&axldev->irq_wrk[dma_wrk->id].irq_done,
		axldev->irq_wrk[dma_wrk->id].timeout);

	ch_stat = GET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel);
	if (err == 0) {
		dev_err(&pdev->dev,
			"DMA %s CH%d timeout (irq %d, status 0x%x)\n", mode,
			dma_wrk->channel, dma_wrk->id, ch_stat);
		if (ch_stat != STATUS_REG_STOPPED) {
			dma_wrk->status = -ETIMEDOUT;
			sctx->async_dma_xfer = ASYNC_XFER_TIMEOUT;
			dma_wrk->qctrl->num_err++;
			return -1;
		}
	}
	if (ch_stat != STATUS_REG_STOPPED) {
		dev_err(&pdev->dev, "DMA error %s CH%d (status 0x%x)\n", mode,
			dma_wrk->channel, ch_stat);
		dma_wrk->status = -EIO;
		sctx->async_dma_xfer = ASYNC_XFER_FAIL;
		dma_wrk->qctrl->num_err++;
		return -1;
	}
	return 0;
}
#define DMA_POLL_TIMEOUT 100000
static inline int dma_wait_poll(struct axl_pcie_aipu_dev *axldev,
				struct dma_wrk *dma_wrk)
{
	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx = dma_wrk->sctx;
	u32 timeout = DMA_POLL_TIMEOUT;
	u32 ch_stat;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	enum dw_hdma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_HDMA_DIR_WRITE :
				       DW_HDMA_DIR_READ;

	SET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel, 0);
	ch_stat = GET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel);
	SET_RW_32_CH(hdma, dir, doorbell, dma_wrk->channel,
		     HDMA_V0_DOORBELL_START);
	dev_dbg(&pdev->dev, "DMA %s CH%d (irq %d, stat 0x%x)\n", mode,
		dma_wrk->channel, dma_wrk->id, ch_stat);
	do {
		ch_stat = GET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel);
		if (ch_stat == STATUS_REG_ABORTED)
			break;
		if (!--timeout)
			break;
		udelay(10);
	} while (ch_stat != STATUS_REG_STOPPED);
	dev_dbg(&pdev->dev, "DMA %s CH%d  timeout %d (irq %d, stat 0x%x)\n",
		mode, dma_wrk->channel, timeout, dma_wrk->id, ch_stat);

	ch_stat = GET_RW_32_CH(hdma, dir, ch_stat, dma_wrk->channel);
	if ((ch_stat != STATUS_REG_STOPPED) && !timeout) {
		dma_wrk->status = -ETIMEDOUT;
		sctx->async_dma_xfer = ASYNC_XFER_TIMEOUT;
		dma_wrk->qctrl->num_err++;
		return -1;
	}
	if (ch_stat != STATUS_REG_STOPPED) {
		dev_err(&pdev->dev, "DMA Poll error %s CH%d (stat 0x%x %d)\n",
			mode, dma_wrk->channel, ch_stat, timeout);
		dma_wrk->status = -EIO;
		sctx->async_dma_xfer = ASYNC_XFER_FAIL;
		dma_wrk->qctrl->num_err++;
		return -1;
	}
	return 0;
}
static inline int dma_wait(struct axl_pcie_aipu_dev *axldev,
			   struct dma_wrk *dma_wrk)
{
	if (dma_poll)
		return dma_wait_poll(axldev, dma_wrk);

	return dma_wait_irq(axldev, dma_wrk);
}

static void hdma_dma_job(struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;

	volatile struct dw_hdma_v0_regs *hdma = axldev->dma;
	volatile struct dw_hdma_ll_buf *hwlldch;
	volatile struct dw_hdma_ll_buf *lldch;

	struct sg_table *table = dma_wrk->table;
	struct scatterlist *sg;
	int i, n, channel, id;
	size_t tr_size = 0, total_size = 0;
	u64 axi;
	u32 setup;
	enum dw_hdma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_HDMA_DIR_WRITE :
				       DW_HDMA_DIR_READ;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	hwlldch = (struct dw_hdma_ll_buf *)(HDMA_LINKED_LIST_DESC_BASE);
	lldch = axldev->vl2base + HDMA_LINKED_LIST_DESC_OFF;
	dma_wrk->status = 0;

	axi = dma_wrk->axi;
	sg = table->sgl;
	channel = dma_wrk->channel;
	id = dma_wrk->id;

	dev_dbg(&pdev->dev, "DMA %s CH%d work (%p)\n", mode, dma_wrk->channel,
		dma_wrk);

	setup = GET_RW_32_CH(hdma, dir, int_setup, dma_wrk->channel);
	setup &= ~(HDMA_V0_STOP_INT_MASK | HDMA_V0_ABORT_INT_MASK);
	if (!dma_poll) {
		setup |= HDMA_V0_REMOTE_STOP_INT_EN |
			 HDMA_V0_REMOTE_ABORT_INT_EN;
	}
	SET_RW_32_CH(hdma, dir, int_setup, dma_wrk->channel, setup);

	for (n = dma_wrk->num_sgt; n < table->nents; n += i) {
		dev_dbg(&pdev->dev, "DMA %s CH%d (%d of %d)\n", mode, channel,
			n, table->nents);
		SET_RW_32_CH(hdma, dir, control1, channel, HDMA_V0_LINKLIST_EN);
		SET_RW_32_CH(hdma, dir, cycle_sync, channel,
			     HDMA_V0_CONSUMER_CYCLE_STAT |
				     HDMA_V0_CONSUMER_CYCLE_BIT);

		dev_dbg(&pdev->dev, "DMA %s CH%d (%llx)\n", mode, channel,
			__get_ll_base(hwlldch, dir, channel));
		total_size = 0;
		for (i = 0; i < DW_HDMA_LL_MAX_NUM && i < (table->nents - n);
		     sg = sg_next(sg)) {
			__u64 haddr;
			if (sg == NULL) {
				dev_err(&pdev->dev, "SG NULL\n");
				return;
			}
			if (dma_wrk->offset > sg_dma_len(sg)) {
				dma_wrk->offset -= sg_dma_len(sg);
				n++;
				continue;
			}
			if (dma_wrk->offset) {
				haddr = sg_dma_address(sg) + dma_wrk->offset;
				tr_size =
					min_t(size_t,
					      sg_dma_len(sg) - dma_wrk->offset,
					      dma_wrk->size - total_size);
				dma_wrk->offset = 0;
			} else {
				tr_size = min_t(size_t, sg_dma_len(sg),
						dma_wrk->size - total_size);
				haddr = sg_dma_address(sg);
			}
			LL_SET_RW_32_CH(lldch, dir, channel, i, control,
					DW_HDMA_V0_CB);
			LL_SET_RW_32_CH(lldch, dir, channel, i, transfer_size,
					tr_size);
			if (dir == DW_HDMA_DIR_WRITE) {
				LL_SET_RW_64_CH(lldch, dir, channel, i, sar,
						axi);
				LL_SET_RW_64_CH(lldch, dir, channel, i, dar,
						haddr);
			} else {
				LL_SET_RW_64_CH(lldch, dir, channel, i, sar,
						haddr);
				LL_SET_RW_64_CH(lldch, dir, channel, i, dar,
						axi);
			}
			dev_dbg(&pdev->dev,
				"CH%d CTRL=0x%x | SIZE=0x%x | SAR=0x%llx | DAR=0x%llx (%d)\n",
				channel,
				LL_GET_RW_32_CH(lldch, dir, channel, i,
						control),
				LL_GET_RW_32_CH(lldch, dir, channel, i,
						transfer_size),
				LL_GET_RW_64_CH(lldch, dir, channel, i,
						sar.reg),
				LL_GET_RW_64_CH(lldch, dir, channel, i,
						dar.reg),
				i);

			axi += tr_size;
			total_size += tr_size;
			if (total_size == dma_wrk->size) {
				i++;
				break;
			}
			i++;
		}
		dma_wrk->size -= total_size;

		if (i == DW_HDMA_LL_MAX_NUM)
			dev_dbg(&pdev->dev,
				"GO OUT of max channel desc number %d 0x%lx 0x%lx desc\n",
				i, total_size, dma_wrk->size);

		LL_SET_RW_32_CH(lldch, dir, channel, i, control, 0);
		mb();
		LL_GET_RW_32_CH(lldch, dir, channel, i, control);
		if (!dma_poll) {
			reinit_completion(
				&axldev->irq_wrk[dma_wrk->id].irq_done);
		}

		if (dma_wait(axldev, dma_wrk))
			break;

		if (dma_wrk->status || (dma_wrk->size == 0))
			break;
	}
	if ((dir == DW_HDMA_DIR_WRITE) && enable_dmabuf_sync)
		dma_sync_sg_for_cpu(&pdev->dev, table->sgl, table->nents,
				    DMA_FROM_DEVICE);
}

static struct axl_dev_fops hdma_fops = {
	.dma_irq_ck = hdma_dma_irq_ck,
	.dma_enable_ctrl = hdma_enable_ctrl,
	.dma_job_submit = hdma_dma_job,
	.dma_init_imwr = hdma_init_imwr,

	.dev_debugfs_init = hdma_dev_debugfs_init,
	.dev_debugfs_exit = hdma_dev_debugfs_exit,
};

void hdma_register_dev_fops(struct axl_pcie_aipu_dev *axldev)
{
	axldev->fops = &hdma_fops;
}
