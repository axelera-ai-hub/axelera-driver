/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2025 Axelera AI. All rights reserved.  */
#ifndef __METIS_DMABUFH__
#define __METIS_DMABUF_H__

struct dmabuf_import {
	__u64 phy;
	size_t size;
	int nents;
};
struct dmabuf_sgl {
	__u64 phy;
	size_t size;
};
struct dmabuf_full_import {
	struct dmabuf_sgl *dsgl;
	int nvecs;
	int nbase;
	int nimp;
};

struct dmabuf_xfer {
	__u64 phy;
	size_t size;
	__u32 offset;
	int handle;
	int flags;
#define DMABUF_XFER_FLAG_READ  0x1
#define DMABUF_XFER_FLAG_WRITE 0x2
#define DMABUF_XFER_FLAG_SYNC  0x4
#define DMABUF_XFER_FLAG_ASYNC 0x8
	int channel;
};

struct dma_xfer {
	__u64 phy;
	__u64 virt;
	size_t size;
	__u32 offset;
	int flags;
};

struct dma_p2p_xfer {
	__u64 axi;
	__u64 p2pphy;
	size_t size;
	int channel;
	int flags;
};
#define DMA_XFER_FLAG_P2P 0x80000000

#define AXL_IOCTL_BASE	     'U'
#define AXL_IOCTL_MSG_LOCK   _IOWR(AXL_IOCTL_BASE, 0, int)
#define AXL_IOCTL_CTX_ALLOC  _IOWR(AXL_IOCTL_BASE, 1, int)
#define AXL_IOCTL_CTX_FREE   _IOWR(AXL_IOCTL_BASE, 2, int)
// #define AXL_IOCTL_EMPTY _IOWR(AXL_IOCTL_BASE, 3, int)
// #define AXL_IOCTL_EMPTY _IOWR(AXL_IOCTL_BASE, 4, int)
#define AXL_IOCTL_DMA_ATTACH _IOWR(AXL_IOCTL_BASE, 5, int)
#define AXL_IOCTL_DMA_DETACH _IOWR(AXL_IOCTL_BASE, 6, int)
#define AXL_IOCTL_DMA_XFER   _IOWR(AXL_IOCTL_BASE, 7, struct dmabuf_xfer *)
#define AXL_IOCTL_DMA_GET_XFER_ASYNC_STATUS \
	_IOWR(AXL_IOCTL_BASE, 8, struct dmabuf_xfer *)
#define AXL_IOCTL_DMA_GET_XFER_SYNC_STATUS \
	_IOWR(AXL_IOCTL_BASE, 9, struct dma_xfer *)
#define AXL_IOCTL_USR_DMA_XFER	      _IOWR(AXL_IOCTL_BASE, 10, struct dma_xfer *)
#define AXL_IOCTL_MSI		      _IOWR(AXL_IOCTL_BASE, 11, struct msi_info *)
#define AXL_IOCTL_CLEAN_MSI	      _IOWR(AXL_IOCTL_BASE, 12, struct msi_info *)
#define AXL_IOCTL_MSI_ATTACH	      _IOWR(AXL_IOCTL_BASE, 13, int)
#define AXL_IOCTL_GET_CTX_AICORE_MASK _IOWR(AXL_IOCTL_BASE, 14, uint64_t)
#define AXL_IOCTL_CLEAR_POLL_EVENT    _IOWR(AXL_IOCTL_BASE, 15, int)
#define AXL_IOCTL_GET_RESOURCE_INFO \
	_IOR(AXL_IOCTL_BASE, 16, struct dev_res_info)
#define AXL_IOCTL_GET_PCIE_WINDOWS \
	_IOR(AXL_IOCTL_BASE, 17, struct dev_mem_window)
#define AXL_IOCTL_DMA_P2P_XFER	_IOWR(AXL_IOCTL_BASE, 18, struct dma_p2p_xfer)
#define AXL_IOCTL_GET_DMA_STATS _IOR(AXL_IOCTL_BASE, 32, struct dma_stats)

enum {
	ASYNC_XFER_DONE = 1,
	ASYNC_XFER_FAIL,
	ASYNC_XFER_TIMEOUT,
	ASYNC_XFER_PENDING,
};
#ifdef __KERNEL__
struct dmabuf_imp {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	dma_addr_t phys;
	size_t size;
	void *virt; // for debug
};

#endif // __KERNEL__

#endif // __METIS_DMABUF_H__
