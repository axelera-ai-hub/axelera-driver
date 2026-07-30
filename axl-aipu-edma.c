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
 * SPDX-License-Identifier: GPL-2.0
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
#include <linux/circ_buf.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-pcie-edma.h"
#include "axl-aipu-edma-core.h"

/* Module params */
extern unsigned int dma_poll;
extern unsigned int enable_dmabuf_sync;
extern unsigned int enable_sg_host_dma;

/**
 * axl_dma_trace_add - Add DMA transfer trace to circular buffer
 * @axldev: Device context
 * @dma_wrk: DMA work structure with timing information
 * @mode: Transfer mode string ("RD" or "WR")
 * @total_size: Total bytes transferred
 *
 * Thread-safe trace capture from workqueue context.
 * If buffer is disabled or NULL, this is a fast no-op.
 */
static void axl_dma_trace_add(struct axl_pcie_aipu_dev *axldev,
			      struct dma_wrk *dma_wrk, const char *mode,
			      size_t total_size)
{
	struct dma_trace_buffer *tb = axldev->trace_buf;
	struct dma_trace_entry *entry;
	unsigned long flags;
	unsigned long head;

	/* Fast path: bail if tracing disabled or not allocated */
	if (!tb || !atomic_read(&tb->enabled))
		return;

	spin_lock_irqsave(&tb->lock, flags);

	/* Get current head position */
	head = tb->head;

	/* Check if buffer is full (overrun condition) */
	if (CIRC_SPACE(head, tb->tail, tb->size) == 0) {
		/* Buffer full: overwrite oldest entry by advancing tail */
		tb->tail = (tb->tail + 1) & tb->size_mask;
		atomic64_inc(&tb->overruns);
	}

	/* Write trace entry */
	entry = &tb->entries[head];
	entry->timestamp = ktime_get_ns();
	entry->duration_ns = ktime_to_ns(dma_wrk->duration);
	entry->ktime_ns = ktime_to_ns(dma_wrk->ktime);
	entry->ktime_wq_ns = ktime_to_ns(dma_wrk->ktime_wq);
	entry->ktime_dma_start_ns = ktime_to_ns(dma_wrk->ktime_dma_start);
	entry->ktime_dma_end_ns = ktime_to_ns(dma_wrk->ktime_dma_end);
	entry->transfer_size = total_size;
	entry->channel = dma_wrk->channel;
	entry->sgt_entries = dma_wrk->table ? dma_wrk->table->nents : 0;
	entry->flags = dma_wrk->flags;
	strncpy(entry->mode, mode, sizeof(entry->mode) - 1);
	entry->mode[sizeof(entry->mode) - 1] = '\0';

	/* Memory barrier: ensure entry is fully written before advancing head */
	smp_wmb();

	/* Advance head pointer */
	tb->head = (head + 1) & tb->size_mask;

	atomic64_inc(&tb->total_traces);

	spin_unlock_irqrestore(&tb->lock, flags);
}

static inline struct dw_edma_ll_buf *
axl_aipu_edma_get_ll_desc_base(struct axl_pcie_aipu_dev *axldev)
{
	return (struct dw_edma_ll_buf *)axldev->desc_base;
}

static void axl_aipu_edma_dev_dynmem_init(struct axl_pcie_aipu_dev *axldev)
{
	struct device_dma_sg_desc_t *dma_sg_desc;
	struct device_sys_ctl_t *dsctl;

	dsctl = axldev->vbase;
	dma_sg_desc = axl_aipu_get_dma_sg_desc_area(axldev);
	if (dma_sg_desc) {
		axldev->desc_base = dma_sg_desc->dma_sg_desc_buf_ref.addr;
		axldev->desc_offset =
			axldev->desc_base - dsctl->memory_map[MEMORY_AREA_0];
		pr_debug(
			"EDMA DMA SG descriptor area found at 0x%llx (offset 0x%llx)\n",
			axldev->desc_base, axldev->desc_offset);
	} else {
		pr_debug(
			"DMA SG descriptor area not found, falling back to fixed area\n");
		axldev->desc_base = EDMA_L2_LINKED_LIST_DESC_BASE;
		axldev->desc_offset = EDMA_L2_LINKED_LIST_DESC_OFF;
	}
	axldev->dma_vm = (dsctl->dma_vm == DMA_VM_SUPPORTED) ? 1 : 0;
}

static int axl_aipu_edma_dma_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	struct dw_edma_v0_regs *edma = axldev->dma;
	struct pci_dev *pdev = axldev->pdev;
	__u32 ctrl;
	int ch = id <= PMSI_METIS_RD_CH3 ? id - PMSI_METIS_RD_CH0 :
					   id - PMSI_METIS_WR_CH0;

	if (id <= PMSI_METIS_RD_CH3) {
		ctrl = GET_32_CH(edma, rd.ch_control1, ch);
		if (DW_EDMA_CTRL_CS(ctrl) == DW_EDMA_V0_CS_STOP) {
			dev_dbg(&pdev->dev,
				"wake (%d) DMA RD CH%d (ctrl 0x%x)\n", id, ch,
				ctrl);
			atomic_set(&axldev->irq_wrk[id].dma_done, 1);
			SET_32_CH(edma, rd.ch_control1, ch,
				  ctrl & ~DW_EDMA_V0_CS);
			return 1;
		}
	} else {
		ctrl = GET_32_CH(edma, wr.ch_control1, ch);
		if (DW_EDMA_CTRL_CS(ctrl) == DW_EDMA_V0_CS_STOP) {
			dev_dbg(&pdev->dev,
				"wake (%d) DMA WR CH%d (ctrl 0x%x)\n", id, ch,
				ctrl);
			atomic_set(&axldev->irq_wrk[id].dma_done, 1);
			SET_32_CH(edma, wr.ch_control1, ch,
				  ctrl & ~DW_EDMA_V0_CS);
			return 1;
		}
	}
	return 0;
}

static void axl_aipu_edma_enable_ctrl(struct axl_pcie_aipu_dev *axldev)
{
	struct dw_edma_v0_regs *edma = axldev->dma;

	edma->wr_engine_en = 1;
	edma->rd_engine_en = 1;
}

static void axl_aipu_edma_init_imwr(struct axl_pcie_aipu_dev *axldev)
{
	volatile struct dw_edma_v0_regs *edma = axldev->dma;

	if (axldev->dma_vm)
		return;

	edma->wr_done_imwr.lsb = axldev->irq_msi.address_lo;
	edma->wr_done_imwr.msb = axldev->irq_msi.address_hi;
	edma->wr_abort_imwr.lsb = axldev->irq_msi.address_lo;
	edma->wr_abort_imwr.msb = axldev->irq_msi.address_hi;

	edma->rd_done_imwr.lsb = axldev->irq_msi.address_lo;
	edma->rd_done_imwr.msb = axldev->irq_msi.address_hi;
	edma->rd_abort_imwr.lsb = axldev->irq_msi.address_lo;
	edma->rd_abort_imwr.msb = axldev->irq_msi.address_hi;

	if (axldev->nmsi == 1) {
		u32 imwr_data = axldev->irq_msi.data;
		edma->wr_ch01_imwr_data = imwr_data | (imwr_data << 16);
		edma->wr_ch23_imwr_data = imwr_data | (imwr_data << 16);
		edma->rd_ch01_imwr_data = imwr_data | (imwr_data << 16);
		edma->rd_ch23_imwr_data = imwr_data | (imwr_data << 16);
		return;
	}

	edma->rd_ch01_imwr_data = PMSI_METIS_RD_CH0 | (PMSI_METIS_RD_CH1 << 16);
	edma->rd_ch23_imwr_data = PMSI_METIS_RD_CH2 | (PMSI_METIS_RD_CH3 << 16);
	edma->wr_ch01_imwr_data = PMSI_METIS_WR_CH0 | (PMSI_METIS_WR_CH1 << 16);
	edma->wr_ch23_imwr_data = PMSI_METIS_WR_CH2 | (PMSI_METIS_WR_CH3 << 16);
}

static void axl_aipu_edma_align_imwr(struct axl_pcie_aipu_dev *axldev)
{
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	u32 imwr_data;

	if (axldev->dma_vm)
		return;

	get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
	if (axldev->nmsi != 1)
		return;

	imwr_data = axldev->irq_msi.data;
	edma->wr_ch01_imwr_data = imwr_data | (imwr_data << 16);
	edma->wr_ch23_imwr_data = imwr_data | (imwr_data << 16);
	edma->rd_ch01_imwr_data = imwr_data | (imwr_data << 16);
	edma->rd_ch23_imwr_data = imwr_data | (imwr_data << 16);
}

#define DMA_START_TIMEOUT 100000
static inline int dma_wait_irq(struct axl_pcie_aipu_dev *axldev,
			       struct dma_wrk *dma_wrk)
{
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx = dma_wrk->sctx;
	int err;
	__u32 ctrl;
	u32 timeout = DMA_START_TIMEOUT;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	enum dw_edma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_EDMA_DIR_WRITE :
				       DW_EDMA_DIR_READ;

	ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
	dev_dbg(&pdev->dev, "DMA %s CH%d (irq %d, ctrl 0x%x)\n", mode,
		dma_wrk->channel, dma_wrk->id, DW_EDMA_CTRL_CS(ctrl));
	SET_RW_32(edma, dir, doorbell, dma_wrk->channel);
	do {
		ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
		if (DW_EDMA_CTRL_CS(ctrl) == 0)
			SET_RW_32(edma, dir, doorbell, dma_wrk->channel);

		if (DW_EDMA_CTRL_CS(ctrl) == DW_EDMA_V0_CS_HALT)
			break;
		if (!--timeout)
			break;
		udelay(10);
	} while (!DW_EDMA_CTRL_CS(ctrl));
	err = wait_for_completion_timeout(
		&axldev->irq_wrk[dma_wrk->id].irq_done,
		axldev->irq_wrk[dma_wrk->id].timeout);

	if (err == 0) {
		dev_err(&pdev->dev, "DMA %s CH%d timeout (irq %d)\n", mode,
			dma_wrk->channel, dma_wrk->id);
		dma_wrk->status = -ETIMEDOUT;
		sctx_set_async_dma_xfer(sctx, ASYNC_XFER_TIMEOUT);
		dma_wrk->qctrl->num_err++;
		return -1;
	}

	if (axldev->nmsi == 1) {
		if (!atomic_read(&axldev->irq_wrk[dma_wrk->id].dma_done)) {
			dev_err(&pdev->dev,
				"DMA %s CH%d spurious wakeup (irq %d)\n", mode,
				dma_wrk->channel, dma_wrk->id);
			dma_wrk->status = -EIO;
			sctx_set_async_dma_xfer(sctx, ASYNC_XFER_FAIL);
			dma_wrk->qctrl->num_err++;
			return -1;
		}
	} else {
		ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
		if (DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP) {
			dev_err(&pdev->dev, "DMA error %s CH%d (ctrl 0x%x)\n",
				mode, dma_wrk->channel, DW_EDMA_CTRL_CS(ctrl));
			dma_wrk->status = -EIO;
			sctx_set_async_dma_xfer(sctx, ASYNC_XFER_FAIL);
			dma_wrk->qctrl->num_err++;
			return -1;
		}
	}

	return 0;
}
#define DMA_POLL_TIMEOUT 100000
static inline int dma_wait_poll(struct axl_pcie_aipu_dev *axldev,
				struct dma_wrk *dma_wrk)
{
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx = dma_wrk->sctx;
	u32 timeout = DMA_POLL_TIMEOUT;
	__u32 ctrl;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	enum dw_edma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_EDMA_DIR_WRITE :
				       DW_EDMA_DIR_READ;

	ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
	dev_dbg(&pdev->dev, "DMA %s CH%d (irq %d, ctrl 0x%x)\n", mode,
		dma_wrk->channel, dma_wrk->id, DW_EDMA_CTRL_CS(ctrl));
	do {
		ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
		if (DW_EDMA_CTRL_CS(ctrl) == 0)
			SET_RW_32(edma, dir, doorbell, dma_wrk->channel);

		if (DW_EDMA_CTRL_CS(ctrl) == DW_EDMA_V0_CS_HALT)
			break;
		if (!--timeout)
			break;
		udelay(10);
	} while (DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP);
	dev_dbg(&pdev->dev, "DMA %s CH%d  timeout %d (irq %d, ctrl 0x%x)\n",
		mode, dma_wrk->channel, timeout, dma_wrk->id,
		DW_EDMA_CTRL_CS(ctrl));

	ctrl = GET_RW_32_CH(edma, dir, ch_control1, dma_wrk->channel);
	if ((DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP) && !timeout) {
		dma_wrk->status = -ETIMEDOUT;
		sctx_set_async_dma_xfer(sctx, ASYNC_XFER_TIMEOUT);
		dma_wrk->qctrl->num_err++;
		return -1;
	}
	if (DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP) {
		dev_err(&pdev->dev, "DMA Poll error %s CH%d (ctrl 0x%x %d)\n",
			mode, dma_wrk->channel, DW_EDMA_CTRL_CS(ctrl), timeout);
		dma_wrk->status = -EIO;
		sctx_set_async_dma_xfer(sctx, ASYNC_XFER_FAIL);
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

/**
 * edma_transfer_descriptors - DMA descriptors from host memory to device L2
 * @axldev: Device context
 * @channel: DMA Read channel to use for descriptor transfer
 * @desc_size: Size of descriptors to transfer
 *
 * Uses a dedicated DMA Read channel to transfer pre-built descriptors from
 * host memory (after VMSI) to device L2 memory. This replaces individual
 * PCIe BAR writes with a single bulk DMA transfer.
 *
 * Return: 0 on success, negative error code on failure
 */
static int edma_transfer_descriptors(struct dma_wrk *dma_wrk, int channel,
				     size_t desc_size)
{
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;
	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_ll_buf *hwlldch;
	struct dw_edma_ll_buf *lldch =
		(struct dw_edma_ll_buf *)axldev->desc_host_pa;
	u64 ch_desc_base, ch_desc_hbase;
	enum dw_edma_dir dir = DW_EDMA_DIR_READ;
	enum dw_edma_dir desc_dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
					    DW_EDMA_DIR_WRITE :
					    DW_EDMA_DIR_READ;
	u32 ctrl;
	int desc_ch = dma_wrk->channel;
	int irq_id, err, ret = 0;
	bool need_lock;

	if (!axldev->desc_host_va) {
		dev_err(&pdev->dev, "Host descriptor buffer not initialized\n");
		return -EINVAL;
	}

	/*
	 * Only lock mutex for shared channel 3 (used by WRITE channels).
	 * READ channels use their own channel and don't need locking.
	 */
	need_lock = (channel == 3);
	if (need_lock)
		mutex_lock(&axldev->desc_mutex);

	hwlldch =
		(struct dw_edma_ll_buf *)axl_aipu_edma_get_ll_desc_base(axldev);
	ch_desc_base = __get_ll_base(hwlldch, desc_dir, desc_ch);
	ch_desc_hbase = __get_ll_base(lldch, desc_dir, desc_ch);

	dev_dbg(&pdev->dev,
		"CH%d: host_pa=0x%llx -> device=0x%llx, size=0x%lx\n", channel,
		ch_desc_hbase, ch_desc_base, desc_size);

	/* Map descriptor channel to IRQ ID (using Read channel) */
	irq_id = PMSI_METIS_RD_CH0 + channel;

	/* Configure DMA Read channel for descriptor transfer */
	SET_RW_32_CH(edma, dir, transfer_size, channel, desc_size);

	/* Enable interrupt if not in polling mode */
	if (!dma_poll) {
		SET_RW_32_CH(edma, dir, ch_control1, channel, DW_EDMA_V0_RIE);
		atomic_set(&axldev->irq_wrk[irq_id].dma_done, 0);
		reinit_completion(&axldev->irq_wrk[irq_id].irq_done);
	} else {
		SET_RW_32_CH(edma, dir, ch_control1, channel, 0);
	}

	/* Source: Host memory (descriptor buffer) */
	SET_RW_32_CH(edma, dir, sar.lsb, channel, _LSB(ch_desc_hbase));
	SET_RW_32_CH(edma, dir, sar.msb, channel, _MSB(ch_desc_hbase));

	/* Destination: Device L2 memory (descriptor area) */
	SET_RW_32_CH(edma, dir, dar.lsb, channel, _LSB(ch_desc_base));
	SET_RW_32_CH(edma, dir, dar.msb, channel, _MSB(ch_desc_base));

	/* Memory barrier to ensure descriptor writes are visible */
	wmb();

	dev_dbg(&pdev->dev,
		"CH%d: SIZE=0x%x | SAR=0x%x%08x | DAR=0x%x%08x | IRQ=%d\n",
		channel, GET_RW_32_CH(edma, dir, transfer_size, channel),
		GET_RW_32_CH(edma, dir, sar.msb, channel),
		GET_RW_32_CH(edma, dir, sar.lsb, channel),
		GET_RW_32_CH(edma, dir, dar.msb, channel),
		GET_RW_32_CH(edma, dir, dar.lsb, channel), irq_id);

	/* Start DMA transfer by ringing doorbell */
	SET_RW_32(edma, dir, doorbell, channel);
	ctrl = GET_RW_32_CH(edma, dir, ch_control1, channel);
	dev_dbg(&pdev->dev,
		"Starting descriptor transfer on DMA Read CH%d (ctrl=0x%x)\n",
		channel, DW_EDMA_CTRL_CS(ctrl));

	/* Wait for completion using interrupt or polling */
	if (dma_poll) {
		/* Polling mode */
		u32 timeout = DMA_POLL_TIMEOUT;
		do {
			ctrl = GET_RW_32_CH(edma, dir, ch_control1, channel);
			if (DW_EDMA_CTRL_CS(ctrl) == DW_EDMA_V0_CS_HALT) {
				dev_err(&pdev->dev,
					"Descriptor transfer halted unexpectedly (ctrl=0x%x)\n",
					DW_EDMA_CTRL_CS(ctrl));
				ret = -EIO;
				goto edma_release_lock;
			}
			if (--timeout == 0) {
				dev_err(&pdev->dev,
					"Descriptor transfer timeout (ctrl=0x%x)\n",
					DW_EDMA_CTRL_CS(ctrl));
				ret = -ETIMEDOUT;
				goto edma_release_lock;
			}
			udelay(10);
		} while (DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP);
	} else {
		/* Interrupt mode */
		err = wait_for_completion_timeout(
			&axldev->irq_wrk[irq_id].irq_done,
			axldev->irq_wrk[irq_id].timeout);

		ctrl = GET_RW_32_CH(edma, dir, ch_control1, channel);

		if (err == 0) {
			dev_err(&pdev->dev,
				"Descriptor transfer timeout on CH%d (irq %d, ctrl 0x%x)\n",
				channel, irq_id, DW_EDMA_CTRL_CS(ctrl));
			ret = -ETIMEDOUT;
			goto edma_release_lock;
		}

		if (DW_EDMA_CTRL_CS(ctrl) != DW_EDMA_V0_CS_STOP) {
			dev_err(&pdev->dev,
				"Descriptor transfer error on CH%d (ctrl 0x%x)\n",
				channel, DW_EDMA_CTRL_CS(ctrl));
			ret = -EIO;
			goto edma_release_lock;
		}
	}

	dev_dbg(&pdev->dev, "Descriptor transfer completed successfully\n");
edma_release_lock:
	if (need_lock)
		mutex_unlock(&axldev->desc_mutex);
	return ret;
}

static void axl_aipu_edma_sgdev_dma_job(struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;

	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_ll_buf *hwlldch;
	volatile struct dw_edma_ll_buf *lldch;

	struct sg_table *table = dma_wrk->table;
	struct scatterlist *sg;
	int i, n, channel, id;
	size_t tr_size = 0, total_size = 0;
	__u64 axi;
	enum dw_edma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_EDMA_DIR_WRITE :
				       DW_EDMA_DIR_READ;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	hwlldch =
		(struct dw_edma_ll_buf *)axl_aipu_edma_get_ll_desc_base(axldev);
	lldch = axldev->vbase + axldev->desc_offset;

	dma_wrk->status = 0;
	dma_wrk->ktime_wq = ktime_get();

	axi = dma_wrk->axi;
	sg = table->sgl;
	channel = dma_wrk->channel;
	id = dma_wrk->id;

	dev_dbg(&pdev->dev, "DMA %s CH%d work (%p)\n", mode, dma_wrk->channel,
		dma_wrk);
	axl_aipu_edma_align_imwr(axldev);
	for (n = dma_wrk->num_sgt; n < table->nents; n += i) {
		dev_dbg(&pdev->dev, "DMA %s CH%d (%d of %d)\n", mode, channel,
			n, table->nents);
		SET_RW_32(edma, dir, linked_list_err_en, BIT(channel));
		SET_RW_32_CH(edma, dir, ch_control1, channel,
			     DW_EDMA_V0_LLE | DW_EDMA_V0_CCS);
		SET_RW_32_CH(edma, dir, llp.lsb, channel,
			     _LSB(__get_ll_base(hwlldch, dir, channel)));
		SET_RW_32_CH(edma, dir, llp.msb, channel,
			     _MSB(__get_ll_base(hwlldch, dir, channel)));

		dev_dbg(&pdev->dev, "DMA %s CH%d (%llx)\n", mode, channel,
			__get_ll_base(hwlldch, dir, channel));
		total_size = 0;
		for (i = 0; i < DW_EDMA_LL_MAX_NUM && i < (table->nents - n);
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
					DW_EDMA_V0_CB);
			LL_SET_RW_32_CH(lldch, dir, channel, i, transfer_size,
					tr_size);
			if (dir == DW_EDMA_DIR_WRITE) {
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
		if (!dma_poll) {
			LL_SET_RW_32_CH(lldch, dir, channel, i - 1, control,
					DW_EDMA_V0_CB | DW_EDMA_V0_RIE);
		} else {
			LL_SET_RW_32_CH(lldch, dir, channel, i - 1, control,
					DW_EDMA_V0_CB);
		}

		dma_wrk->size -= total_size;

		if (i == DW_EDMA_LL_MAX_NUM)
			dev_dbg(&pdev->dev,
				"GO OUT of max channel desc number %d 0x%lx 0x%lx desc\n",
				i, total_size, dma_wrk->size);

		LL_SET_RW_32_CH(lldch, dir, channel, i, control,
				DW_EDMA_V0_TCB);
		LL_SET_RW_32_CH(lldch, dir, channel, i + 1, control, 0);
		mb();
		LL_GET_RW_32_CH(lldch, dir, channel, i, control);
		if (!dma_poll) {
			atomic_set(&axldev->irq_wrk[dma_wrk->id].dma_done, 0);
			reinit_completion(
				&axldev->irq_wrk[dma_wrk->id].irq_done);
		}

		dma_wrk->ktime_dma_start = ktime_get();
		if (dma_wait(axldev, dma_wrk))
			break;

		if (dma_wrk->status || (dma_wrk->size == 0))
			break;
	}
	if ((dir == DW_EDMA_DIR_WRITE) && enable_dmabuf_sync)
		dma_sync_sg_for_cpu(&pdev->dev, table->sgl, table->nents,
				    DMA_FROM_DEVICE);
	dma_wrk->ktime_dma_end = ktime_get();
	get_max_duration(dma_wrk);
	/* Add to trace buffer */
	axl_dma_trace_add(axldev, dma_wrk, mode, total_size);
}

static void axl_aipu_edma_sghost_dma_job(struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;

	volatile struct dw_edma_v0_regs *edma = axldev->dma;
	volatile struct dw_edma_ll_buf *hwlldch;
	struct dw_edma_ll_buf *lldch, *lldch_pa;

	struct sg_table *table = dma_wrk->table;
	struct scatterlist *sg;
	int i, n, channel, id, desc_ch;
	size_t tr_size = 0, total_size = 0;
	__u64 axi;
	enum dw_edma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_EDMA_DIR_WRITE :
				       DW_EDMA_DIR_READ;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	hwlldch =
		(struct dw_edma_ll_buf *)axl_aipu_edma_get_ll_desc_base(axldev);
	lldch = axldev->desc_host_va;
	lldch_pa = (struct dw_edma_ll_buf *)axldev->desc_host_pa;

	dma_wrk->status = 0;
	dma_wrk->ktime_wq = ktime_get();

	axi = dma_wrk->axi;
	sg = table->sgl;
	channel = dma_wrk->channel;
	id = dma_wrk->id;

	dev_dbg(&pdev->dev, "DMA %s CH%d work (%p)\n", mode, dma_wrk->channel,
		dma_wrk);
	axl_aipu_edma_align_imwr(axldev);
	dev_dbg(&pdev->dev,
		"DMA %s CH%d LLD host va 0x%llx pa 0x%llx device 0x%llx\n",
		mode, channel, (u64)lldch, (u64)lldch_pa,
		(u64)__get_ll_base(hwlldch, dir, channel));

	for (n = dma_wrk->num_sgt; n < table->nents; n += i) {
		dev_dbg(&pdev->dev, "DMA %s CH%d (%d of %d)\n", mode, channel,
			n, table->nents);
		total_size = 0;
		for (i = 0; i < DW_EDMA_LL_MAX_NUM && i < (table->nents - n);
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
			/* Build descriptor in host memory */
			LL_SET_RW_32_HOST(lldch, dir, channel, i, control,
					  DW_EDMA_V0_CB);
			LL_SET_RW_32_HOST(lldch, dir, channel, i, transfer_size,
					  tr_size);
			if (dir == DW_EDMA_DIR_WRITE) {
				LL_SET_RW_64_HOST(lldch, dir, channel, i, sar,
						  axi);
				LL_SET_RW_64_HOST(lldch, dir, channel, i, dar,
						  haddr);
			} else {
				LL_SET_RW_64_HOST(lldch, dir, channel, i, sar,
						  haddr);
				LL_SET_RW_64_HOST(lldch, dir, channel, i, dar,
						  axi);
			}
			dev_dbg(&pdev->dev,
				"CH%d CTRL=0x%x | SIZE=0x%x | SAR=0x%llx | DAR=0x%llx (%d 0x%llx)\n",
				channel,
				LL_GET_RW_32_HOST(lldch, dir, channel, i,
						  control),
				LL_GET_RW_32_HOST(lldch, dir, channel, i,
						  transfer_size),
				LL_GET_RW_64_HOST(lldch, dir, channel, i, sar),
				LL_GET_RW_64_HOST(lldch, dir, channel, i, dar),
				i, (u64)__get_ll_base(lldch_pa, dir, channel));

			axi += tr_size;
			total_size += tr_size;
			if (total_size == dma_wrk->size) {
				i++;
				break;
			}
			i++;
		}
		/* Set interrupt enable on last descriptor (host memory) */
		if (!dma_poll) {
			LL_SET_RW_32_HOST(lldch, dir, channel, i - 1, control,
					  DW_EDMA_V0_CB | DW_EDMA_V0_RIE);
		} else {
			LL_SET_RW_32_HOST(lldch, dir, channel, i - 1, control,
					  DW_EDMA_V0_CB);
		}

		dma_wrk->size -= total_size;

		if (i == DW_EDMA_LL_MAX_NUM)
			dev_dbg(&pdev->dev,
				"GO OUT of max channel desc number %d 0x%lx 0x%lx desc\n",
				i, total_size, dma_wrk->size);

		/* Terminator descriptor (host memory) */
		LL_SET_RW_32_HOST(lldch, dir, channel, i, control,
				  DW_EDMA_V0_TCB);
		LL_SET_RW_32_HOST(lldch, dir, channel, i + 1, control, 0);

		/*
		 * Transfer descriptors from host memory to device L2 memory.
		 * READ channels can use themselves for descriptor transfer.
		 * WRITE channels must use a READ channel (channel 3) since
		 * descriptor transfer is always host->device (READ operation).
		 */
		if (dir == DW_EDMA_DIR_READ) {
			/* READ channels: use same channel for descriptor transfer */
			desc_ch = channel;
		} else {
			/* WRITE channels: must borrow READ channel 3 */
			desc_ch = RESERVED_DESC_DMA_CH;
		}
		if (edma_transfer_descriptors(
			    dma_wrk, desc_ch,
			    (i + 1) * sizeof(struct dw_edma_v0_lli))) {
			dev_err(&pdev->dev,
				"Failed to transfer descriptors for %s CH%d\n",
				mode, channel);
			dma_wrk->status = -EIO;
			return;
		}

		SET_RW_32(edma, dir, linked_list_err_en, BIT(channel));
		SET_RW_32_CH(edma, dir, ch_control1, channel,
			     DW_EDMA_V0_LLE | DW_EDMA_V0_CCS);
		SET_RW_32_CH(edma, dir, llp.lsb, channel,
			     _LSB(__get_ll_base(hwlldch, dir, channel)));
		SET_RW_32_CH(edma, dir, llp.msb, channel,
			     _MSB(__get_ll_base(hwlldch, dir, channel)));

		dev_dbg(&pdev->dev, "DMA %s CH%d (0x%llx 0x%llx)\n", mode,
			channel, (u64)axl_aipu_edma_get_ll_desc_base(axldev),
			__get_ll_base(hwlldch, dir, channel));

		if (!dma_poll) {
			atomic_set(&axldev->irq_wrk[dma_wrk->id].dma_done, 0);
			reinit_completion(
				&axldev->irq_wrk[dma_wrk->id].irq_done);
		}

		dma_wrk->ktime_dma_start = ktime_get();
		if (dma_wait(axldev, dma_wrk))
			break;

		if (dma_wrk->status || (dma_wrk->size == 0))
			break;
	}
	if ((dir == DW_EDMA_DIR_WRITE) && enable_dmabuf_sync)
		dma_sync_sg_for_cpu(&pdev->dev, table->sgl, table->nents,
				    DMA_FROM_DEVICE);
	dma_wrk->ktime_dma_end = ktime_get();
	get_max_duration(dma_wrk);
	/* Add to trace buffer */
	axl_dma_trace_add(axldev, dma_wrk, mode, total_size);
}

static void axl_aipu_edma_dma_job(struct dma_wrk *dma_wrk)
{
	if (enable_sg_host_dma)
		axl_aipu_edma_sghost_dma_job(dma_wrk);
	else
		axl_aipu_edma_sgdev_dma_job(dma_wrk);
}

static void axl_aipu_edma_dma_p2p_job(struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = dma_wrk->axldev;
	struct pci_dev *pdev = axldev->pdev;

	volatile struct dw_edma_v0_regs *edma = axldev->dma;

	int channel, id;
	__u64 axi, p2pphy;
	enum dw_edma_dir dir = dma_wrk->flags & DMABUF_XFER_FLAG_READ ?
				       DW_EDMA_DIR_WRITE :
				       DW_EDMA_DIR_READ;
	const char *mode = dma_wrk->flags & DMABUF_XFER_FLAG_READ ? "WR" : "RD";

	dma_wrk->status = 0;

	axi = dma_wrk->axi;
	p2pphy = dma_wrk->p2pphy;
	channel = dma_wrk->channel;
	id = dma_wrk->id;

	dev_dbg(&pdev->dev, "DMA P2P %s CH%d work (%p)\n", mode,
		dma_wrk->channel, dma_wrk);
	SET_RW_32_CH(edma, dir, transfer_size, channel, dma_wrk->size);
	SET_RW_32_CH(edma, dir, ch_control1, channel,
		     !dma_poll ? DW_EDMA_V0_RIE : 0);

	if (dir == DW_EDMA_DIR_WRITE) {
		SET_RW_32_CH(edma, dir, sar.lsb, channel, _LSB(axi));
		SET_RW_32_CH(edma, dir, sar.msb, channel, _MSB(axi));
		SET_RW_32_CH(edma, dir, dar.lsb, channel, _LSB(p2pphy));
		SET_RW_32_CH(edma, dir, dar.msb, channel, _MSB(p2pphy));
	} else {
		SET_RW_32_CH(edma, dir, dar.lsb, channel, _LSB(axi));
		SET_RW_32_CH(edma, dir, dar.msb, channel, _MSB(axi));
		SET_RW_32_CH(edma, dir, sar.lsb, channel, _LSB(p2pphy));
		SET_RW_32_CH(edma, dir, sar.msb, channel, _MSB(p2pphy));
	}
	dev_dbg(&pdev->dev,
		"CH%d CTRL=0x%x | SIZE=0x%x | SAR=0x%x%08x | DAR=0x%x%08x\n",
		channel, GET_RW_32_CH(edma, dir, ch_control1, channel),
		GET_RW_32_CH(edma, dir, transfer_size, channel),
		GET_RW_32_CH(edma, dir, sar.msb, channel),
		GET_RW_32_CH(edma, dir, sar.lsb, channel),
		GET_RW_32_CH(edma, dir, dar.msb, channel),
		GET_RW_32_CH(edma, dir, dar.lsb, channel));
	if (!dma_poll) {
		atomic_set(&axldev->irq_wrk[dma_wrk->id].dma_done, 0);
		reinit_completion(&axldev->irq_wrk[dma_wrk->id].irq_done);
	}

	(void)dma_wait(axldev, dma_wrk);
	get_max_duration(dma_wrk);
}

static struct axl_dev_fops axl_aipu_edma_fops = {
	.dma_irq_ck = axl_aipu_edma_dma_irq_ck,
	.dma_enable_ctrl = axl_aipu_edma_enable_ctrl,
	.dma_job_submit = axl_aipu_edma_dma_job,
	.dma_p2p_job_submit = axl_aipu_edma_dma_p2p_job,
	.dma_init_imwr = axl_aipu_edma_init_imwr,
	.dev_dynmem_init = axl_aipu_edma_dev_dynmem_init,

	.dev_debugfs_init = axl_aipu_edma_dev_debugfs_init,
	.dev_debugfs_exit = axl_aipu_edma_dev_debugfs_exit,
};

void axl_aipu_edma_register_dev_fops(struct axl_pcie_aipu_dev *axldev)
{
	axldev->fops = &axl_aipu_edma_fops;
}
