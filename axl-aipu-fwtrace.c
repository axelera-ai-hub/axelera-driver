// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Axelera AI
 * Firmware log/trace kernel consumer implementation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/msi.h>
#include <linux/idr.h>
#include <linux/kref.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-fwtrace.h"

/* Module parameters */
static unsigned int fwtrace_kfifo_size = 1048576; /* 1MB default */
module_param(fwtrace_kfifo_size, uint, 0644);
MODULE_PARM_DESC(fwtrace_kfifo_size,
		 "Kernel buffer size per trace source (bytes)");

static unsigned int fwtrace_dma_buffer_size = 262144; /* 256KB default */
module_param(fwtrace_dma_buffer_size, uint, 0644);
MODULE_PARM_DESC(fwtrace_dma_buffer_size,
		 "DMA buffer size for device transfers (bytes)");

static unsigned int fwtrace_poll_interval = 1000; /* 1 second default */
module_param(fwtrace_poll_interval, uint, 0644);
MODULE_PARM_DESC(fwtrace_poll_interval,
		 "Polling interval in milliseconds (0 = disabled)");

static bool fwtrace_enabled = true;
module_param(fwtrace_enabled, bool, 0644);
MODULE_PARM_DESC(fwtrace_enabled, "Enable firmware trace consumer");

static unsigned int fwtrace_dma_threshold = FWTRACE_DMA_THRESHOLD;
module_param(fwtrace_dma_threshold, uint, 0644);
MODULE_PARM_DESC(
	fwtrace_dma_threshold,
	"Min bytes to use DMA for binary trace sources (0 = always PIO)");

/*
 * Firmware datastream structures matching device_datastream_t in
 * libaxldev/include/sysctl_mem.h (SYSCTL_DATASTREAM_AREA_VERSION 2.0).
 * Must match firmware layout exactly.
 *
 * ring_buffer_ctrl layout (16 bytes):
 *   read_pos  u32 @ 0
 *   capacity  u32 @ 4
 *   write_pos u32 @ 8
 *   reg       u32 @ 12  [bits 0-9: msi, bits 10-30: reserved, bit 31: enabled]
 *
 * data_stream layout (80 bytes, __attribute__((aligned(8)))):
 *   ctrl      16B @ 0   (ring_buffer_ctrl_t)
 *   buf_ref   16B @ 16  (addr u64 @ 16, size u64 @ 24)
 *   type      u32 @ 32
 *   cfg_opcode u32@ 36
 *   cfg_core_id i32@ 40
 *   name[32]      @ 44
 *   <4B pad>      @ 76  (struct size rounded to 8-byte alignment)
 *
 * device_datastream layout:
 *   num_streams u32       @ 0
 *   source_to_index[256]  @ 4
 *   <4B pad>              @ 260 (to align stream[] to 8 bytes)
 *   stream[]              @ 264
 */
#define DATASTREAM_NUM_STREAMS_OFFSET  0
#define DATASTREAM_SOURCE_INDEX_OFFSET 4 /* uint8_t[256] */
#define DATASTREAM_STREAM_ARRAY_OFFSET 264 /* first data_stream */
#define DATA_STREAM_SIZE	       80 /* sizeof(data_stream_t) */

/* Offsets within a single data_stream (version 2.0 layout) */
#define DS_CTRL_READ_POS  0
#define DS_CTRL_CAPACITY  4
#define DS_CTRL_WRITE_POS 8
#define DS_CTRL_REG	  12
#define DS_BUF_REF_ADDR	  16
#define DS_BUF_REF_SIZE	  24
#define DS_TYPE_OFFSET	  32 /* stream_type_t: 0=TEXT, 1=BINARY */
#define DS_NAME_OFFSET	  44 /* char name[DATA_STREAM_NAME_MAX_LEN] */
#define DS_NAME_LEN	  32 /* DATA_STREAM_NAME_MAX_LEN */

#define STREAM_TYPE_TEXT   0
#define STREAM_TYPE_BINARY 1

/* ring_buffer_ctrl.reg bit fields */
#define CTRL_REG_MSI_MASK    GENMASK(9, 0) /* bits 0-9 */
#define CTRL_REG_ENABLED_BIT BIT(31)

/**
 * get_datastream_base - Get pointer to device_datastream_t in device memory
 * @dev: Device structure
 */
static void __iomem *get_datastream_base(struct axl_pcie_aipu_dev *dev)
{
	struct device_sys_ctl_t *dsctl;
	struct memory_reference_t *memref;

	if (!dev->vl2base) {
		dev_err(&dev->pdev->dev, "L2 base not mapped\n");
		return NULL;
	}

	dsctl = (struct device_sys_ctl_t *)dev->vl2base;
	if (dsctl->datastream_mem_ref.magic != SYSCTL_DATASTREAM_AREA_MAGIC) {
		dev_dbg(&dev->pdev->dev,
			"Datastream area not available (firmware not running)\n");
		return NULL;
	}
	memref = &dsctl->datastream_mem_ref;

	if (memref->size == 0 || memref->offset == 0) {
		dev_dbg(&dev->pdev->dev,
			"Datastream area not initialized (size=%u, offset=%llu)\n",
			memref->size, memref->offset);
		return NULL;
	}

	return (void __iomem *)dev->vl2base + memref->offset;
}

static int fwtrace_try_attach_datastream(struct axl_pcie_aipu_dev *dev);

/**
 * get_data_stream - Get pointer to data_stream for a trace source
 * @base: device_datastream base pointer
 * @source: Trace source (stream_source_t value used directly as firmware index)
 *
 * Uses source_to_index[] lookup to find the right stream entry.
 * Returns NULL if the source is not available on this device.
 */
static void __iomem *get_data_stream(void __iomem *base, stream_source_t source)
{
	uint8_t stream_src =
		(uint8_t)source; /* stream_source_t IS the firmware index */
	uint8_t idx;
	uint32_t num_streams;

	num_streams = ioread32(base + DATASTREAM_NUM_STREAMS_OFFSET);
	idx = ioread8(base + DATASTREAM_SOURCE_INDEX_OFFSET + stream_src);

	if (idx >= num_streams)
		return NULL;

	return base + DATASTREAM_STREAM_ARRAY_OFFSET + (idx * DATA_STREAM_SIZE);
}

/**
 * fwtrace_dma_read - DMA read from device L2/SPM into fwtrace DMA-coherent buffer
 * @dev:        Device structure
 * @dev_addr:   Device-side AXI address (buf_ref.addr + read_pos)
 *              Points to device L2 SRAM (Metis/Omega/Alpha) or SPM (Europa)
 * @size:       Bytes to transfer
 * @dst_offset: Byte offset into consumer->dma_buffer for the destination
 *
 * Uses the common EDMA/HDMA abstraction (axl_aipu_dma_job_submit) to perform
 * a synchronous DMA read. Called directly from fwtrace_consume_work workqueue
 * context, so axl_aipu_dma_job_submit() is called directly (not re-queued).
 *
 * Returns 0 on success, negative error code on failure.
 */
static int fwtrace_dma_read(struct axl_pcie_aipu_dev *dev, u64 dev_addr,
			    size_t size, size_t dst_offset)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct dma_wrk wrk = {};
	struct dma_queue_ctrl *dma_ctrl;
	struct scatterlist sg;
	struct sg_table sgt;
	int channel;

	channel = axl_aipu_find_free_dma_channel(dev, DMABUF_XFER_FLAG_READ);
	dma_ctrl = &dev->dma_rdqc[channel];
	dev_dbg(&dev->pdev->dev, "fwtrace DMA read (addr=0x%llx size=%zu)\n",
		dev_addr, size);

	/*
	 * Build a single-entry sg_table from the DMA-coherent fwtrace buffer.
	 * The buffer is already mapped by dma_alloc_coherent so we set the DMA
	 * address directly, skipping dma_map_sg.
	 */
	sg_init_one(&sg, (u8 *)consumer->dma_buffer + dst_offset, size);
	sg_dma_address(&sg) = consumer->dma_handle + dst_offset;
	sg_dma_len(&sg) = size;
	sgt.sgl = &sg;
	sgt.nents = sgt.orig_nents = 1;

	kref_init(&wrk.refcount);
	wrk.axldev = dev;
	wrk.sctx = NULL;
	wrk.channel = channel;
	/* READ from device to host: MSI ID follows PMSI_DMA_WR_CH0 mapping */
	wrk.id = PMSI_DMA_WR_CH0 + channel;
	wrk.timeout = get_timeout_ms(dma_timeout);
	wrk.axi = dev_addr;
	wrk.table = &sgt;
	wrk.offset = 0;
	wrk.num_sgt = 0;
	wrk.size = size;
	wrk.flags = DMABUF_XFER_FLAG_READ;
	wrk.qctrl = dma_ctrl;
	wrk.dmabuf = NULL;
	wrk.ktime = ktime_get();
	init_completion(&wrk.done);

	atomic_inc(&dma_ctrl->count);
	dma_ctrl->num_xfer++;
	dma_ctrl->bytes_xfer += size;
	dma_ctrl->size = size;

	/* Submit synchronously — we are already running in workqueue context */
	axl_aipu_dma_job_submit(dev, &wrk);

	atomic_dec(&dma_ctrl->count);

	if (wrk.status)
		dev_warn(&dev->pdev->dev,
			 "fwtrace DMA read failed (addr=0x%llx size=%zu): %d\n",
			 dev_addr, size, wrk.status);

	return wrk.status;
}

/* ------------------------------------------------------------------ */
/* Internal types for ring buffer consumption                           */
/* ------------------------------------------------------------------ */

struct ring_data {
	uint32_t read_pos;
	uint32_t write_pos;
	uint32_t capacity;
	uint64_t buf_addr;
	void __iomem *stream;
};

struct chunk_layout {
	size_t first; /* bytes from read_pos to end-of-ring (or write_pos) */
	size_t second; /* bytes from ring start to write_pos (wrap case) */
	size_t total;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/**
 * fwtrace_read_ring - Read and validate ring buffer state from device
 *
 * Return: 0 on success, -ENODATA if empty, -ENODEV/-EINVAL on error
 */
static int fwtrace_read_ring(struct axl_pcie_aipu_dev *dev,
			     stream_source_t source, struct ring_data *rd)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (!consumer->datastream_base) {
		dev_warn(&dev->pdev->dev, "Datastream base not available\n");
		return -ENODEV;
	}

	rd->stream = get_data_stream(consumer->datastream_base, source);
	if (!rd->stream) {
		dev_err(&dev->pdev->dev, "Source %d not in datastream table\n",
			source);
		return -ENODEV;
	}

	rd->read_pos = ioread32(rd->stream + DS_CTRL_READ_POS);
	rd->write_pos = ioread32(rd->stream + DS_CTRL_WRITE_POS);
	rd->capacity = ioread32(rd->stream + DS_CTRL_CAPACITY);
	rd->buf_addr = readq(rd->stream + DS_BUF_REF_ADDR);

	dev_dbg(&dev->pdev->dev,
		"fwtrace src=%d: read=%u write=%u cap=%u buf=0x%llx\n", source,
		rd->read_pos, rd->write_pos, rd->capacity, rd->buf_addr);

	if (rd->read_pos == rd->write_pos)
		return -ENODATA;

	if (rd->capacity == 0) {
		dev_warn(&dev->pdev->dev,
			 "fwtrace src=%d: zero capacity ring buffer\n", source);
		return -EINVAL;
	}

	return 0;
}

/**
 * fwtrace_layout_chunks - Compute wrap-aware chunk sizes, clamped to max_size
 */
static void fwtrace_layout_chunks(const struct ring_data *rd, size_t max_size,
				  struct chunk_layout *cl)
{
	if (rd->write_pos > rd->read_pos) {
		cl->first = rd->write_pos - rd->read_pos;
		cl->second = 0;
	} else {
		cl->first = rd->capacity - rd->read_pos;
		cl->second = rd->write_pos;
	}

	cl->total = cl->first + cl->second;
	if (cl->total <= max_size)
		return;

	/* Clamp: fill first chunk before trimming second */
	if (cl->first >= max_size) {
		cl->first = max_size;
		cl->second = 0;
	} else {
		cl->second = max_size - cl->first;
	}
	cl->total = cl->first + cl->second;
}

/**
 * fwtrace_pio_copy - Copy ring buffer chunks to dst using memcpy_fromio
 */
static void fwtrace_pio_copy(struct axl_pcie_aipu_dev *dev,
			     const struct ring_data *rd,
			     const struct chunk_layout *cl, void *dst)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	uint64_t bar_off = rd->buf_addr - consumer->hdif_base;
	void __iomem *ring_base = dev->vl2base + bar_off;

	if (cl->first > 0)
		memcpy_fromio(dst, ring_base + rd->read_pos, cl->first);
	if (cl->second > 0)
		memcpy_fromio(dst + cl->first, ring_base, cl->second);
}

/**
 * fwtrace_dma_transfer - DMA transfer with automatic PIO fallback
 *
 * Attempts DMA for both chunks. Falls back to fwtrace_pio_copy() if
 * any DMA call fails.
 */
static void fwtrace_dma_transfer(struct axl_pcie_aipu_dev *dev,
				 stream_source_t source,
				 const struct ring_data *rd,
				 const struct chunk_layout *cl)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	bool ok = true;

	if (cl->first > 0)
		ok = fwtrace_dma_read(dev, rd->buf_addr + rd->read_pos,
				      cl->first, 0) == 0;
	if (ok && cl->second > 0)
		ok = fwtrace_dma_read(dev, rd->buf_addr, cl->second,
				      cl->first) == 0;
	if (ok)
		return;

	dev_warn(&dev->pdev->dev,
		 "fwtrace src=%d: DMA failed, falling back to PIO\n", source);
	fwtrace_pio_copy(dev, rd, cl, consumer->dma_buffer);
}

/**
 * fwtrace_source_is_binary - Return true if firmware reports STREAM_TYPE_BINARY
 *
 * Reads the type field from the firmware's data_stream_t directly, so it works
 * correctly for any device configuration without hardcoded source ranges.
 */
static bool fwtrace_source_is_binary(struct axl_pcie_aipu_dev *dev,
				     stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	void __iomem *stream;

	if (!consumer->datastream_base)
		return false;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return false;

	return ioread32(stream + DS_TYPE_OFFSET) == STREAM_TYPE_BINARY;
}

/**
 * fwtrace_fetch_to_buf - Transfer ring data into consumer->dma_buffer
 *
 * Selects DMA (binary sources above threshold) or PIO transparently.
 */
static void fwtrace_fetch_to_buf(struct axl_pcie_aipu_dev *dev,
				 stream_source_t source,
				 const struct ring_data *rd,
				 const struct chunk_layout *cl)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (fwtrace_source_is_binary(dev, source) &&
	    cl->total >= fwtrace_dma_threshold) {
		dev_dbg(&dev->pdev->dev,
			"fwtrace DMA src=%d: addr=0x%llx first=%zu second=%zu\n",
			source, rd->buf_addr, cl->first, cl->second);
		fwtrace_dma_transfer(dev, source, rd, cl);
	} else {
		dev_dbg(&dev->pdev->dev,
			"fwtrace PIO src=%d: first=%zu second=%zu\n", source,
			cl->first, cl->second);
		fwtrace_pio_copy(dev, rd, cl, consumer->dma_buffer);
	}
}

/**
 * fwtrace_broadcast_to_sessions - Fan-out dma_buffer content to all kfifos
 */
static void fwtrace_broadcast_to_sessions(struct axl_pcie_aipu_dev *dev,
					  stream_source_t source, size_t nbytes)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct fwtrace_buffer *kbuf = &consumer->buffers[source];
	struct fwtrace_session *sess;
	unsigned long sflags;

	spin_lock_irqsave(&kbuf->sessions_lock, sflags);
	list_for_each_entry(sess, &kbuf->sessions, list)
	{
		unsigned int written = kfifo_in_spinlocked(
			&sess->fifo, consumer->dma_buffer, nbytes, &sess->lock);
		if (written < nbytes)
			atomic64_add(nbytes - written, &sess->overruns);
		wake_up_interruptible(&sess->wait_queue);
	}
	spin_unlock_irqrestore(&kbuf->sessions_lock, sflags);

	atomic64_add(nbytes, &kbuf->total_bytes);
}

/**
 * fwtrace_commit_read_pos - Write updated read position back to device ring
 */
static void fwtrace_commit_read_pos(const struct ring_data *rd,
				    const struct chunk_layout *cl)
{
	uint32_t new_pos = cl->second > 0 ? rd->write_pos :
					    rd->read_pos + (uint32_t)cl->first;

	iowrite32(new_pos, rd->stream + DS_CTRL_READ_POS);
	wmb();
}

/**
 * fwtrace_consume_data - Consume one batch of data from a device ring buffer
 * @dev: Device structure
 * @source: Trace source to consume from
 *
 * Reads data from the device ring buffer into consumer->dma_buffer,
 * broadcasts to all active sessions, then advances the ring read pointer.
 * Called from both the MSI workqueue handler and the polling timer.
 */
static void fwtrace_consume_data(struct axl_pcie_aipu_dev *dev,
				 stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct ring_data rd;
	struct chunk_layout cl;

	if (consumer->dying)
		return;

	if (fwtrace_read_ring(dev, source, &rd) < 0)
		return;

	fwtrace_layout_chunks(&rd, consumer->dma_buffer_size, &cl);
	fwtrace_fetch_to_buf(dev, source, &rd, &cl);
	fwtrace_broadcast_to_sessions(dev, source, cl.total);
	fwtrace_commit_read_pos(&rd, &cl);

	dev_dbg(&dev->pdev->dev, "fwtrace src=%d: consumed %zu bytes\n", source,
		cl.total);
}

/**
 * fwtrace_consume_work - Workqueue handler for MSI-triggered consumption
 * @work: Work structure
 *
 * Wrapper around fwtrace_consume_data for MSI interrupt handling.
 */
static void fwtrace_consume_work(struct work_struct *work)
{
	struct fwtrace_work *fwwork =
		container_of(work, struct fwtrace_work, work);
	fwtrace_consume_data(fwwork->dev, fwwork->source);
}

/**
 * fwtrace_poll_work - Polling timer handler
 * @work: Delayed work structure
 *
 * Dispatches a per-source work item for each active trace source, then
 * reschedules itself. The actual data consumption (DMA/PIO) happens in
 * fwtrace_consume_work, so this handler returns quickly without blocking
 * on PCIe DMA completions.
 */
static void fwtrace_poll_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fwtrace_consumer *consumer =
		container_of(dwork, struct fwtrace_consumer, poll_work);
	struct axl_pcie_aipu_dev *dev =
		container_of(consumer, struct axl_pcie_aipu_dev, fwtrace);
	int i;
	bool has_active_sessions = false;

	if (!consumer->consumer_enabled || consumer->dying)
		return;

	/* Dispatch a work item per active source — queue_work is a no-op if
	 * the item is already queued (e.g. a concurrent MSI fired first).
	 */
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		struct fwtrace_buffer *kbuf = &consumer->buffers[i];

		if (atomic_read(&kbuf->session_count) > 0)
			queue_work(consumer->wq, &consumer->work[i].work);
	}

	/*
	 * Re-check session counts right before rescheduling to close the
	 * race between the check at the top of the loop and a concurrent
	 * axl_fwtrace_close_session(): if all sessions have closed while
	 * we were consuming data, stop now instead of rescheduling.
	 */
	has_active_sessions = false;
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		if (atomic_read(&consumer->buffers[i].session_count) > 0) {
			has_active_sessions = true;
			break;
		}
	}

	if (has_active_sessions && consumer->poll_interval_ms > 0) {
		schedule_delayed_work(
			&consumer->poll_work,
			msecs_to_jiffies(consumer->poll_interval_ms));
	} else {
		dev_dbg(&dev->pdev->dev,
			"fwtrace poll: stopping (no active sessions)\n");
	}
}

/**
 * fwtrace_start_polling - Start polling timer if not already running
 * @consumer: Consumer structure
 *
 * Starts the polling timer if there are active sessions and polling is enabled.
 */
static void fwtrace_start_polling(struct fwtrace_consumer *consumer)
{
	struct axl_pcie_aipu_dev *dev =
		container_of(consumer, struct axl_pcie_aipu_dev, fwtrace);

	if (consumer->poll_interval_ms == 0) {
		dev_dbg(&dev->pdev->dev, "fwtrace: polling disabled\n");
		return;
	}

	if (!consumer->consumer_enabled) {
		dev_dbg(&dev->pdev->dev,
			"fwtrace: consumer not enabled, skip poll start\n");
		return;
	}

	dev_dbg(&dev->pdev->dev, "fwtrace: starting polling (interval=%u ms)\n",
		consumer->poll_interval_ms);

	/* Schedule initial poll */
	schedule_delayed_work(&consumer->poll_work,
			      msecs_to_jiffies(consumer->poll_interval_ms));
}

/**
 * fwtrace_stop_polling - Stop polling timer if no active sessions
 * @consumer: Consumer structure
 *
 * Checks if any source has active sessions. If not, cancels the polling timer.
 */
static void fwtrace_stop_polling(struct fwtrace_consumer *consumer)
{
	int i;
	bool has_active_sessions = false;

	/* Check if any source has active sessions */
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		if (atomic_read(&consumer->buffers[i].session_count) > 0) {
			has_active_sessions = true;
			break;
		}
	}

	/* If no active sessions, cancel polling */
	if (!has_active_sessions) {
		cancel_delayed_work_sync(&consumer->poll_work);
	}
}

/**
 * axl_fwtrace_init - Initialize firmware trace consumer
 * @dev: Device structure
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_init(struct axl_pcie_aipu_dev *dev)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	int i, ret;

	BUILD_BUG_ON(FWTRACE_MAX_VMSI != PMSI_MAX);

	if (!fwtrace_enabled) {
		dev_dbg(&dev->pdev->dev, "Firmware trace consumer disabled\n");
		return 0;
	}

	dev_dbg(&dev->pdev->dev, "Initializing firmware trace consumer\n");

	/* Allocate DMA buffer for transfers */
	consumer->dma_buffer_size = fwtrace_dma_buffer_size;
	consumer->dma_buffer =
		dma_alloc_coherent(&dev->pdev->dev, consumer->dma_buffer_size,
				   &consumer->dma_handle, GFP_KERNEL);
	if (!consumer->dma_buffer) {
		dev_err(&dev->pdev->dev, "Failed to allocate DMA buffer\n");
		return -ENOMEM;
	}

	/* Initialize per-source buffers */
	consumer->kfifo_size = fwtrace_kfifo_size;
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		struct fwtrace_buffer *kbuf = &consumer->buffers[i];

		atomic_set(&kbuf->session_count, 0);
		atomic64_set(&kbuf->total_bytes, 0);
		atomic64_set(&kbuf->overruns, 0);
		INIT_LIST_HEAD(&kbuf->sessions);
		spin_lock_init(&kbuf->sessions_lock);
	}

	/* Create workqueue for deferred processing */
	consumer->wq = alloc_workqueue(
		"fwtrace_%s", WQ_MEM_RECLAIM | WQ_UNBOUND, 1, dev->name);
	if (!consumer->wq) {
		dev_err(&dev->pdev->dev, "Failed to create workqueue\n");
		ret = -ENOMEM;
		goto err_dma;
	}

	/* Initialize work structures */
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		struct fwtrace_work *fwwork = &consumer->work[i];
		INIT_WORK(&fwwork->work, fwtrace_consume_work);
		fwwork->source = (stream_source_t)i;
		fwwork->dev = dev;
	}

	/* Initialize polling timer */
	INIT_DELAYED_WORK(&consumer->poll_work, fwtrace_poll_work);
	consumer->poll_interval_ms = fwtrace_poll_interval;

	consumer->consumer_enabled = true;

	/* Firmware may already be running if the driver is reloaded — attach
	 * eagerly so debugfs stats are populated without needing an enable(). */
	fwtrace_try_attach_datastream(dev);

	dev_info(
		&dev->pdev->dev,
		"Firmware trace consumer initialized (kfifo=%zu KB, dma=%zu KB, poll=%u ms)\n",
		consumer->kfifo_size / 1024, consumer->dma_buffer_size / 1024,
		consumer->poll_interval_ms);

	return 0;

err_dma:
	if (consumer->dma_buffer) {
		dma_free_coherent(&dev->pdev->dev, consumer->dma_buffer_size,
				  consumer->dma_buffer, consumer->dma_handle);
	}

	return ret;
}

/**
 * axl_aipu_fwtrace_kill_sessions - Set dying flag and wake all blocked readers
 * @dev: Device structure
 *
 * Marks the fwtrace consumer as dying and wakes every process blocked in
 * axl_fwtrace_read() so it returns -ENODEV. Must be called before freeing
 * any resources that blocked readers may reference.
 */
void axl_aipu_fwtrace_kill_sessions(struct axl_pcie_aipu_dev *dev)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	int i;

	if (!consumer->consumer_enabled)
		return;

	consumer->dying = true;

	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		struct fwtrace_buffer *kbuf = &consumer->buffers[i];
		struct fwtrace_session *sess;
		unsigned long flags;

		spin_lock_irqsave(&kbuf->sessions_lock, flags);
		list_for_each_entry(sess, &kbuf->sessions, list)
			wake_up_all(&sess->wait_queue);
		spin_unlock_irqrestore(&kbuf->sessions_lock, flags);
	}
}

/**
 * axl_fwtrace_cleanup - Cleanup firmware trace consumer
 * @dev: Device structure
 */
void axl_fwtrace_cleanup(struct axl_pcie_aipu_dev *dev)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (!consumer->consumer_enabled)
		return;

	dev_info(&dev->pdev->dev, "Cleaning up firmware trace consumer\n");

	/* Wake all blocked readers before freeing resources. */
	axl_aipu_fwtrace_kill_sessions(dev);

	/* Cancel polling timer */
	cancel_delayed_work_sync(&consumer->poll_work);

	/* Destroy workqueue */
	if (consumer->wq) {
		flush_workqueue(consumer->wq);
		destroy_workqueue(consumer->wq);
		consumer->wq = NULL;
	}

	/* Free DMA buffer */
	if (consumer->dma_buffer) {
		dma_free_coherent(&dev->pdev->dev, consumer->dma_buffer_size,
				  consumer->dma_buffer, consumer->dma_handle);
		consumer->dma_buffer = NULL;
	}

	consumer->consumer_enabled = false;
}

/**
 * axl_aipu_fwtrace_refresh - Re-attach to firmware datastream after firmware restart
 * @dev: Device structure
 *
 * Called from AXL_IOCTL_DYNMEM_LOAD after the firmware has restarted.
 * Invalidates datastream_base, l2_base, and the vmsi_to_source table so that
 * fwtrace_try_attach_datastream() will re-read them from the new firmware on
 * the next axl_fwtrace_enable() call.
 *
 * Active sessions are left intact; userspace is responsible for re-enabling
 * sources after firmware reload.
 */
void axl_aipu_fwtrace_refresh(struct axl_pcie_aipu_dev *dev)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (!consumer->consumer_enabled)
		return;

	/* Kill active sessions before invalidating firmware state. */
	axl_aipu_fwtrace_kill_sessions(dev);

	cancel_delayed_work_sync(&consumer->poll_work);

	/* Invalidate firmware-dependent state; fwtrace_try_attach_datastream
	 * will repopulate these on the next axl_fwtrace_enable() call. */
	consumer->datastream_base = NULL;
	consumer->hdif_base = 0;
	memset(consumer->vmsi_to_source, STREAM_SOURCE_RESERVED,
	       sizeof(consumer->vmsi_to_source));

	/* BARs are valid again — allow the fwtrace service to be re-used once
	 * firmware reloads and axl_fwtrace_enable() is called. */
	consumer->dying = false;

	dev_info(&dev->pdev->dev, "fwtrace: detached for firmware reload\n");
}

/**
 * axl_fwtrace_clear_buffer - Drain device ring into open sessions, then reset
 * @filp: File pointer (caller's session)
 * @source: Trace source to clear
 *
 * Drains any pending data from the device ring into all open session kfifos
 * BEFORE advancing read_pos. This ensures that concurrent sessions (e.g.
 * long-running axtrace processes) receive all data that was in the ring at
 * the time of the clear request.
 *
 * After draining, read_pos is advanced to write_pos so firmware can produce
 * new data. Only the calling session's kfifo is then reset so it does not
 * see the pre-clear data; other sessions keep what they received.
 *
 * Return: 0 on success, -ENODEV if not attached or source not found
 */
int axl_fwtrace_clear_buffer(struct file *filp, stream_source_t source)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct axl_pcie_aipu_dev *dev = ctx->axldev;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	unsigned long flags;
	void __iomem *stream;
	uint32_t write_pos;

	if (!consumer->consumer_enabled || !consumer->datastream_base)
		return -ENODEV;

	if (source >= STREAM_SOURCE_RESERVED)
		return -EINVAL;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return -ENODEV;

	/* Drain pending ring data into all open session kfifos before clearing.
	 * This prevents data loss for concurrent readers: they receive whatever
	 * was buffered on the device side at the moment of the clear request. */
	fwtrace_consume_data(dev, source);

	/* Advance read_pos to write_pos to free the ring for new firmware data.
	 * After fwtrace_consume_data() read_pos is already near write_pos, but
	 * set it explicitly to discard any bytes that could not fit in the DMA
	 * buffer in a single call. */
	write_pos = ioread32(stream + DS_CTRL_WRITE_POS);
	iowrite32(write_pos, stream + DS_CTRL_READ_POS);
	wmb();

	/* Reset only the calling session's kfifo so it starts fresh. Other
	 * sessions have already received the pre-clear data above. */
	if (ctx->fwtrace_mode && ctx->fwtrace_src == source) {
		spin_lock_irqsave(&ctx->fwtrace_session.lock, flags);
		kfifo_reset(&ctx->fwtrace_session.fifo);
		spin_unlock_irqrestore(&ctx->fwtrace_session.lock, flags);
	}

	dev_dbg(&dev->pdev->dev,
		"fwtrace: cleared buffer for source %d (write_pos=%u)\n",
		source, write_pos);
	return 0;
}

/**
 * axl_fwtrace_msi_handler - MSI interrupt handler for firmware trace
 * @dev: Device structure
 * @msi_index: Virtual MSI index from the VMSI dispatch loop
 */
void axl_fwtrace_msi_handler(struct axl_pcie_aipu_dev *dev, int msi_index)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	stream_source_t source;

	if (!consumer->consumer_enabled)
		return;

	source = (stream_source_t)consumer->vmsi_to_source[msi_index];
	if (source == STREAM_SOURCE_RESERVED)
		return;

	dev_dbg(&dev->pdev->dev, "fwtrace MSI wakeup: vmsi=%d source=%d (%s)\n",
		msi_index, source,
		fwtrace_source_is_binary(dev, source) ? "trace" : "log");

	queue_work(consumer->wq, &consumer->work[source].work);
}

/**
 * axl_fwtrace_hw_pmsi_handler - HW-sourced PMSI handler for firmware trace
 * @dev: Device structure
 * @pmsi_id: Physical MSI index that fired
 */
void axl_fwtrace_hw_pmsi_handler(struct axl_pcie_aipu_dev *dev, int pmsi_id)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	int i;

	if (!consumer->consumer_enabled)
		return;
	if (pmsi_id != PMSI_LOG && pmsi_id != PMSI_TRACE)
		return;

	dev_dbg(&dev->pdev->dev,
		"fwtrace HW MSI wakeup: pmsi=%d, queuing active sources\n",
		pmsi_id);

	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		struct fwtrace_buffer *kbuf = &consumer->buffers[i];

		if (atomic_read(&kbuf->session_count) > 0)
			queue_work(consumer->wq, &consumer->work[i].work);
	}
}

/**
 * axl_fwtrace_read - Read handler for trace sessions
 * @filp: File pointer
 * @buf: Userspace buffer
 * @count: Number of bytes to read
 * @ppos: File position (unused)
 *
 * Returns: Number of bytes read, or negative error code
 */
ssize_t axl_fwtrace_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *ppos)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct fwtrace_consumer *consumer = &ctx->axldev->fwtrace;
	unsigned int copied;
	int ret;

	if (!ctx->fwtrace_mode)
		return -EINVAL;

	/* Wait for data if none available (blocking mode) */
	if (filp->f_flags & O_NONBLOCK) {
		if (consumer->dying)
			return -ENODEV;
		if (kfifo_is_empty(&ctx->fwtrace_session.fifo))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(
			ctx->fwtrace_session.wait_queue,
			!kfifo_is_empty(&ctx->fwtrace_session.fifo) ||
				consumer->dying);
		if (ret)
			return ret;
		if (consumer->dying)
			return -ENODEV;
	}

	/* Read from kfifo to userspace */
	ret = kfifo_to_user(&ctx->fwtrace_session.fifo, buf, count, &copied);
	if (ret)
		return ret;

	return copied;
}

/**
 * axl_fwtrace_poll - Poll handler for trace sessions
 * @filp: File pointer
 * @wait: Poll table
 *
 * Returns: Poll mask (POLLIN | POLLRDNORM if data available)
 */
__poll_t axl_fwtrace_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct fwtrace_consumer *consumer = &ctx->axldev->fwtrace;
	__poll_t mask = 0;

	if (!ctx->fwtrace_mode)
		return POLLERR;

	poll_wait(filp, &ctx->fwtrace_session.wait_queue, wait);

	if (consumer->dying)
		return EPOLLHUP | EPOLLERR;

	if (!kfifo_is_empty(&ctx->fwtrace_session.fifo))
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

/**
 * axl_fwtrace_open_session - Open a trace session
 * @filp: File pointer
 * @source: Trace source to read
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_open_session(struct file *filp, stream_source_t source)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct axl_pcie_aipu_dev *dev = ctx->axldev;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct fwtrace_buffer *kbuf;
	unsigned long flags;
	int prev_count;
	int ret;

	if (source >= STREAM_SOURCE_RESERVED)
		return -EINVAL;

	if (!consumer->consumer_enabled)
		return -ENODEV;

	/* Cannot reopen if already in trace mode */
	if (ctx->fwtrace_mode)
		return -EBUSY;

	kbuf = &consumer->buffers[source];

	/* Allocate and initialize per-session state */
	ret = kfifo_alloc(&ctx->fwtrace_session.fifo, consumer->kfifo_size,
			  GFP_KERNEL);
	if (ret)
		return ret;

	spin_lock_init(&ctx->fwtrace_session.lock);
	init_waitqueue_head(&ctx->fwtrace_session.wait_queue);
	atomic64_set(&ctx->fwtrace_session.overruns, 0);

	/* Configure this file descriptor for trace reading */
	ctx->fwtrace_mode = true;
	ctx->fwtrace_src = source;

	/* Add to source's session list */
	spin_lock_irqsave(&kbuf->sessions_lock, flags);
	list_add(&ctx->fwtrace_session.list, &kbuf->sessions);
	spin_unlock_irqrestore(&kbuf->sessions_lock, flags);

	/* Increment session count and start polling if first session */
	prev_count = atomic_inc_return(&kbuf->session_count);
	if (prev_count == 1) {
		/* First session for this source - start polling timer */
		fwtrace_start_polling(consumer);
	}

	dev_dbg(&dev->pdev->dev,
		"fwtrace: opened session source=%d sessions=%d\n", source,
		prev_count);

	return 0;
}

/**
 * axl_fwtrace_close_session - Close a trace session
 * @filp: File pointer
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_close_session(struct file *filp)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct axl_pcie_aipu_dev *dev = ctx->axldev;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct fwtrace_buffer *kbuf;
	stream_source_t source;
	unsigned long flags;
	int new_count;

	if (!ctx->fwtrace_mode)
		return -EINVAL;

	source = ctx->fwtrace_src;
	kbuf = &consumer->buffers[source];

	/* Flush any tail data below the firmware MSI threshold before removing
	 * the session from the broadcast list, so it is still reachable by the
	 * fan-out loop in fwtrace_consume_data(). */
	if (!consumer->dying)
		fwtrace_consume_data(dev, source);

	/* Remove from source's session list */
	spin_lock_irqsave(&kbuf->sessions_lock, flags);
	list_del(&ctx->fwtrace_session.list);
	spin_unlock_irqrestore(&kbuf->sessions_lock, flags);

	kfifo_free(&ctx->fwtrace_session.fifo);

	/* Decrement session count and stop polling if no more sessions */
	new_count = atomic_dec_return(&kbuf->session_count);
	if (new_count == 0) {
		/* No more sessions for this source - maybe stop polling */
		fwtrace_stop_polling(consumer);
	}

	ctx->fwtrace_mode = false;

	dev_dbg(&dev->pdev->dev,
		"Closed trace session for source %d (sessions=%d)\n", source,
		new_count);

	return 0;
}

/**
 * axl_fwtrace_get_stats - Get statistics for a session
 * @filp: File pointer
 * @stats: Output statistics structure
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_get_stats(struct file *filp, struct fwtrace_stats *stats)
{
	struct sysctrl_ctx *ctx = filp->private_data;
	struct axl_pcie_aipu_dev *dev = ctx->axldev;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	struct fwtrace_buffer *kbuf;

	if (!ctx->fwtrace_mode)
		return -EINVAL;

	kbuf = &consumer->buffers[ctx->fwtrace_src];

	stats->total_bytes = atomic64_read(&kbuf->total_bytes);
	stats->overruns = atomic64_read(&ctx->fwtrace_session.overruns);
	stats->available_bytes = kfifo_len(&ctx->fwtrace_session.fifo);

	return 0;
}

/**
 * fwtrace_set_dev_enabled - Write device-side ctrl.enabled bit for a source
 * @consumer: Consumer structure
 * @source: Trace source
 * @enable: true to enable, false to disable
 *
 * Writes the enabled bit in the device ring buffer control register so the
 * firmware starts (or stops) generating trace data for this source.
 */
static void fwtrace_set_dev_enabled(struct fwtrace_consumer *consumer,
				    stream_source_t source, bool enable)
{
	void __iomem *stream;
	uint32_t ctrl_reg;

	if (!consumer->datastream_base)
		return;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return;

	ctrl_reg = ioread32(stream + DS_CTRL_REG);
	if (enable)
		ctrl_reg |= CTRL_REG_ENABLED_BIT;
	else
		ctrl_reg &= ~CTRL_REG_ENABLED_BIT;
	iowrite32(ctrl_reg, stream + DS_CTRL_REG);
}

/**
 * axl_fwtrace_get_dev_enabled - Read device-side ctrl.enabled bit for a source
 * @dev: Device structure
 * @source: Trace source
 *
 * Returns true if the firmware ring buffer ctrl.enabled bit is set.
 */
bool axl_fwtrace_get_dev_enabled(struct axl_pcie_aipu_dev *dev,
				 stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	void __iomem *stream;

	if (!consumer->datastream_base || source >= STREAM_SOURCE_RESERVED)
		return false;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return false;

	return !!(ioread32(stream + DS_CTRL_REG) & CTRL_REG_ENABLED_BIT);
}

/**
 * axl_fwtrace_get_ring_state - Read device ring buffer positions for a source
 * @dev: Device structure
 * @source: Trace source
 * @state: Output ring buffer state
 */
int axl_fwtrace_get_ring_state(struct axl_pcie_aipu_dev *dev,
			       stream_source_t source,
			       struct fwtrace_ring_state *state)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	void __iomem *stream;
	uint32_t ctrl_reg;

	if (!consumer->datastream_base || source >= STREAM_SOURCE_RESERVED)
		return -ENODEV;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return -ENODEV;

	state->read_pos = ioread32(stream + DS_CTRL_READ_POS);
	state->write_pos = ioread32(stream + DS_CTRL_WRITE_POS);
	state->capacity = ioread32(stream + DS_CTRL_CAPACITY);
	ctrl_reg = ioread32(stream + DS_CTRL_REG);
	state->msi = ctrl_reg & CTRL_REG_MSI_MASK;

	return 0;
}

/**
 * axl_fwtrace_source_on_dev - Return true if source is supported by firmware
 * @dev: Device structure
 * @source: Trace source
 *
 * Reads source_to_index[source] from the firmware's device_datastream_t and
 * checks it against DATA_STREAM_NOT_SUPPORTED (= STREAM_SOURCE_RESERVED).
 * Returns false also when the datastream area is not yet attached.
 */
bool axl_fwtrace_source_on_dev(struct axl_pcie_aipu_dev *dev,
			       stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	uint8_t idx;

	if (!consumer->datastream_base || source >= STREAM_SOURCE_RESERVED)
		return false;

	idx = ioread8(consumer->datastream_base +
		      DATASTREAM_SOURCE_INDEX_OFFSET + (uint8_t)source);
	return idx != (uint8_t)STREAM_SOURCE_RESERVED;
}

/**
 * axl_fwtrace_get_stream_name - Read the firmware-assigned name for a source
 * @dev: Device structure
 * @source: Trace source
 * @buf: Output buffer (at least DS_NAME_LEN bytes)
 * @len: Buffer length
 *
 * Copies the null-terminated name from the firmware's data_stream_t.name
 * field into buf. Returns -ENODEV if the source is not in the firmware table.
 */
int axl_fwtrace_get_stream_name(struct axl_pcie_aipu_dev *dev,
				stream_source_t source, char *buf, size_t len)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	void __iomem *stream;
	size_t copy_len;

	if (!consumer->datastream_base || source >= STREAM_SOURCE_RESERVED)
		return -ENODEV;

	stream = get_data_stream(consumer->datastream_base, source);
	if (!stream)
		return -ENODEV;

	copy_len = min(len, (size_t)DS_NAME_LEN);
	memcpy_fromio(buf, stream + DS_NAME_OFFSET, copy_len);
	buf[copy_len - 1] = '\0';

	return 0;
}

/**
 * fwtrace_try_attach_datastream - Populate datastream_base and vmsi table if not yet done
 *
 * Called lazily from axl_fwtrace_enable(). At probe time, Europa firmware
 * may not have written datastream_mem_ref yet; by the time userspace calls
 * enable(), the firmware is guaranteed to be running.
 *
 * Return: 0 if datastream_base is valid, -ENODEV if still not available.
 */
static int fwtrace_try_attach_datastream(struct axl_pcie_aipu_dev *dev)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	int i;

	/* datastream_base is a fixed L2 region — attach once. */
	if (!consumer->datastream_base) {
		/* Both hdif_base and datastream_base require firmware to be running.
		 * Read them together so they are always consistent. */
		consumer->hdif_base = axl_aipu_get_hdif_base(dev);
		dev_dbg(&dev->pdev->dev, "fwtrace: hdif base = 0x%llx\n",
			consumer->hdif_base);

		consumer->datastream_base = get_datastream_base(dev);
		if (!consumer->datastream_base) {
			consumer->hdif_base = 0;
			dev_warn(
				&dev->pdev->dev,
				"fwtrace: datastream area not available yet\n");
			return -ENODEV;
		}

		dev_info(&dev->pdev->dev, "fwtrace: datastream attached\n");
	}

	/* Rebuild VMSI -> source table on every enable() call. Firmware writes
	 * ctrl_reg MSI fields lazily after datastream_base becomes available,
	 * so the table can be stale if it was built before firmware finished
	 * configuring the VMSI assignments. */
	memset(consumer->vmsi_to_source, STREAM_SOURCE_RESERVED,
	       sizeof(consumer->vmsi_to_source));
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		void __iomem *stream = get_data_stream(
			consumer->datastream_base, (stream_source_t)i);
		uint32_t ctrl_reg;
		int msi;

		if (!stream)
			continue;
		ctrl_reg = ioread32(stream + DS_CTRL_REG);
		msi = ctrl_reg & CTRL_REG_MSI_MASK;
		if (msi < (int)ARRAY_SIZE(consumer->vmsi_to_source)) {
			consumer->vmsi_to_source[msi] = (uint8_t)i;
			dev_dbg(&dev->pdev->dev,
				"fwtrace: source %d -> VMSI %d\n", i, msi);
		}
	}

	return 0;
}

/**
 * axl_fwtrace_enable - Enable a trace source
 * @dev: Device structure
 * @source: Trace source to enable
 *
 * Sets both the kernel consumer flag and the device-side ring buffer
 * ctrl.enabled bit so firmware starts generating trace data.
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_enable(struct axl_pcie_aipu_dev *dev, stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (source >= STREAM_SOURCE_RESERVED)
		return -EINVAL;

	if (!consumer->consumer_enabled)
		return -ENODEV;

	if (fwtrace_try_attach_datastream(dev) < 0)
		return -ENODEV;

	fwtrace_set_dev_enabled(consumer, source, true);

	dev_dbg(&dev->pdev->dev, "Enabled trace source %d\n", source);

	return 0;
}

/**
 * axl_fwtrace_disable - Disable a trace source
 * @dev: Device structure
 * @source: Trace source to disable
 *
 * Clears both the kernel consumer flag and the device-side ring buffer
 * ctrl.enabled bit so firmware stops generating trace data.
 *
 * Returns: 0 on success, negative error code on failure
 */
int axl_fwtrace_disable(struct axl_pcie_aipu_dev *dev, stream_source_t source)
{
	struct fwtrace_consumer *consumer = &dev->fwtrace;

	if (source >= STREAM_SOURCE_RESERVED)
		return -EINVAL;

	if (!consumer->consumer_enabled)
		return -ENODEV;

	fwtrace_set_dev_enabled(consumer, source, false);

	dev_dbg(&dev->pdev->dev, "Disabled trace source %d\n", source);

	return 0;
}
