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

#ifndef __METIS_H__
#define __METIS_H__

#ifdef __KERNEL__
/* constants */
#define MAX_MSG		256
#define MAX_MEMORY_AREA 2
#define AICORE_COUNT	4
#define CONTEXT_COUNT	4
#define NAME_SIZE	32
#define MAX_DMA_CHANNEL 4
#define DMA_SIZE	(4 * 1024 * 1024)

#define EDMA_V0_MAX_NR_CH 4
#define HDMA_V0_MAX_NR_CH 4
/* ported from sysctl_mem.h */

enum msi_mapping {
	MSI_KRN_0 = 0,
	MSI_KRN_1 = 1,
	MSI_KRN_2 = 2,
	MSI_KRN_3 = 3,
	MSI_RD_CH0 = 4,
	MSI_RD_CH1 = 5,
	MSI_RD_CH2 = 6,
	MSI_RD_CH3 = 7,
	MSI_WR_CH0 = 8,
	MSI_WR_CH1 = 9,
	MSI_WR_CH2 = 10,
	MSI_WR_CH3 = 11,
	MSI_MSG = 12,
	MSI_DEV_AXE_MSG = 24,
	MAX_MSI = 32,
};

struct device_ctx_t {
	/* --- Context Resources --- */
	uint64_t aicore; // bitmask of aicores allocate to context
	uint64_t l2_base; // L2 context base address
	uint64_t l2_size; // L2 context size
	uint64_t ddr_base; // DDR context base address
	uint64_t ddr_size; // DDR context size
	uint64_t msi; // msi notify allocate to context
	/* --- Kernel Offload Resources --- */
	uint64_t cmd; // context command operation
	uint64_t sts; // context return operation state
	uint64_t arg; // context command argument
	uint64_t elf_base; // elf base address
};

struct device_host_drv_t {
	uint32_t ctrl;
	uint32_t base;
	uint64_t target;
	uint32_t size;
};
#define SYSCTL_HOST_DRV_AREA_MAGIC (0xBAC1)
#define MAX_VIRT_MSI		   (1024)
#define VMSI_IRQ_EN_BIT		   (0)
#define VMSI_IRQ_EN		   (1 << VMSI_IRQ_EN_BIT)
struct device_virt_msi_t {
	volatile u32 msi[MAX_VIRT_MSI];
};

struct cmd_t {
	uint32_t opcode;
	uint32_t size;
	char msg[MAX_MSG];
};

struct version_t {
	uint8_t major;
	uint8_t minor;
};

struct memory_reference_t {
	uint16_t magic; // magic number for sanity check
	struct version_t version; // version of the area
	uint32_t size; // size of the area
	uint64_t offset; // offset of the area (w.r.t. BAR)
	uint16_t memory_area; // memory where the area is located
	uint16_t reserved[3];
};

struct device_sys_ctl_t {
	/* -------- Static Runtime Area (1KB) --------- */
	uint8_t pad0[176]; // 0x00
	struct cmd_t cmd; // 0xb0
	uint8_t pad1[72];
	struct version_t master_version; // 0x200
	uint8_t pad2[6];
	uint64_t memory_map[MAX_MEMORY_AREA]; // memory types to device phys. addr.
	uint8_t pad3[32];
	uint64_t fw_load_addr;
	struct memory_reference_t logtrace_mem_ref;
	struct memory_reference_t boardinfo_mem_ref;
	struct memory_reference_t axemsg_mem_ref;
	struct memory_reference_t ctx_mem_ref;
	struct memory_reference_t virt_mem_ref;
	struct memory_reference_t hdrv_mem_ref;
	uint8_t pad4[304];
	/* ---- Static MCUBoot Reserved Area (1KB) ---- */
	uint8_t mcuboot_shared[1024]; // 0x400
};

/* end of ported from sysctl_mem.h */

enum dma_type {
	EDMA_DMA = 0,
	HYPER_DMA = 1,
};

enum {
	AXL_QEMU_MODE = 2,
	AXL_SILICON_MODE = 3,
};

struct axe_device_info {
	const char *name;
	const char *devname;
	int mode;
	int dma_rd_ch;
	int dma_wr_ch;
	int dma_type;
	int dma_size;
	int aicore_count;
	int pve_count;
};

struct msi_info {
	int num;
	int timeout;
};
struct irq_wrk {
	struct axl_pcie_aipu_dev *axldev;
	int id;
	int timeout;
	spinlock_t irq_lock;
	struct completion irq_done;
	int (*check)(struct axl_pcie_aipu_dev *axldev, int id);
	ktime_t stime;
	struct list_head sctx_list;
};
struct dma_wrk {
	struct work_struct work;
	struct axl_pcie_aipu_dev *axldev;
	struct sysctrl_ctx *sctx;
	int id;
	int timeout;
	int status;
	int channel;
	struct completion done;
	size_t size;
	u32 offset;
	__u64 axi;
	__u64 p2pphy;
	int num_sgt;
	int flags;
	struct sg_table *table;
	struct dma_queue_ctrl *qctrl;
};
struct dma_queue_ctrl {
	struct axl_pcie_aipu_dev *axldev;
	int id;
	int timeout;
	struct workqueue_struct *wq;
	atomic_t count;
	ktime_t ktime;
	// statistics
	int num_xfer;
	int num_err;
	__u64 bytes_xfer;
	int max_sgt;
	size_t size;
	ktime_t duration;
	ktime_t max_duration;
};
struct axl_dev_fops {
	int (*dma_irq_ck)(struct axl_pcie_aipu_dev *axldev, int id);
	void (*dma_enable_ctrl)(struct axl_pcie_aipu_dev *axldev);
	void (*dma_job_submit)(struct dma_wrk *dma_wrk);
	void (*dma_init_imwr)(struct axl_pcie_aipu_dev *axldev);
	void (*dma_p2p_job_submit)(struct dma_wrk *dma_wrk);

	void (*dev_debugfs_init)(struct axl_pcie_aipu_dev *axldev);
	void (*dev_debugfs_exit)(struct axl_pcie_aipu_dev *axldev);
};
#define MAX_MEMORY_AREA 2
struct dev_res_info {
	__u64 sysmem_base;
	__u64 sysmem_size;
	__u64 l2_base;
	__u64 l2_size;
	__u64 ddr_base[MAX_MEMORY_AREA];
	__u64 ddr_size[MAX_MEMORY_AREA];
};

struct dev_mem_window {
	__u64 np_base;
	__u64 np_size;
	__u64 p_base;
	__u64 p_size;
	__u64 base_res[6];
	__u64 size_res[6];
};

struct axl_pcie_aipu_dev {
	char name[NAME_SIZE];
	struct pci_dev *pdev;
	int dma_enabled : 1;
	struct device_host_drv_t *hdrv_base;
	dma_addr_t dma_addr;
	unsigned long *dma_va;
	int dma_size;
	int nmsi;
	int irq_vec;
	struct msi_msg irq_msi;
	struct axl_dev_fops *fops;
	const struct axe_device_info *dev_info;
	struct dev_res_info *res_info;
	struct dev_mem_window *mem_win;
	// context
	struct mutex mutex;
	struct mutex msg_mutex;
	uint64_t glob_ctx_mask; // global context mask
	uint64_t ctx_mask[CONTEXT_COUNT]; // per device context mask
	// char
	struct cdev cdev;
	struct dentry *dentry;
	int minor;
	// recovery thread
	int dlllarc; // Data Link Layer Link Active Reporting Capable
	struct task_struct *recovery;
	struct pci_saved_state *pcie_state;
	int dev_state;
	// pcie dma
	void *dma;
	phys_addr_t pdma;
	void __iomem *vl2base;
	phys_addr_t pl2base;
	struct irq_wrk *irq_wrk;
	struct dma_queue_ctrl *dma_wrqc;
	struct dma_queue_ctrl *dma_rdqc;
	spinlock_t msi_lock;
};
struct sysctrl_ctx {
	struct axl_pcie_aipu_dev *axldev;
	uint64_t ctx_mask; // context mask
	int msg_flag;
	struct dmabuf_imp di;
	int async_dma_xfer;
	struct dma_wrk *dma_wrk;
	wait_queue_head_t poll_wait_queue;
	atomic_t poll_event_cnt;
	struct list_head node;
};

struct dma_channel_stats {
	int count; /* Current active transfers */
	int num_xfer; /* Total number of transfers */
	int num_err; /* Number of errors */
	__u64 bytes_xfer; /* Total bytes transferred */
	int max_sgt; /* Maximum scatter-gather table entries */
	__u64 max_duration_us; /* Maximum duration in microseconds */
	__u64 duration_us; /* Last transfer duration in microseconds */
	size_t size; /* Last transfer size in bytes */
	__u32 speed_mbps; /* Last transfer speed in MB/s */
};

struct dma_stats {
	struct dma_channel_stats rd_channels[MAX_DMA_CHANNEL];
	struct dma_channel_stats wr_channels[MAX_DMA_CHANNEL];
};

static inline void axlaipu_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->fops->dev_debugfs_init)
		axldev->fops->dev_debugfs_init(axldev);
}

static inline void axlaipu_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->fops->dev_debugfs_exit)
		axldev->fops->dev_debugfs_exit(axldev);
}

static inline void axlaipu_dma_enable_ctrl(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->fops->dma_enable_ctrl)
		axldev->fops->dma_enable_ctrl(axldev);
}

static inline void axlaipu_dma_init_imwr(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->fops->dma_init_imwr)
		axldev->fops->dma_init_imwr(axldev);
}

static inline void axlaipu_dma_job_sumbit(struct axl_pcie_aipu_dev *axldev,
					  struct dma_wrk *dma_wrk)
{
	if (axldev->fops->dma_job_submit)
		axldev->fops->dma_job_submit(dma_wrk);
}

static inline void axlaipu_dma_p2p_job_sumbit(struct axl_pcie_aipu_dev *axldev,
					      struct dma_wrk *dma_wrk)
{
	if (axldev->fops->dma_p2p_job_submit)
		axldev->fops->dma_p2p_job_submit(dma_wrk);
}
static inline int axlaipu_dma_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	if (!axldev->fops->dma_irq_ck)
		return -EINVAL;

	return axldev->fops->dma_irq_ck(axldev, id);
}

static inline unsigned int first_set_bit(unsigned int n)
{
	unsigned int pos = 0;
	while (!(n & (1 << pos))) {
		pos++;
	}
	return pos;
}

static inline void get_max_duration(struct dma_queue_ctrl *dma_ctrl)
{
	dma_ctrl->duration = ktime_sub(ktime_get(), dma_ctrl->ktime);
	dma_ctrl->max_duration =
		max_t(ktime_t, dma_ctrl->duration, dma_ctrl->max_duration);
}
static inline int get_timeout_ms(int timeout)
{
	return timeout == 0 ? msecs_to_jiffies(1000) :
			      msecs_to_jiffies(timeout * 1000);
}

static inline int validate_dma_xfer(struct dmabuf_xfer *dxfer,
				    struct pci_dev *pdev)
{
	int read_write = DMABUF_XFER_FLAG_READ | DMABUF_XFER_FLAG_WRITE;
	int async_sync = DMABUF_XFER_FLAG_ASYNC | DMABUF_XFER_FLAG_SYNC;

	dev_dbg(&pdev->dev,
		"dma xfer channel:%s%d | phy 0x%llx | off 0x%x | size 0x%x | flags 0x%x | ch %d\n",
		dxfer->flags & DMABUF_XFER_FLAG_READ ? "RD" : "WR",
		dxfer->channel, dxfer->phy, dxfer->offset, (int)dxfer->size,
		dxfer->flags, dxfer->channel);

	if (((dxfer->flags & read_write) == 0) ||
	    ((dxfer->flags & read_write) == read_write))
		return -EINVAL;
	if (((dxfer->flags & async_sync) == 0) ||
	    ((dxfer->flags & async_sync) == async_sync))
		return -EINVAL;
	if (dxfer->size == 0)
		return -EINVAL;
	return 0;
}

long sysctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
#endif // __KERNEL__

#endif // __METIS_H__
