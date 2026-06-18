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

/*
 * This driver is a PCI Express driver for the Metis PCIe chip.
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
#include <linux/idr.h>
#include <linux/kref.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-edma-core.h"
#include "axl-aipu-hdma-core.h"
#include "axl-aipu-fwtrace.h"
#include "axl-aipu-version.h"
#include "axl-aipu-msi.h"

#define DRIVER_AUTHOR "Axelera AI"
#define DRIVER_DESC   "Axelera AI AIPU driver"

#define AXL_VENDOR_TEST	    0x1234
#define AXL_DEV_TRITON	    0x1972
#define AXL_DEV_SYNOPSYS    0x2023
#define AXL_DEV_QEMU_OMEGA  0x2024
#define AXL_DEV_QEMU_EUROPA 0x2025

#define DEVICE_CLASS_NAME \
	"metis" // keep metis just to be backward compatible with old SDK

#define AXELERA_VENDOR_ID	  0x1F9D
#define AXL_AIPU_ALPHA_DEVICE_ID  0x11AA
#define AXL_AIPU_OMEGA_DEVICE_ID  0x1100
#define AXL_AIPU_EUROPA_DEVICE_ID 0x0001

#define METIS_CLASS_CODE  0x1200
#define METIS_REVISION_ID 0x0   

#define AXL_AIPU_MAX_MINORS 256

static DEFINE_IDA(axl_aipu_minor_ida);
struct dentry *axl_aipu_debugfs_root;

static struct class *axl_aipu_class;
static int axl_aipu_major;

unsigned int dma_timeout = 2;
module_param(dma_timeout, uint, 0644);
MODULE_PARM_DESC(dma_timeout, "DMA timeout in seconds (default 2 sec)");
unsigned int irq_timeout = 1;
module_param(irq_timeout, uint, 0644);
MODULE_PARM_DESC(irq_timeout, "IRQ timeout in seconds (default 1 sec)");
MODULE_PARM_DESC(enable_dmabuf_sync,
		 "Enable dmabuf sync : 1 enable, 0 disable");

static unsigned int single_msi = 0;
module_param(single_msi, uint, 0644);

unsigned int dma_poll = 0;
module_param(dma_poll, uint, 0644);
MODULE_PARM_DESC(dma_poll, "DMA polling mode (default 0 disabled, 1 enabled)");

unsigned int europa_veloce = 0;
module_param(europa_veloce, uint, 0644);
MODULE_PARM_DESC(europa_veloce,
		 "Enable europa veloce mode default 0 disabled, 1 enabled)");

unsigned int enable_sg_host_dma = 0;
module_param(enable_sg_host_dma, uint, 0644);
MODULE_PARM_DESC(
	enable_sg_host_dma,
	"Enable host sglist and xfer via dma engine (default 0 disabled)");

static unsigned int dma_trace_entries = 4000;
module_param(dma_trace_entries, uint, 0644);
MODULE_PARM_DESC(dma_trace_entries,
		 "DMA trace buffer entries (64 - 65536, default 4000)");

static void axl_aipu_dma_imwr_restore(struct axl_pcie_aipu_dev *axldev);
static void axl_aipu_disable_dev_dma(struct pci_dev *pdev);
static void axl_aipu_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev);

static const struct vm_operations_struct axl_physical_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

static int sysctrl_open(struct inode *inode, struct file *file)
{
	struct axl_pcie_aipu_dev *axldev =
		container_of(inode->i_cdev, struct axl_pcie_aipu_dev, cdev);
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sys_ctx;

	if (axldev->dev_state)
		return -ENODEV;

	sys_ctx = kzalloc(sizeof(*sys_ctx), GFP_KERNEL);
	if (!sys_ctx)
		return -ENOMEM;

	kref_init(&sys_ctx->refcount);
	INIT_LIST_HEAD(&sys_ctx->node);
	init_waitqueue_head(&sys_ctx->poll_wait_queue);
	atomic_set(&sys_ctx->poll_event_cnt, 0);
	sys_ctx->axldev = axldev;
	file->private_data = sys_ctx;

	/* Initialize firmware trace session fields */
	sys_ctx->fwtrace_mode = false;
	INIT_LIST_HEAD(&sys_ctx->fwtrace_session.list);
	init_waitqueue_head(&sys_ctx->fwtrace_session.wait_queue);
	spin_lock_init(&sys_ctx->fwtrace_session.lock);
	atomic64_set(&sys_ctx->fwtrace_session.overruns, 0);

	dev_dbg(&pdev->dev, "open ctx by %d\n", current->pid);

	return 0;
}

/* Deferred cleanup for sys_ctx when DMA work is pending */
struct sys_ctx_cleanup {
	struct work_struct work;
	struct sysctrl_ctx *sys_ctx;
	struct dma_wrk *dma_wrk;
};

static void axl_aipu_sys_ctx_cleanup_dmabuf(struct sysctrl_ctx *sys_ctx)
{
	struct dmabuf_imp *di = &sys_ctx->di;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	if (di->dmabuf) {
		dev_dbg(&pdev->dev, "Release dmabuf\n");
		dma_buf_unmap_attachment(di->attachment, di->table,
					 DMA_BIDIRECTIONAL);
		dma_buf_detach(di->dmabuf, di->attachment);
		dma_buf_put(di->dmabuf);
		di->dmabuf = NULL;
	}
}

static void axl_aipu_sys_ctx_cleanup_final(struct sysctrl_ctx *sys_ctx)
{
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	wake_up_interruptible_poll(&sys_ctx->poll_wait_queue, EPOLLIN);
	dev_dbg(&pdev->dev, "sys_ctx freed %p\n", sys_ctx);
	kfree(sys_ctx);
}

static void axl_aipu_sys_ctx_cleanup_func(struct work_struct *work)
{
	struct sys_ctx_cleanup *cleanup =
		container_of(work, struct sys_ctx_cleanup, work);
	struct sysctrl_ctx *sys_ctx = cleanup->sys_ctx;
	struct dma_wrk *dma_wrk = cleanup->dma_wrk;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	dev_dbg(&pdev->dev, "Async cleanup: waiting for dma_wrk %p\n", dma_wrk);

	/*
	 * Use cancel_work_sync() rather than flush_work() to avoid the
	 * kernel 5.15+ lockdep warning triggered by calling flush_work()
	 * from within a workqueue worker context. cancel_work_sync() uses
	 * from_cancel=true internally, bypassing the check, and is safe
	 * here: on fd-close we either let the DMA work finish or cancel
	 * it before freeing resources — both outcomes are correct.
	 */
	cancel_work_sync(&dma_wrk->work);

	/* Now safe to release dmabuf and free sys_ctx */
	kref_put(&dma_wrk->refcount, axl_aipu_dma_wrk_release);
	axl_aipu_sys_ctx_cleanup_dmabuf(sys_ctx);

	dev_dbg(&pdev->dev, "Async cleanup: sys_ctx freed %p\n", sys_ctx);
	kfree(sys_ctx);
	kfree(cleanup);
}

static void
axl_aipu_sys_ctx_release_from_work_context(struct sysctrl_ctx *sys_ctx,
					   struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	dev_dbg(&pdev->dev, "Release from work context, skipping flush\n");

	/* Release sys_ctx's reference to dma_wrk */
	kref_put(&dma_wrk->refcount, axl_aipu_dma_wrk_release);

	/* Normal cleanup */
	axl_aipu_sys_ctx_cleanup_dmabuf(sys_ctx);
	axl_aipu_sys_ctx_cleanup_final(sys_ctx);
}

static bool axl_aipu_sys_ctx_schedule_async_cleanup(struct sysctrl_ctx *sys_ctx,
						    struct dma_wrk *dma_wrk)
{
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct sys_ctx_cleanup *cleanup;

	dev_dbg(&pdev->dev, "Scheduling async cleanup for dma_wrk %p\n",
		dma_wrk);

	cleanup = kmalloc(sizeof(*cleanup), GFP_ATOMIC);
	if (!cleanup) {
		dev_warn(&pdev->dev,
			 "Failed to allocate cleanup, falling back to sync\n");
		return false;
	}

	INIT_WORK(&cleanup->work, axl_aipu_sys_ctx_cleanup_func);
	cleanup->sys_ctx = sys_ctx;
	cleanup->dma_wrk = dma_wrk;
	schedule_work(&cleanup->work);

	return true; /* sys_ctx will be freed by cleanup worker */
}

static void axl_aipu_sys_ctx_release_sync_fallback(struct sysctrl_ctx *sys_ctx,
						   struct dma_wrk *dma_wrk)
{
	dev_dbg(&sys_ctx->axldev->pdev->dev, "Sync fallback cleanup\n");

	/* Fallback to blocking cleanup */
	flush_work(&dma_wrk->work);
	kref_put(&dma_wrk->refcount, axl_aipu_dma_wrk_release);
	axl_aipu_sys_ctx_cleanup_dmabuf(sys_ctx);
	axl_aipu_sys_ctx_cleanup_final(sys_ctx);
}

void axl_aipu_sys_ctx_release(struct kref *kref)
{
	struct sysctrl_ctx *sys_ctx =
		container_of(kref, struct sysctrl_ctx, refcount);
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dma_wrk *dma_wrk;

	dma_wrk = xchg(&sys_ctx->dma_wrk, NULL);
	if (!dma_wrk) {
		/* No pending DMA work - normal cleanup */
		axl_aipu_sys_ctx_cleanup_dmabuf(sys_ctx);
		axl_aipu_sys_ctx_cleanup_final(sys_ctx);
		return;
	}

	/* DMA work is pending */
	dev_dbg(&pdev->dev, "sys_ctx releasing dma_wrk %p\n", dma_wrk);

	if (current_work() == &dma_wrk->work) {
		/* Called from work context - cannot flush */
		axl_aipu_sys_ctx_release_from_work_context(sys_ctx, dma_wrk);
		return;
	}

	/* Schedule async cleanup to avoid blocking close() */
	if (axl_aipu_sys_ctx_schedule_async_cleanup(sys_ctx, dma_wrk))
		return; /* sys_ctx will be freed by cleanup worker */

	/* Async cleanup failed - fallback to sync */
	axl_aipu_sys_ctx_release_sync_fallback(sys_ctx, dma_wrk);
}

static inline void axl_aipu_sysctrl_poll_unregister(struct sysctrl_ctx *sys_ctx)
{
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct irq_wrk *irq_wrk = axldev->irq_wrk;
	struct pci_dev *pdev = axldev->pdev;
	struct sysctrl_ctx *sctx_msi;
	int i;

	spin_lock_irq(&axldev->msi_lock);
	for (i = 0; i < axldev->max_msi; i++) {
		if (list_empty(&irq_wrk[i].sctx_list))
			continue;
		list_for_each_entry(sctx_msi, &irq_wrk[i].sctx_list, node)
		{
			if (sys_ctx == sctx_msi) {
				list_del(&sctx_msi->node);
				dev_dbg(&pdev->dev,
					"Force remove ctx %p from msi %d\n",
					sctx_msi, i);
				break;
			}
		}
	}
	spin_unlock_irq(&axldev->msi_lock);
}

static inline void axl_aipu_sysctrl_ctx_release(struct sysctrl_ctx *sys_ctx)
{
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	mutex_lock(&axldev->mutex);
	if (sys_ctx->ctx_mask) {
		int pos = first_set_bit(sys_ctx->ctx_mask);
		dev_dbg(&pdev->dev, "Release (%d) 0x%llx by %d\n", pos,
			axldev->glob_ctx_mask, current->pid);
		axldev->glob_ctx_mask &= ~(sys_ctx->ctx_mask);
		axldev->ctx_mask[pos] = 0;
	} else
		dev_dbg(&pdev->dev, "Release no ctx allocated by %d\n",
			current->pid);
	mutex_unlock(&axldev->mutex);
}

static ssize_t sysctrl_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;

	/* If in firmware trace mode, delegate to trace read handler */
	if (sys_ctx->fwtrace_mode)
		return axl_fwtrace_read(file, buf, count, ppos);

	/* Normal sysctl file doesn't support read */
	return -EINVAL;
}

static int sysctrl_release(struct inode *inode, struct file *file)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;

	if (sys_ctx->msg_flag)
		mutex_unlock(&axldev->msg_mutex);

	/* Cleanup firmware trace session if active */
	if (sys_ctx->fwtrace_mode)
		axl_fwtrace_close_session(file);

	axl_aipu_sysctrl_poll_unregister(sys_ctx);
	axl_aipu_sysctrl_ctx_release(sys_ctx);

	kref_put(&sys_ctx->refcount, axl_aipu_sys_ctx_release);
	return 0;
}

#define MAX_METIS_MAPS 2
static int sysctl_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	resource_size_t paddr;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_ops = &axl_physical_vm_ops;
	if (vma->vm_pgoff >= MAX_METIS_MAPS)
		return -EINVAL;

	paddr = vma->vm_pgoff ? axldev->pdma : axldev->pl2base;
	if (remap_pfn_range(vma, vma->vm_start, paddr >> PAGE_SHIFT,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		return -ENOMEM;
	}

	return 0;
}

static __poll_t sysctrl_poll(struct file *file, poll_table *wait)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;

	/* If in firmware trace mode, delegate to trace poll handler */
	if (sys_ctx->fwtrace_mode)
		return axl_fwtrace_poll(file, wait);

	dev_dbg(&pdev->dev, "sysctrl_poll wait %p\n", sys_ctx);

	poll_wait(file, &sys_ctx->poll_wait_queue, wait);

	if (atomic_read(&sys_ctx->poll_event_cnt) > 0) {
		return EPOLLIN;
	}

	return 0;
}

static struct file_operations axl_aipu_file_fops = {
	.owner = THIS_MODULE,
	.open = sysctrl_open,
	.release = sysctrl_release,
	.read = sysctrl_read,
	.unlocked_ioctl = sysctl_ioctl,
	.compat_ioctl = sysctl_ioctl,
	.mmap = sysctl_mmap,
	.poll = sysctrl_poll,
};

static ssize_t axl_aipu_restore_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct axl_pcie_aipu_dev *axldev = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", axldev->dlllarc);
}
static ssize_t axl_aipu_restore_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct axl_pcie_aipu_dev *axldev = dev_get_drvdata(dev);
	struct pci_dev *pdev = axldev->pdev;
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val) != 0)
		return -EINVAL;

	dev_info(&pdev->dev, "Recovery device state\n");
	pci_load_saved_state(pdev, axldev->pcie_state);
	pci_restore_state(pdev);
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable device\n");
		return ret;
	}
	pci_set_master(pdev);
	return count;
}
static DEVICE_ATTR(axl_aipu_restore, 0664, axl_aipu_restore_show,
		   axl_aipu_restore_store);

static struct attribute *axl_aipu_pcie_axl_attrs[] = {
	&dev_attr_axl_aipu_restore.attr,
	NULL,
};
ATTRIBUTE_GROUPS(axl_aipu_pcie_axl);

static int axl_aipu_recovery(void *data)
{
	struct axl_pcie_aipu_dev *axldev = (struct axl_pcie_aipu_dev *)data;
	struct pci_dev *pdev = axldev->pdev;
	struct pci_dev *port = pdev->bus->self;
	u16 lnksta = 0;
	int link = 1;
	int ret;
	while (!kthread_should_stop()) {
		pcie_capability_read_word(port, PCI_EXP_LNKSTA, &lnksta);
		dev_dbg(&axldev->pdev->dev, "link %x %x %x\n", lnksta,
			PCI_EXP_LNKSTA_DLLLA, PCI_EXP_LNKSTA_LBMS);
		if (link && !(lnksta & PCI_EXP_LNKSTA_DLLLA)) {
			if (!axldev->dev_state) {
				dev_info(&pdev->dev, "Link down\n");
				axl_aipu_fwtrace_kill_sessions(axldev);
			}
			axldev->dev_state = 1;
		}

		if (axldev->dev_state && (lnksta & PCI_EXP_LNKSTA_DLLLA)) {
			dev_info(&pdev->dev, "Link up\n");
			link = 1;
			msleep(5000);
			dev_dbg(&pdev->dev, "Wait FW init dynamic area\n");
			pci_load_saved_state(pdev, axldev->pcie_state);
			pci_restore_state(pdev);
			ret = pci_enable_device(pdev);
			if (ret)
				dev_err(&pdev->dev,
					"pci_enable_device failed (%d) ", ret);
			else
				axldev->dev_state = 0;
			pci_set_master(pdev);
			axl_aipu_dma_imwr_restore(axldev);
			axl_aipu_config_dev_dma(axldev);
			axl_aipu_config_dev_msi(axldev);
			axl_aipu_dev_dynmem_init(axldev);
			axl_aipu_fwtrace_refresh(axldev);
		}

		link = (lnksta & PCI_EXP_LNKSTA_DLLLA) ? 1 : 0;
		/*The teoretical Gen1 to Gen3  switch according to spec is 100 ms .. 800 ms.
		For safety: we set this to 80 ms and this should work every time now even with our
		Omega boot code now in ROM*/
		msleep(80);
	}

	return 0;
}

static int axl_pci_msi_init(struct pci_dev *pdev,
			    struct axl_pcie_aipu_dev *axldev)
{
	int err = 0, nmsi;

	nmsi = pci_msi_vec_count(pdev);
	dev_dbg(&pdev->dev, "MSI available %d\n", nmsi);

	if (nmsi != 32) {
		dev_warn(&pdev->dev, "Wrong msi number %d\n", nmsi);
		nmsi = 1;
	}
	if (single_msi)
		nmsi = 1;

	err = pci_alloc_irq_vectors(pdev, 1, nmsi, PCI_IRQ_MSI);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to enable MSI (%x)\n", err);
		return -ENODEV;
	} else
		dev_info(&pdev->dev, "MSI registered %d (%d)\n", nmsi, err);
	axldev->nmsi = err;

	axldev->irq_wrk = devm_kcalloc(&pdev->dev, MAX_VIRT_MSI,
				       sizeof(*axldev->irq_wrk), GFP_KERNEL);
	if (!axldev->irq_wrk)
		return -ENOMEM;

	axldev->vmsi_count = devm_kcalloc(&pdev->dev, MAX_VIRT_MSI,
					  sizeof(*axldev->vmsi_count),
					  GFP_KERNEL);
	if (!axldev->vmsi_count)
		return -ENOMEM;

	axldev->irq_vec = pci_irq_vector(pdev, 0);
	dev_info(&pdev->dev, "irq vec number %d\n", axldev->irq_vec);

	/* Initialize and register IRQ handlers via MSI-specific init function */
	err = axldev->msi_fops->init(axldev);
	if (err)
		return err;

	dev_dbg(&pdev->dev, "msi_info 0x%x 0x%x : 0x%x\n",
		axldev->irq_msi.address_hi, axldev->irq_msi.address_lo,
		axldev->irq_msi.data);

	axl_aipu_dma_enable_ctrl(axldev);

	return 0;
}
static void axl_aipu_dma_imwr_restore(struct axl_pcie_aipu_dev *axldev)
{
	get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
	axl_aipu_dma_init_imwr(axldev);
}

static int axl_set_dma_mask(struct pci_dev *pdev)
{
	int ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_coerce_mask_and_coherent(&pdev->dev,
						   DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "Failed to set DMA bit mask\n");
			return ret;
		}
		dev_warn(&pdev->dev, "Cannot set DMA highmem bit mask\n");
	}
	return ret;
}

static inline int axl_aipu_alloc_minor(void)
{
	return ida_alloc_max(&axl_aipu_minor_ida, AXL_AIPU_MAX_MINORS - 1,
			     GFP_KERNEL);
}
static inline void axl_aipu_free_minor(struct axl_pcie_aipu_dev *axldev)
{
	ida_free(&axl_aipu_minor_ida, axldev->minor);
}

static void axl_aipu_pci_deinit(struct pci_dev *pdev)
{
	axl_aipu_disable_dev_dma(pdev);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static void disable_serr_bit(struct pci_dev *pdev)
{
	u16 cmd = 0;

	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	cmd &= ~PCI_COMMAND_SERR; // Clear SERR# Enable (bit 8)
	pci_write_config_word(pdev, PCI_COMMAND, cmd);
}

static void mask_all_aer_errors(struct pci_dev *pdev)
{
	int pos;

	// Find AER extended capability
	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ERR);
	if (!pos) {
		dev_warn(&pdev->dev, "AER capability not found\n");
		return;
	}

	// Mask all uncorrectable errors
	pci_write_config_dword(pdev, pos + PCI_ERR_UNCOR_MASK, 0xFFFFFFFF);

	// Mask all correctable errors
	pci_write_config_dword(pdev, pos + PCI_ERR_COR_MASK, 0xFFFFFFFF);

	dev_info(&pdev->dev, "All AER errors masked\n");
}
static int axl_aipu_check_pes_group(struct pci_dev *pdev)
{
	struct pci_dev *parent_port;
	struct pci_dev *upstream_port;
	u16 parent_vendor, parent_device;

	if (!pdev->bus || !pdev->bus->self)
		return -1;

	parent_port = pdev->bus->self;

	/* Check if parent port is a Microsemi device with ID 0x8562 */
	pci_read_config_word(parent_port, PCI_VENDOR_ID, &parent_vendor);
	pci_read_config_word(parent_port, PCI_DEVICE_ID, &parent_device);
	if (parent_vendor != PCI_VENDOR_ID_MICROSEMI || parent_device != 0x8562)
		return -1;

	if (!parent_port->bus || !parent_port->bus->self)
		return -1;

	upstream_port = parent_port->bus->self;

	return upstream_port->bus->number;
}

static void axl_aipu_get_memwindow_info(struct axl_pcie_aipu_dev *axldev,
					struct pci_dev *pdev)
{
	int flags, bar;
	resource_size_t start, size, np_max_size = 0, p_max_size = 0;
	resource_size_t np_min = 0, np_max = 0, p_min = 0, p_max = 0;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		flags = pci_resource_flags(pdev, bar);
		if (!(flags & IORESOURCE_MEM))
			continue;
		size = pci_resource_len(pdev, bar);
		start = pci_resource_start(pdev, bar);
		axldev->mem_win->base_res[bar] = (__u64)start;
		axldev->mem_win->size_res[bar] = (__u64)size;

		dev_dbg(&pdev->dev, "Memory (%d) %s 0x%016llx 0x%016llx\n", bar,
			flags & IORESOURCE_PREFETCH ? "prefetch" :
						      "no-prefetch",
			start, size);

		if (flags & IORESOURCE_PREFETCH) {
			if (p_min == 0) {
				p_min = p_max = start;
			}
			p_max = max_t(resource_size_t, start, p_max);
			p_min = max_t(resource_size_t, start, p_min);
			p_max_size = max_t(resource_size_t, size, p_max_size);
			continue;
		}

		if (np_min == 0) {
			np_min = np_max = start;
		}
		np_max = max_t(resource_size_t, start, np_max);
		np_min = min_t(resource_size_t, start, np_min);
		np_max_size = max(size, np_max_size);
	}
	axldev->mem_win->np_base = (__u64)np_min;
	axldev->mem_win->np_size = (__u64)(np_max - np_min + np_max_size);
	axldev->mem_win->p_base = (__u64)p_min;
	axldev->mem_win->p_size = (__u64)(p_max - p_min + p_max_size);
	dev_info(&pdev->dev, "Memory windows prefetch 0x%016llx 0x%016llx\n",
		 axldev->mem_win->p_base, axldev->mem_win->p_size);
	dev_info(&pdev->dev, "Memory windows no-prefetch 0x%016llx 0x%016llx\n",
		 axldev->mem_win->np_base, axldev->mem_win->np_size);
}

#define BAR_0 0
#define BAR_2 2
#define BAR_5 5
static int axl_aipu_pci_resources_init(struct axl_pcie_aipu_dev *axldev)
{
	int err, mask, dma_bar;
	struct pci_dev *pdev = axldev->pdev;

	if (axldev->dev_info->hw_gen >= AXL_HW_GEN_EUROPA) {
		mask = BIT(BAR_0) | BIT(BAR_2) | BIT(BAR_5);
		dma_bar = BAR_5;
	} else if (axldev->dev_info->hw_gen == AXL_HW_GEN_METIS) {
		mask = BIT(BAR_0) | BIT(BAR_2);
		dma_bar = BAR_0;
	} else {
		dev_err(&pdev->dev, "Unknown device generation %d\n",
			axldev->dev_info->hw_gen);
		return -ENODEV;
	}
	if (europa_veloce) {
		irq_timeout = 5 * irq_timeout;
		dma_timeout = 5 * dma_timeout;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	err = pcim_iomap_regions(pdev, mask, axldev->name);
#else
	err = pcim_iomap_regions_request_all(pdev, mask, axldev->name);
#endif
	if (err) {
		dev_err(&pdev->dev, "Failed to request resources\n");
		return -ENOMEM;
	}
	axldev->dma = pcim_iomap_table(pdev)[dma_bar];
	axldev->pdma = pci_resource_start(pdev, dma_bar);
	axldev->vl2base = pcim_iomap_table(pdev)[BAR_2];
	axldev->pl2base = pci_resource_start(pdev, BAR_2);

	if (!axldev->dma || !axldev->vl2base) {
		dev_err(&pdev->dev, "Failed to map resources %p %p\n",
			axldev->dma, axldev->vl2base);
		return -EIO;
	}

	axldev->res_info->l2_base = axldev->pl2base;
	axldev->res_info->l2_size = pci_resource_len(pdev, BAR_2);
	return 0;
}

static int axl_aipu_pci_init(struct pci_dev *pdev,
			     struct axl_pcie_aipu_dev *axldev)
{
	int err;
	u16 reg16, lnkctl2, lnksta;
	u32 reg32, lnkcap;

	pci_aer_clear_nonfatal_status(pdev);
	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", err);
		axl_aipu_free_minor(axldev);
		return err;
	}
	pci_set_master(pdev);

	err = axl_set_dma_mask(pdev);
	if (err)
		goto pci_err_out;

	pci_set_drvdata(pdev, axldev);
	axldev->pdev = pdev;

	err = axl_aipu_pci_resources_init(axldev);
	if (err)
		goto pci_err_out;

	disable_serr_bit(pdev);
	mask_all_aer_errors(pdev);

	axl_aipu_get_memwindow_info(axldev, pdev);

	if (pdev->bus->self) {
		axldev->pes_group = axl_aipu_check_pes_group(pdev);
		if (axldev->pes_group != -1) {
			dev_info(
				&pdev->dev,
				"Microsemi switch upstream port bus ID: %02x\n",
				axldev->pes_group);
		}

		pcie_capability_read_dword(pdev->bus->self, PCI_EXP_LNKCAP,
					   &reg32);
		if ((reg32 & PCI_EXP_LNKCAP_DLLLARC))
			axldev->dlllarc = 1;
		if (europa_veloce) {
			dev_info(&pdev->dev,
				 " Disable link recovery in Veloce\n");
			axldev->dlllarc = 0;
		}
		lnkcap = (reg32 & PCI_EXP_LNKCAP_SLS);
		pcie_capability_read_word(pdev->bus->self, PCI_EXP_LNKSTA,
					  &lnksta);
		lnksta = (lnksta & PCI_EXP_LNKSTA_CLS);
		if ((lnkcap >= PCI_EXP_LNKCAP_SLS_8_0GB) &&
		    (lnksta < PCI_EXP_LNKSTA_CLS_8_0GB)) {
			dev_warn(&pdev->dev, "Host cap Gen%d - current Gen%d\n",
				 lnkcap, lnksta);
			pcie_capability_read_word(pdev->bus->self,
						  PCI_EXP_LNKCTL2, &lnkctl2);
			dev_dbg(&pdev->dev, "linkctl2 %x %x\n", PCI_EXP_LNKCTL2,
				lnkctl2);
			lnkctl2 &= ~(PCI_EXP_LNKCTL2_TLS);
			lnkctl2 |= PCI_EXP_LNKCTL2_TLS_8_0GT;
			dev_dbg(&pdev->dev, "new linkctl2 %x %x\n",
				PCI_EXP_LNKCTL2, lnkctl2);
			pcie_capability_write_word(pdev->bus->self,
						   PCI_EXP_LNKCTL2, lnkctl2);
			pcie_capability_read_word(pdev->bus->self,
						  PCI_EXP_LNKCTL, &reg16);
			dev_dbg(&pdev->dev, "linkctl %x %x\n", PCI_EXP_LNKCTL,
				reg16);
			reg16 |= PCI_EXP_LNKCTL_RL;
			pcie_capability_write_word(pdev->bus->self,
						   PCI_EXP_LNKCTL, reg16);
			dev_dbg(&pdev->dev, "New linkctl %x %x\n",
				PCI_EXP_LNKCTL, reg16);
			msleep(100);
			dev_warn(
				&pdev->dev,
				"PCIe link speed is Gen%d force Retrain Link to max device speed\n",
				lnksta);
			pcie_capability_read_word(pdev->bus->self,
						  PCI_EXP_LNKCTL, &reg16);
			dev_info(&pdev->dev, "New PCIe link speed id Gen%x\n",
				 reg16);
			if ((reg16 & PCI_EXP_LNKSTA_CLS) !=
			    PCI_EXP_LNKSTA_CLS_8_0GB)
				dev_err(&pdev->dev,
					"Fail to retrain link to max device speed, current is Gen%d\n",
					reg16 & PCI_EXP_LNKSTA_CLS);
		}
	} else
		dev_info(&pdev->dev, "No PCI Express Link Capability\n");

	pci_save_state(pdev);
	axldev->pcie_state = pci_store_saved_state(pdev);
	if (!axldev->pcie_state)
		dev_err(&pdev->dev, "Fail to save pcie state\n");

	return 0;

pci_err_out:
	pci_clear_master(pdev);
	return err;
}

static int axl_aipu_dma_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int i;
	struct workqueue_struct *wq;
	int max_dma_ch;

	if (enable_sg_host_dma) {
		axldev->dev_info->dma_rd_ch -= 1;
		axldev->dev_info->dma_wr_ch -= 1;
	}

	max_dma_ch = axldev->dev_info->dma_rd_ch;
	axldev->dma_wrqc = devm_kcalloc(&pdev->dev, max_dma_ch,
					sizeof(*axldev->dma_wrqc), GFP_KERNEL);
	if (!axldev->dma_wrqc)
		return -ENOMEM;

	axldev->dma_rdqc = devm_kcalloc(&pdev->dev, max_dma_ch,
					sizeof(*axldev->dma_rdqc), GFP_KERNEL);
	if (!axldev->dma_rdqc)
		return -ENOMEM;

	dev_dbg(&pdev->dev, "Allocate workqueue  controllers\n");

	for (i = 0; i < max_dma_ch; i++) {
		axldev->dma_rdqc[i].wq = wq = alloc_ordered_workqueue(
			"%s-rd-%d", WQ_HIGHPRI, axldev->name, i);
		if (!wq) {
			dev_err(&pdev->dev, "Failed to create workqueue\n");
			goto free_rdwq;
		}
		dev_dbg(&pdev->dev, "Create workqueue %s-dma-rd-%d %p\n",
			axldev->name, i, wq);
		atomic_set(&axldev->dma_wrqc[i].count, 0);
		axldev->dma_wrqc[i].axldev = axldev;
		axldev->dma_wrqc[i].id = i;
		axldev->dma_wrqc[i].timeout = get_timeout_ms(dma_timeout);
	}
	for (i = 0; i < max_dma_ch; i++) {
		axldev->dma_wrqc[i].wq = wq = alloc_ordered_workqueue(
			"%s-wr-%d", WQ_HIGHPRI, axldev->name, i);
		if (!wq) {
			dev_err(&pdev->dev, "Failed to create workqueue\n");
			goto free_wrwq;
		}
		dev_dbg(&pdev->dev, "Create workqueue %s-wr-%d %p\n",
			axldev->name, i, wq);
		atomic_set(&axldev->dma_rdqc[i].count, 0);
		axldev->dma_rdqc[i].axldev = axldev;
		axldev->dma_rdqc[i].id = i;
		axldev->dma_rdqc[i].timeout = get_timeout_ms(dma_timeout);
	}
	return 0;

free_wrwq:
	for (i = 0; i < max_dma_ch; i++)
		if (axldev->dma_wrqc[i].wq)
			destroy_workqueue(axldev->dma_wrqc[i].wq);
free_rdwq:
	for (i = 0; i < max_dma_ch; i++)
		if (axldev->dma_rdqc[i].wq)
			destroy_workqueue(axldev->dma_rdqc[i].wq);

	return -ENOMEM;
}

static void axl_aipu_dma_deinit(struct axl_pcie_aipu_dev *axldev)
{
	int i, max_dma_ch = axldev->dev_info->dma_rd_ch;
	dev_dbg(&axldev->pdev->dev, "Destroy workqueue\n");
	for (i = 0; i < max_dma_ch; i++) {
		if (axldev->dma_rdqc[i].wq)
			destroy_workqueue(axldev->dma_rdqc[i].wq);
		if (axldev->dma_wrqc[i].wq)
			destroy_workqueue(axldev->dma_wrqc[i].wq);
	}
}

static struct axl_pcie_aipu_dev *
axl_aipu_allocate_device(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pci_bus *bus = pdev->bus;
	struct axl_pcie_aipu_dev *axldev;

	axldev = devm_kzalloc(&pdev->dev, sizeof(struct axl_pcie_aipu_dev),
			      GFP_KERNEL);
	if (!axldev) {
		dev_err(&pdev->dev, "cannot alloc axldev\n");
		return ERR_PTR(-ENOMEM);
	}
	axldev->res_info = (struct dev_res_info *)devm_kcalloc(
		&pdev->dev, 1, sizeof(struct dev_res_info), GFP_KERNEL);
	if (!axldev->res_info) {
		dev_err(&pdev->dev, "cannot alloc resource info\n");
		return ERR_PTR(-ENOMEM);
	}
	axldev->mem_win = (struct dev_mem_window *)devm_kcalloc(
		&pdev->dev, 1, sizeof(struct dev_mem_window), GFP_KERNEL);
	if (!axldev->mem_win) {
		dev_err(&pdev->dev, "cannot alloc memory window info\n");
		return ERR_PTR(-ENOMEM);
	}
	axldev->minor = axl_aipu_alloc_minor();
	if (axldev->minor < 0) {
		dev_err(&pdev->dev, "cannot allocate minor\n");
		return ERR_PTR(-ENODEV);
	}
	axldev->pdev = pdev;
	axldev->dev_info = (struct axe_device_info *)id->driver_data;
	axldev->ctx_mask = devm_kcalloc(&pdev->dev,
					axldev->dev_info->aicore_count,
					sizeof(uint64_t), GFP_KERNEL);
	if (!axldev->ctx_mask) {
		dev_err(&pdev->dev, "cannot alloc ctx_mask\n");
		return ERR_PTR(-ENOMEM);
	}
	snprintf(axldev->name, NAME_SIZE - 1, "%s-%x:%x:%x",
		 axldev->dev_info->devname, pci_domain_nr(bus),
		 pdev->bus->number, PCI_SLOT(pdev->devfn));

	return axldev;
}

static void axl_aipu_register_dev_fops(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->dev_info->dma_type == EDMA_DMA)
		axl_aipu_edma_register_dev_fops(axldev);
	else
		axl_aipu_hdma_register_dev_fops(axldev);
}

static void axl_aipu_register_msi_dev_fops(struct axl_pcie_aipu_dev *axldev)
{
	if (axldev->dev_info->hw_gen >= AXL_HW_GEN_EUROPA)
		axl_aipu_register_msi_fops(axldev);
	else
		axl_aipu_register_msi_metis_fops(axldev);
}

static struct device_host_drv_t *
axl_aipu_get_hdrv_area(struct axl_pcie_aipu_dev *axldev)
{
	struct device_sys_ctl_t *dsctl = axldev->vl2base;
	if (dsctl->hdrv_mem_ref.magic != SYSCTL_HOST_DRV_AREA_MAGIC) {
		dev_dbg(&axldev->pdev->dev, "Invalid hdrv magic %x\n",
			dsctl->hdrv_mem_ref.magic);
		return NULL;
	}
	return (struct device_host_drv_t *)((uintptr_t)dsctl +
					    dsctl->hdrv_mem_ref.offset);
}

static void axl_aipu_disable_dev_dma(struct pci_dev *pdev)
{
	struct axl_pcie_aipu_dev *axldev = pci_get_drvdata(pdev);
	if (!axldev->hdrv_base)
		return;

	axldev->hdrv_base->ctrl = 0;
}

static void axl_aipu_deinit_dev_dma(struct axl_pcie_aipu_dev *axldev)
{
	struct device_host_drv_t *hdrv;

	hdrv = axl_aipu_get_hdrv_area(axldev);
	if (!hdrv) {
		return;
	}
	hdrv->ctrl = 0;
}

void axl_aipu_config_dev_dma(struct axl_pcie_aipu_dev *axldev)
{
	struct device_host_drv_t *hdrv;
	struct pci_dev *pdev = axldev->pdev;

	hdrv = axl_aipu_get_hdrv_area(axldev);
	if (!hdrv) {
		dev_info(&pdev->dev, "vmsi not available\n");
		return;
	}
	hdrv->target = (u64)axldev->dma_addr;
	hdrv->base = 0x0;
	hdrv->size = axldev->dma_size;
	hdrv->ctrl = 1;
	axldev->hdrv_base = hdrv;
	dev_info(&pdev->dev, "vmsi configured\n");
}

void axl_aipu_config_dev_msi(struct axl_pcie_aipu_dev *axldev)
{
	struct device_vmsi_config_t *msi_cfg;

	msi_cfg = axl_aipu_get_msi_config_area(axldev);
	axldev->msi_cfg = msi_cfg;

	if (axldev->msi_cfg) {
		axldev->max_msi = axldev->msi_cfg->num_vmsi;
	} else if (axldev->dev_info->hw_gen >= AXL_HW_GEN_EUROPA) {
		axldev->max_msi = MAX_VIRT_MSI;
	} else {
		axldev->max_msi = PMSI_MAX;
	}

	axl_aipu_register_msi_dev_fops(axldev);
}

static void axl_aipu_drv_dma_alloc(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	dma_addr_t dma_addr = 0;
	dma_addr_t aligned_dma_addr;
	void *aligned_va;
	size_t offset;

	axldev->dma_size = axldev->dev_info->dma_size;
	axldev->dma_alloc_size = axldev->dma_size * 2;
	axldev->dma_va_unaligned =
		dma_alloc_wc(&pdev->dev, axldev->dma_alloc_size, &dma_addr,
			     GFP_KERNEL | __GFP_NOWARN);
	if (axldev->dma_va_unaligned == NULL) {
		dev_err(&pdev->dev, "Fail to dma alloc %x\n",
			axldev->dma_alloc_size);
		return;
	}
	axldev->dma_addr_unaligned = dma_addr;
	aligned_dma_addr = ALIGN(dma_addr, axldev->dma_size);
	offset = aligned_dma_addr - dma_addr;
	aligned_va = axldev->dma_va_unaligned + offset / sizeof(unsigned long);

	axldev->dma_enabled = 1;
	axldev->dma_addr = aligned_dma_addr;
	axldev->dma_va = aligned_va;

	/* Initialize host-side descriptor buffer after VMSI (4KB) */
	axldev->desc_host_va =
		(struct dw_edma_ll_buf *)((u8 *)aligned_va +
					  sizeof(struct device_virt_msi_t));
	axldev->desc_host_pa =
		aligned_dma_addr + sizeof(struct device_virt_msi_t);
	axldev->desc_host_size = DMA_DESC_BUF_SIZE;

	/* Clear descriptor buffer to ensure no stale data */
	memset(axldev->desc_host_va, 0, axldev->desc_host_size);

	dev_dbg(&pdev->dev,
		"dma alloc unaligned: 0x%llx, aligned: 0x%llx (0x%x), offset: 0x%lx\n",
		dma_addr, aligned_dma_addr, axldev->dma_size, offset);
	dev_dbg(&pdev->dev, "descriptor buffer: va=%p, pa=0x%llx, size=0x%lx\n",
		axldev->desc_host_va, axldev->desc_host_pa,
		axldev->desc_host_size);

	axl_aipu_config_dev_dma(axldev);
}

static void axl_aipu_drv_dma_free(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	if (axldev->dma_enabled) {
		dev_info(&pdev->dev, "Release dma mem %s\n", axldev->name);
		/* Clear host descriptor buffer pointers */
		axldev->desc_host_va = NULL;
		axldev->desc_host_pa = 0;
		dma_free_wc(&pdev->dev, axldev->dma_alloc_size,
			    axldev->dma_va_unaligned,
			    axldev->dma_addr_unaligned);
		axldev->dma_va = NULL;
		axldev->dma_va_unaligned = NULL;
		axldev->dma_enabled = 0;
		axl_aipu_deinit_dev_dma(axldev);
	}
}

static int axl_aipu_create_device(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int err;

	dev_dbg(&pdev->dev, "Add class dev %d:%d\n", axl_aipu_major,
		axldev->minor);
	cdev_init(&axldev->cdev, &axl_aipu_file_fops);
	err = cdev_add(&axldev->cdev, MKDEV(axl_aipu_major, axldev->minor), 1);
	if (err) {
		dev_err(&pdev->dev, "chardev registration failed\n");
		return err;
	}

	dev_dbg(&pdev->dev, "device create\n");
	if (IS_ERR(device_create(axl_aipu_class, &pdev->dev,
				 MKDEV(axl_aipu_major, axldev->minor), axldev,
				 "%s", axldev->name))) {
		dev_err(&pdev->dev, "can't create device\n");
		err = -ENOMEM;
		return err;
	}
	return 0;
}
#ifndef PCI_SUBDEVICE_ID_QEMU
#define PCI_SUBDEVICE_ID_QEMU 0x1100
#endif
#ifndef PCI_VENDOR_ID_REDHAT
#define PCI_VENDOR_ID_REDHAT 0x1b36
#endif

static int axl_aipu_drv_recovery_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	struct pci_dev *parent_port;
	u16 parent_vendor, parent_subsys_device;
	int err;

	if (!axldev->dlllarc) {
		axldev->recovery = NULL;
		return 0;
	}

	/* Check if parent port exists and is not a QEMU device */
	if (pdev->bus && pdev->bus->self) {
		parent_port = pdev->bus->self;
		pci_read_config_word(parent_port, PCI_VENDOR_ID,
				     &parent_vendor);
		pci_read_config_word(parent_port, PCI_SUBSYSTEM_ID,
				     &parent_subsys_device);

		/* Skip recovery thread for QEMU (vendor ID 0x1b36 or subsystem device ID 0x1100) */
		if (parent_vendor == PCI_VENDOR_ID_REDHAT ||
		    parent_subsys_device == PCI_SUBDEVICE_ID_QEMU) {
			dev_info(
				&pdev->dev,
				"QEMU parent port detected, skipping recovery thread\n");
			axldev->recovery = NULL;
			return 0;
		}
	}

	dev_info(&pdev->dev,
		 "Data Link Layer Link Active Reporting capability\n");
	axldev->recovery =
		kthread_run(axl_aipu_recovery, axldev, "%s", axldev->name);
	if (IS_ERR(axldev->recovery)) {
		err = PTR_ERR(axldev->recovery);
		dev_err(&pdev->dev, "Failed to create kernel thread\n");
		return err;
	}
	return 0;
}

/**
 * axl_aipu_trace_alloc - Allocate DMA trace buffer
 * @axldev: Device context
 * @num_entries: Requested number of entries
 *
 * Allocates trace buffer and rounds size to next power of 2.
 * Minimum 64 entries, maximum 65536 entries.
 *
 * Returns: 0 on success, -ENOMEM on failure
 */
static int axl_aipu_trace_alloc(struct axl_pcie_aipu_dev *axldev,
				unsigned int num_entries)
{
	struct dma_trace_buffer *tb;
	unsigned int alloc_size;

	/* Clamp entries between 64 and 65536 */
	num_entries = clamp_t(unsigned int, num_entries, 64, 65536);

	/* Round up to next power of 2 for efficient masking */
	num_entries = roundup_pow_of_two(num_entries);

	/* Allocate management structure */
	tb = devm_kzalloc(&axldev->pdev->dev, sizeof(*tb), GFP_KERNEL);
	if (!tb)
		return -ENOMEM;

	/* Allocate trace entries array */
	alloc_size = num_entries * sizeof(struct dma_trace_entry);
	tb->entries = devm_kzalloc(&axldev->pdev->dev, alloc_size, GFP_KERNEL);
	if (!tb->entries)
		return -ENOMEM;

	/* Initialize circular buffer */
	tb->size = num_entries;
	tb->size_mask = num_entries - 1;
	tb->head = 0;
	tb->tail = 0;
	spin_lock_init(&tb->lock);
	atomic_set(&tb->enabled, 0);
	atomic64_set(&tb->overruns, 0);
	atomic64_set(&tb->total_traces, 0);

	axldev->trace_buf = tb;

	dev_info(&axldev->pdev->dev, "DMA trace buffer: %u entries (%u KB)\n",
		 num_entries, alloc_size / 1024);

	return 0;
}

static int axl_aipu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct axl_pcie_aipu_dev *axldev;
	int err;

	axldev = axl_aipu_allocate_device(pdev, id);
	if (IS_ERR(axldev))
		return PTR_ERR(axldev);

	err = axl_aipu_pci_init(pdev, axldev);
	if (err)
		goto err_out;

	axl_aipu_register_dev_fops(axldev);
	axl_aipu_config_dev_msi(axldev);
	axl_aipu_dev_dynmem_init(axldev);

	mutex_init(&axldev->mutex);
	mutex_init(&axldev->msg_mutex);
	mutex_init(&axldev->desc_mutex);

	spin_lock_init(&axldev->msi_lock);

	axl_aipu_drv_dma_alloc(axldev);

	/* Allocate DMA trace buffer */
	err = axl_aipu_trace_alloc(axldev, dma_trace_entries);
	if (err)
		dev_warn(
			&pdev->dev,
			"Failed to allocate DMA trace buffer, tracing disabled\n");

	err = axl_aipu_create_device(axldev);
	if (err)
		goto err_dev_out;

	err = axl_aipu_drv_recovery_init(axldev);
	if (err)
		goto err_dev_out;

	err = axl_aipu_dma_init(axldev);
	if (err)
		goto err_dev_out;

	err = axl_pci_msi_init(pdev, axldev);
	if (err)
		goto err_dev_out;

	/* Initialize firmware trace consumer */
	err = axl_fwtrace_init(axldev);
	if (err) {
		dev_warn(&pdev->dev,
			 "Failed to initialize firmware trace consumer: %d\n",
			 err);
		/* Non-fatal, continue */
	}

	axl_aipu_dev_debugfs_init(axldev);

	return 0;

err_dev_out:
	axl_aipu_dma_deinit(axldev);
	device_destroy(axl_aipu_class, MKDEV(axl_aipu_major, axldev->minor));
	cdev_del(&axldev->cdev);

err_out:
	axl_aipu_drv_dma_free(axldev);
	axl_aipu_free_minor(axldev);
	axl_aipu_pci_deinit(pdev);
	return err;
}

/*
 * axl_aipu_sync_irqs() - Wait for all MSI handlers to finish executing.
 *
 * Calls synchronize_irq() on every allocated MSI vector so that no interrupt
 * handler is running (or pending) once this returns. Must be called after the
 * device's interrupt generation has been stopped and before resources the
 * handlers reference (e.g. the VMSI DMA buffer) are released.
 */
static void axl_aipu_sync_irqs(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int i;

	for (i = 0; i < axldev->nmsi; i++)
		synchronize_irq(pci_irq_vector(pdev, i));
}

static void axl_aipu_remove(struct pci_dev *pdev)
{
	struct axl_pcie_aipu_dev *axldev = pci_get_drvdata(pdev);
	unsigned int minor = MINOR(axldev->cdev.dev);

	/*
	 * Stop the device from generating any further interrupts before
	 * tearing anything down. pci_clear_master() clears Bus Master Enable,
	 * which immediately stops bus-master MSI writes even if the link/FW is
	 * gone. axl_aipu_sync_irqs() then drains any interrupt already latched
	 * in the APIC or in-flight on another CPU, so no handler is running once
	 * it returns. Only then is the VMSI DMA buffer (axldev->dma_va) safe to
	 * release in axl_aipu_drv_dma_free().
	 */
	pci_clear_master(pdev);
	axl_aipu_sync_irqs(axldev);

	/* Cooperatively quiesce the device DMA engine (hdrv->ctrl = 0). */
	axl_aipu_disable_dev_dma(pdev);

	axl_aipu_dev_debugfs_exit(axldev);

	/* Cleanup firmware trace consumer */
	axl_fwtrace_cleanup(axldev);

	device_destroy(axl_aipu_class, MKDEV(axl_aipu_major, axldev->minor));
	cdev_del(&axldev->cdev);
	axl_aipu_free_minor(axldev);

	dev_info(&pdev->dev, "Unregistered %s (%d %d)\n", axldev->name, minor,
		 axldev->minor);

	axl_aipu_drv_dma_free(axldev);
	axl_aipu_dma_deinit(axldev);
	if (axldev->recovery)
		kthread_stop(axldev->recovery);
	if (axldev->pcie_state)
		kfree(axldev->pcie_state);

	axl_aipu_pci_deinit(pdev);
}

static pci_ers_result_t axl_mmio_enabled(struct pci_dev *pdev)
{
	dev_dbg(&pdev->dev, "axl mmio_enabled\n");
	return PCI_ERS_RESULT_DISCONNECT;
}

/**
 * axl_io_resume
 * This callback is called when the error recovery driver tells us that
 * its OK to resume normal operation.
 */
static void axl_io_resume(struct pci_dev *pdev)
{
	int ret;

	dev_dbg(&pdev->dev, "axl io resume\n");

	pci_restore_state(pdev);
	ret = pci_enable_device(pdev);
	if (ret)
		dev_err(&pdev->dev, "pci_enable_device failed (%d) ", ret);
	pci_set_master(pdev);
}

static void axl_aipu_shutdown(struct pci_dev *pdev)
{
	dev_dbg(&pdev->dev, "Shutdown\n");
	pci_clear_master(pdev);
}

/**
 * axl_io_error_detected - called when PCI error is detected
 * @pdev: Pointer to PCI device
 * @state: The current pci connection state
 *
 * This function is called after a PCI bus error affecting
 * this device has been detected.
 */
static pci_ers_result_t axl_io_error_detected(struct pci_dev *pdev,
					      pci_channel_state_t state)
{
	struct pci_dev *port = pdev->bus->self;
	u16 pci_config_word;

	dev_dbg(&pdev->dev, "%s : pci channel state %x\n", __func__, state);
	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	pci_read_config_word(pdev, 0x0, &pci_config_word);
	dev_dbg(&pdev->dev, "Try to read ID 0x%x\n", pci_config_word);
	if (pci_config_word != 0xFFFF) {
		dev_dbg(&pdev->dev, "Don't need to reset device ID 0x%x\n",
			pci_config_word);
		return PCI_ERS_RESULT_NONE;
	}
	pci_disable_device(pdev);

	if (port != NULL) {
		dev_warn(&pdev->dev, "%s : Request a slot reset %p:%p\n",
			 __func__, pdev, port);
		pci_bridge_secondary_bus_reset(port);
		pdev->state_saved = true;
		pci_restore_state(pdev);
	}

	return PCI_ERS_RESULT_NEED_RESET;
}

/**
 * axl_io_slot_reset - called after the pci bus has been reset.
 * @pdev: Pointer to PCI device
 *
 * Restart the card from scratch
 */
static pci_ers_result_t axl_io_slot_reset(struct pci_dev *pdev)
{
	int err;
	pci_ers_result_t result;

	dev_warn(&pdev->dev, "Called after the pci bus has been reset\n");

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev,
			"Cannot re-enable PCI device after reset.\n");
		result = PCI_ERS_RESULT_DISCONNECT;
	} else {
		dev_info(&pdev->dev, "Restore bars\n");
		pdev->state_saved = true;
		pci_restore_state(pdev);
		pci_set_master(pdev);
		pci_enable_wake(pdev, PCI_D3hot, 0);
		pci_enable_wake(pdev, PCI_D3cold, 0);

		result = PCI_ERS_RESULT_RECOVERED;
	}

	pci_aer_clear_nonfatal_status(pdev);
	pci_set_master(pdev);

	return result;
}

/**
 * axl_reset_done - notify device driver of reset
 * @dev: device to be notified of reset
 *
 */
static void axl_reset_done(struct pci_dev *dev)
{
	dev_info(&dev->dev, "axl reset notify:done\n");
}

/**
 * axl_reset_prepare - notify device driver of reset
 * @dev: device to be notified of reset
 *
 */
static void axl_reset_prepare(struct pci_dev *dev)
{
	struct axl_pcie_aipu_dev *axldev = pci_get_drvdata(dev);

	axldev->dev_state = 1;
	axl_aipu_fwtrace_kill_sessions(axldev);
	dev_info(&dev->dev, "axl reset notify:prepare\n");
}

/* PCI Error Recovery (ERS) */
static const struct pci_error_handlers axl_err_handler = {
	.error_detected = axl_io_error_detected,
	.mmio_enabled = axl_mmio_enabled,
	.slot_reset = axl_io_slot_reset,
	.reset_prepare = axl_reset_prepare,
	.reset_done = axl_reset_done,
	.resume = axl_io_resume,
};

static struct axe_device_info axl_aipu_qemu_device_info = {
	.name = "metis qemu",
	.devname = "metis",
	.hw_gen = AXL_HW_GEN_METIS,
	.mode = AXL_QEMU_MODE,
	.dma_rd_ch = EDMA_V0_MAX_NR_CH,
	.dma_wr_ch = EDMA_V0_MAX_NR_CH,
	.dma_type = EDMA_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 4,
};
static struct axe_device_info axl_aipu_silicon_device_info = {
	.name = "metis silicon",
	.devname = "metis",
	.hw_gen = AXL_HW_GEN_METIS,
	.mode = AXL_SILICON_MODE,
	.dma_rd_ch = EDMA_V0_MAX_NR_CH,
	.dma_wr_ch = EDMA_V0_MAX_NR_CH,
	.dma_type = EDMA_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 4,
};

static const struct axe_device_info axl_aipu_europa = {
	.name = "europa silicon",
	.devname = "europa",
	.hw_gen = AXL_HW_GEN_EUROPA,
	.mode = AXL_SILICON_MODE,
	.dma_rd_ch = HDMA_V0_MAX_NR_CH,
	.dma_wr_ch = HDMA_V0_MAX_NR_CH,
	.dma_type = HYPER_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 8,
	.pve_core_count = 16,
};
static const struct axe_device_info axl_aipu_qemu_europa = {
	.name = "europa qemu",
	.devname = "europa",
	.hw_gen = AXL_HW_GEN_EUROPA,
	.mode = AXL_QEMU_MODE,
	.dma_rd_ch = HDMA_V0_MAX_NR_CH,
	.dma_wr_ch = HDMA_V0_MAX_NR_CH,
	.dma_type = HYPER_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 8,
	.pve_core_count = 16,
};
/*
 * Macro is used to create the struct pci_device_id that matches
 * the supported Axelera PCIe-devices
 * @devname: Capitalized name of the particular device
 * @data: Variable passed to the driver of the particular device
 */
#define AXE_PCI_SIM_DEVICE_IDS(devname, data)                                  \
	.vendor = AXL_VENDOR_TEST, .device = devname, .subvendor = PCI_ANY_ID, \
	.subdevice = PCI_ANY_ID, .driver_data = (kernel_ulong_t)&data

#define AXE_PCI_DEVICE_IDS(devname, data)                 \
	.vendor = AXELERA_VENDOR_ID, .device = devname,   \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
	.driver_data = (kernel_ulong_t)&data
static const struct pci_device_id axl_pci_tbl[] = {
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_SYNOPSYS, axl_aipu_qemu_device_info) },
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_QEMU_OMEGA,
				 axl_aipu_qemu_device_info) },
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_QEMU_EUROPA, axl_aipu_qemu_europa) },
	{ AXE_PCI_DEVICE_IDS(AXL_AIPU_ALPHA_DEVICE_ID,
			     axl_aipu_silicon_device_info) },
	{ AXE_PCI_DEVICE_IDS(AXL_AIPU_OMEGA_DEVICE_ID,
			     axl_aipu_silicon_device_info) },
	{ AXE_PCI_DEVICE_IDS(AXL_AIPU_EUROPA_DEVICE_ID, axl_aipu_europa) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, axl_pci_tbl);

static struct pci_driver axl_aipu_pci_driver = {
	.name = "axl",
	.id_table = axl_pci_tbl,
	.probe = axl_aipu_probe,
	.remove = axl_aipu_remove,
	.shutdown = axl_aipu_shutdown,
	.err_handler = &axl_err_handler,
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 14, 0) || defined(RHEL_RELEASE_CODE)
	.dev_groups = axl_aipu_pcie_axl_groups,
#endif
};

static CLASS_ATTR_STRING(version, 0444, DRIVER_VERSION);

#define AXL_AIPU_BUF_LEN 32
static ssize_t axl_aipu_drv_debugfs_version_show(struct file *filp,
						 char __user *ubuf,
						 size_t count, loff_t *offp)
{
	ssize_t pos;
	size_t buf_size;
	char buf[AXL_AIPU_BUF_LEN];

	buf_size = min(count, sizeof(buf));
	pos = scnprintf(buf, buf_size, "%s\n", DRIVER_VERSION);

	return simple_read_from_buffer(ubuf, count, offp, buf, pos);
}

static const struct file_operations axl_aipu_drv_debugfs_version_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_drv_debugfs_version_show
};

static ssize_t axl_aipu_drv_debugfs_vmsi_show(struct file *filp,
					      char __user *ubuf, size_t count,
					      loff_t *ppos)
{
	struct axl_pcie_aipu_dev *axldev = filp->private_data;
	char *strbuf;
	int i;
	size_t size, ret, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = min_t(size_t, count, 0x1000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (strbuf == NULL)
		return -ENOMEM;

	if (!is_vmsi_enabled(axldev)) {
		off += scnprintf(strbuf + off, size - off,
				 "VMSI is disabled\n");
		goto vmsi_dbgfs_out;
	}
	/* Put the data into the string buffer */
	for (i = 0; i < axldev->max_msi; i++) {
		off += scnprintf(strbuf + off, size - off, "VMSI%d: %d\n", i,
				 get_vmsi_count(axldev, i));
	}
vmsi_dbgfs_out:
	ret = simple_read_from_buffer(ubuf, count, ppos, strbuf, off);
	kfree(strbuf);

	return ret;
}
static ssize_t axl_aipu_drv_debugfs_vmsi_write(struct file *file,
					       const char __user *ubuf,
					       size_t size, loff_t *offp)
{
	struct axl_pcie_aipu_dev *axldev = file->private_data;
	int i;

	if (!is_vmsi_enabled(axldev)) {
		dev_warn(&axldev->pdev->dev, "VMSI is disabled\n");
		return -EINVAL;
	}
	for (i = 0; i < axldev->max_msi; i++) {
		clear_vmsi_count(axldev, i);
	}

	return size;
}
static const struct file_operations axl_aipu_drv_debugfs_vmsi_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axl_aipu_drv_debugfs_vmsi_show,
	.write = axl_aipu_drv_debugfs_vmsi_write
};

static void axl_aipu_drv_debugfs_init(void)
{
	axl_aipu_debugfs_root = debugfs_create_dir(DEVICE_CLASS_NAME, NULL);
	if (!axl_aipu_debugfs_root) {
		pr_err("axl_aipu: can't create debugfs root directory for axl_aipu\n");
		return;
	}

	pr_info("axl_aipu: root directory for axl_aipu\n");
	debugfs_create_file("version", 0444, axl_aipu_debugfs_root, NULL,
			    &axl_aipu_drv_debugfs_version_fops);
}
static void axl_aipu_drv_debugfs_exit(void)
{
	debugfs_remove_recursive(axl_aipu_debugfs_root);
	axl_aipu_debugfs_root = NULL;
	pr_info("axl_aipu: debugfs root directory axl_aipu removed\n");
	return;
}

static void axl_aipu_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev)
{
	char name[NAME_SIZE];

	if (!axl_aipu_debugfs_root)
		return;

	snprintf(name, NAME_SIZE, "%s-%s", axldev->dev_info->devname,
		 dev_name(&axldev->pdev->dev));
	axldev->dentry = debugfs_create_dir(name, axl_aipu_debugfs_root);
	if (IS_ERR(axldev->dentry)) {
		dev_err(&axldev->pdev->dev,
			"Failed to create debugfs directory %s\n", name);
		return;
	}

	debugfs_create_file("vmsi", 0644, axldev->dentry, axldev,
			    &axl_aipu_drv_debugfs_vmsi_fops);

	if (axldev->fops->dev_debugfs_init)
		axldev->fops->dev_debugfs_init(axldev);

	axl_fwtrace_debugfs_init(axldev, axldev->dentry);
}

static int __init axl_aipu_init(void)
{
	int retval;
	dev_t dev;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0) || defined(RHEL_RELEASE_CODE)
	axl_aipu_class = class_create(DEVICE_CLASS_NAME);
#else
	axl_aipu_class = class_create(THIS_MODULE, DEVICE_CLASS_NAME);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	axl_aipu_class->dev_groups = axl_aipu_pcie_axl_groups;
#endif
#endif
	if (IS_ERR(axl_aipu_class)) {
		retval = PTR_ERR(axl_aipu_class);
		pr_err("axl_aipu: can't register %s class\n",
		       DEVICE_CLASS_NAME);
		goto err;
	}

	retval = class_create_file(axl_aipu_class, &class_attr_version.attr);
	if (retval) {
		pr_err("%s: can't create sysfs version file\n",
		       DEVICE_CLASS_NAME);
		goto err_class;
	}

	retval = alloc_chrdev_region(&dev, 0, AXL_AIPU_MAX_MINORS,
				     DEVICE_CLASS_NAME);
	if (retval) {
		pr_err("axl_aipu: can't register character device\n");
		goto err_attr;
	}
	axl_aipu_major = MAJOR(dev);

	if (debugfs_initialized())
		axl_aipu_drv_debugfs_init();

	retval = pci_register_driver(&axl_aipu_pci_driver);
	if (retval) {
		pr_err("axl_aipu: can't register pci driver\n");
		goto err_unchr;
	}

	pr_info("Axelera AIPU PCIe Driver, version " DRIVER_VERSION
		", init OK\n");

	return 0;

err_unchr:
	axl_aipu_drv_debugfs_exit();
	unregister_chrdev_region(dev, AXL_AIPU_MAX_MINORS);
err_attr:
	class_remove_file(axl_aipu_class, &class_attr_version.attr);
err_class:
	class_destroy(axl_aipu_class);
err:
	return retval;
}

static void __exit axl_aipu_exit(void)
{
	pci_unregister_driver(&axl_aipu_pci_driver);

	unregister_chrdev_region(MKDEV(axl_aipu_major, 0), AXL_AIPU_MAX_MINORS);

	class_remove_file(axl_aipu_class, &class_attr_version.attr);
	class_destroy(axl_aipu_class);

	axl_aipu_drv_debugfs_exit();
	pr_debug("axl_aipu: module successfully removed\n");
}

module_init(axl_aipu_init);
module_exit(axl_aipu_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
#ifdef RHEL_RELEASE_CODE
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
MODULE_IMPORT_NS(DMA_BUF);
#endif
