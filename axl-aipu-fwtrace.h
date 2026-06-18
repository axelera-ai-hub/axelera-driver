/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Axelera AI
 * Firmware log/trace kernel consumer interface
 */

#ifndef _AXL_AIPU_FWTRACE_H_
#define _AXL_AIPU_FWTRACE_H_

#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/list.h>

struct axl_pcie_aipu_dev;

/*
 * Firmware trace source identifiers use stream_source_t from axl-aipu.h
 * directly. The values are the firmware's own device_datastream_t indices,
 * so no conversion table is needed between the IOCTL interface and the
 * device memory lookup.
 *
 * STREAM_SOURCE_RESERVED (0xFF) is the sentinel for "no source assigned".
 * STREAM_SOURCE_MAX (256) is used to size per-source arrays.
 *
 * On Metis/Omega/Alpha devices, sources for cores > 3 and all PVE
 * sources will return ENODEV as the firmware source_to_index[] table
 * marks them as DATA_STREAM_NOT_SUPPORTED.
 */

/** DS_NAME_LEN - Length of the firmware-assigned stream name (DATA_STREAM_NAME_MAX_LEN) */
#define DS_NAME_LEN 32

/**
 * FWTRACE_DMA_THRESHOLD - Minimum transfer size to use DMA instead of PIO
 *
 * For binary trace sources, transfers larger than this threshold use the
 * EDMA/HDMA engine. Below this size (or for log sources) PIO memcpy_fromio
 * is used since DMA setup overhead would exceed the transfer time.
 */
#define FWTRACE_DMA_THRESHOLD 1024

/*
 * Maximum number of virtual MSI IDs tracked for fwtrace dispatch.
 * Must match PMSI_MAX in axl-aipu.h — enforced by BUILD_BUG_ON in
 * axl-aipu-fwtrace.c (axl-aipu.h cannot be included here due to
 * circular dependency: axl-aipu.h includes axl-aipu-fwtrace.h).
 */
#define FWTRACE_MAX_VMSI 32

/**
 * struct fwtrace_buffer - Per-source kernel buffer
 *
 * Each log/trace source has one shared kernel buffer that stores
 * data consumed from the device ring buffer. Multiple sessions
 * can read from this buffer independently.
 */
struct fwtrace_buffer {
	atomic_t session_count; /* Number of active sessions */
	atomic64_t total_bytes; /* Total bytes consumed from device */
	atomic64_t overruns; /* Device ring -> kfifo fan-out overruns */
	struct list_head sessions; /* List of active fwtrace_session */
	spinlock_t sessions_lock; /* Protects sessions list */
};

/**
 * struct fwtrace_work - Work structure for each source
 *
 * Each trace source has its own work_struct so the workqueue
 * can identify which source triggered the work.
 */
struct fwtrace_work {
	struct work_struct work;
	stream_source_t source;
	struct axl_pcie_aipu_dev *dev;
};

/**
 * struct fwtrace_consumer - Per-device firmware trace consumer
 *
 * This is the main structure that manages firmware trace consumption
 * for a device. It contains kernel buffers for all trace sources,
 * session tracking, and resources for DMA transfers.
 */
struct fwtrace_consumer {
	/* Per-source kernel buffers (shared across all sessions).
	 * Indexed directly by stream_source_t value. */
	struct fwtrace_buffer buffers[STREAM_SOURCE_MAX];

	/* Device memory pointers (mmapped L2) */
	void __iomem *datastream_base; /* Mapped device_datastream_t */
	uint64_t hdif_base; /* Physical base of the host-device interface region;
				 * subtracted from buf_ref.addr to get BAR offset */

	/* DMA buffer for transfers (reusable across all sources) */
	void *dma_buffer; /* Kernel virtual address */
	dma_addr_t dma_handle; /* DMA address */
	size_t dma_buffer_size; /* Buffer size (e.g., 256KB) */

	/* Workqueue for deferred processing */
	struct workqueue_struct *wq;
	struct fwtrace_work work[STREAM_SOURCE_MAX];

	/* Polling timer (checks ring buffers periodically) */
	struct delayed_work poll_work;
	unsigned int poll_interval_ms; /* Polling interval in ms */

	/* VMSI -> fwtrace source lookup (populated during init from ctrl.reg)
	 * Index is the firmware-assigned VMSI ID (0..PMSI_MAX-1).
	 * Value is the stream_source_t, or STREAM_SOURCE_RESERVED if not fwtrace.
	 */
	uint8_t vmsi_to_source[FWTRACE_MAX_VMSI];

	/* Configuration */
	size_t kfifo_size; /* Per-source kernel buffer size */
	bool consumer_enabled; /* Global enable/disable */

	/* Set to true when the device is going away (driver removal) or the
	 * PCIe link goes down. Processes blocked in axl_fwtrace_read() wake up
	 * and return -ENODEV so they do not hold references into invalid BAR
	 * memory. Never cleared once set. */
	bool dying;
};

/**
 * fwtrace_vmsi_is_source - Return true if vmsi_id belongs to fwtrace
 * @consumer: Per-device fwtrace consumer
 * @vmsi_id: Virtual MSI index to test
 *
 * Uses the vmsi_to_source table built at init time from firmware ctrl.reg
 * fields. Works for all devices (Metis/Omega/Alpha/Europa) without
 * hardcoded ranges.
 */
static inline bool fwtrace_vmsi_is_source(struct fwtrace_consumer *consumer,
					  int vmsi_id)
{
	if (vmsi_id < 0 || vmsi_id >= (int)ARRAY_SIZE(consumer->vmsi_to_source))
		return false;
	return consumer->vmsi_to_source[vmsi_id] !=
	       (uint8_t)STREAM_SOURCE_RESERVED;
}

/* Function prototypes */

/**
 * axl_fwtrace_init - Initialize firmware trace consumer
 * @dev: Device structure
 *
 * Called during device probe to initialize the trace consumer.
 * Allocates buffers, creates workqueue, maps device memory.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_init(struct axl_pcie_aipu_dev *dev);

/**
 * axl_fwtrace_cleanup - Cleanup firmware trace consumer
 * @dev: Device structure
 *
 * Called during device removal to cleanup trace consumer.
 * Destroys workqueue, frees buffers, unmaps memory.
 */
void axl_fwtrace_cleanup(struct axl_pcie_aipu_dev *dev);

/**
 * axl_aipu_fwtrace_refresh - Re-attach to firmware datastream after firmware reload
 * @dev: Device structure
 *
 * Called after AXL_IOCTL_DYNMEM_LOAD to invalidate all firmware-dependent
 * state (datastream_base, hdif_base, vmsi_to_source table). The next call to
 * axl_fwtrace_enable() will re-attach lazily via fwtrace_try_attach_logtrace().
 */
void axl_aipu_fwtrace_refresh(struct axl_pcie_aipu_dev *dev);

/**
 * axl_aipu_fwtrace_kill_sessions - Set dying flag and wake all blocked readers
 * @dev: Device structure
 *
 * Marks the fwtrace consumer as dying and wakes every process blocked in
 * axl_fwtrace_read() so it returns -ENODEV. Call on link-down, device
 * reset, or driver removal before freeing any resources.
 */
void axl_aipu_fwtrace_kill_sessions(struct axl_pcie_aipu_dev *dev);

/**
 * axl_fwtrace_msi_handler - MSI interrupt handler for trace (FW-sourced path)
 * @dev: Device structure
 * @msi_index: VMSI index looked up from the VMSI_IRQ_EN scan
 *
 * Called from the FW-sourced path in the MSI interrupt handler when a
 * specific VMSI fires. Looks up the source and queues work.
 */
void axl_fwtrace_msi_handler(struct axl_pcie_aipu_dev *dev, int msi_index);

/**
 * axl_fwtrace_hw_pmsi_handler - MSI interrupt handler for trace (HW-sourced path)
 * @dev: Device structure
 * @pmsi_id: Physical MSI index that fired (e.g. PMSI_LOG, PMSI_TRACE)
 *
 * Called from the HW-sourced path in the MSI interrupt handler. In HW mode
 * the driver sees only the physical MSI index and no VMSI table is scanned,
 * so this function queues work for every active fwtrace source when
 * PMSI_LOG or PMSI_TRACE fires.
 */
void axl_fwtrace_hw_pmsi_handler(struct axl_pcie_aipu_dev *dev, int pmsi_id);

/**
 * axl_fwtrace_read - Read handler for trace sessions
 * @filp: File pointer
 * @buf: Userspace buffer
 * @count: Number of bytes to read
 * @ppos: File position (unused, streaming interface)
 *
 * Called from sysctrl_read() when file descriptor is in trace mode.
 * Reads from the kernel buffer (kfifo) for the session's trace source.
 *
 * Return: Number of bytes read, or negative error code
 */
ssize_t axl_fwtrace_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *ppos);

/**
 * axl_fwtrace_poll - Poll handler for trace sessions
 * @filp: File pointer
 * @wait: Poll table
 *
 * Called from sysctrl_poll() when file descriptor is in trace mode.
 * Checks if data is available in the kernel buffer.
 *
 * Return: Poll mask (POLLIN | POLLRDNORM if data available)
 */
__poll_t axl_fwtrace_poll(struct file *filp, struct poll_table_struct *wait);

/**
 * axl_fwtrace_open_session - Open a trace session
 * @filp: File pointer
 * @source: Trace source to read
 *
 * Called from IOCTL handler to configure file descriptor for trace reading.
 * Adds session to device's linked list.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_open_session(struct file *filp, stream_source_t source);

/**
 * axl_fwtrace_close_session - Close a trace session
 * @filp: File pointer
 *
 * Called from IOCTL handler or file release to cleanup trace session.
 * Removes session from device's linked list.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_close_session(struct file *filp);

/**
 * axl_fwtrace_get_stats - Get statistics for a session
 * @filp: File pointer
 * @stats: Output statistics structure
 *
 * Called from IOCTL handler to retrieve trace statistics.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_get_stats(struct file *filp, struct fwtrace_stats *stats);

/**
 * struct fwtrace_ring_state - Snapshot of device ring buffer positions
 */
struct fwtrace_ring_state {
	uint32_t read_pos;
	uint32_t write_pos;
	uint32_t capacity;
	uint32_t msi;
};

/**
 * axl_fwtrace_get_dev_enabled - Read device-side ctrl.enabled bit for a source
 * @dev: Device structure
 * @source: Trace source
 *
 * Return: true if the firmware ring buffer ctrl.enabled bit is set
 */
bool axl_fwtrace_get_dev_enabled(struct axl_pcie_aipu_dev *dev,
				 stream_source_t source);

/**
 * axl_fwtrace_source_on_dev - Return true if source is supported by firmware
 * @dev: Device structure
 * @source: Trace source
 *
 * Checks source_to_index[source] against DATA_STREAM_NOT_SUPPORTED directly.
 * Returns false if the datastream area is not yet attached.
 */
bool axl_fwtrace_source_on_dev(struct axl_pcie_aipu_dev *dev,
			       stream_source_t source);

/**
 * axl_fwtrace_get_stream_name - Read the firmware-assigned name for a source
 * @dev: Device structure
 * @source: Trace source
 * @buf: Output buffer (at least DS_NAME_LEN bytes)
 * @len: Buffer length
 *
 * Return: 0 on success, -ENODEV if source not in firmware table
 */
int axl_fwtrace_get_stream_name(struct axl_pcie_aipu_dev *dev,
				stream_source_t source, char *buf, size_t len);

/**
 * axl_fwtrace_get_ring_state - Read device ring buffer positions for a source
 * @dev: Device structure
 * @source: Trace source
 * @state: Output ring buffer state
 *
 * Return: 0 on success, -ENODEV if logtrace base not available
 */
int axl_fwtrace_get_ring_state(struct axl_pcie_aipu_dev *dev,
			       stream_source_t source,
			       struct fwtrace_ring_state *state);

/**
 * axl_fwtrace_enable - Enable a trace source
 * @dev: Device structure
 * @source: Trace source to enable
 *
 * Sets both the kernel consumer flag and the device-side ctrl.enabled bit.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_enable(struct axl_pcie_aipu_dev *dev, stream_source_t source);

/**
 * axl_fwtrace_disable - Disable a trace source
 * @dev: Device structure
 * @source: Trace source to disable
 *
 * Called from IOCTL handler to disable trace collection for a source.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_disable(struct axl_pcie_aipu_dev *dev, stream_source_t source);

/**
 * axl_fwtrace_clear_buffer - Align device ring read_pos to write_pos
 * @filp: File pointer (caller's session)
 * @source: Trace source to clear
 *
 * First drains any pending data from the device ring into all open session
 * kfifos, then sets read_pos = write_pos so firmware can write new data.
 * Only the calling session's kfifo is reset; other concurrent sessions
 * keep the data they just received from the drain.
 *
 * Return: 0 on success, negative error code on failure
 */
int axl_fwtrace_clear_buffer(struct file *filp, stream_source_t source);

/**
 * axl_fwtrace_debugfs_init - Initialize firmware trace debugfs entries
 * @dev: Device structure
 * @parent: Parent debugfs directory
 *
 * Called from main debugfs initialization to create trace debugfs files.
 */
void axl_fwtrace_debugfs_init(struct axl_pcie_aipu_dev *dev,
			      struct dentry *parent);

#endif /* _AXL_AIPU_FWTRACE_H_ */
