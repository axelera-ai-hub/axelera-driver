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

#include "metis-dmabuf.h"
#include "metis.h"
#include "metis-edma-core.h"
#include "metis-hdma-core.h"
#include "metis-version.h"

#define DRIVER_AUTHOR "Axelera AI"
#define DRIVER_DESC   "Axelera AI AIPU driver"

#define AXL_VENDOR_TEST	    0x1234
#define AXL_DEV_TRITON	    0x1972
#define AXL_DEV_SYNOPSYS    0x2023
#define AXL_DEV_QEMU_OMEGA  0x2024
#define AXL_DEV_QEMU_EUROPA 0x2025

#define DEVICE_CLASS_NAME \
	"metis" // keep metis just to be backward compatible with old SDK

#define AXELERA_VENDOR_ID	 0x1F9D
#define AXLAIPU_ALPHA_DEVICE_ID	 0x11AA
#define AXLAIPU_OMEGA_DEVICE_ID	 0x1100
#define AXLAIPU_EUROPA_DEVICE_ID 0x0001

#define METIS_CLASS_CODE  0x1200
#define METIS_REVISION_ID 0x0   

#define AXLAIPU_MAX_MINORS 256

static unsigned char axlaipu_devices[AXLAIPU_MAX_MINORS] = {};
struct dentry *axlaipu_debugfs_root;

static struct class *axlaipu_class;
static int axlaipu_major;

unsigned int dma_timeout = 2;
module_param(dma_timeout, uint, 0644);
MODULE_PARM_DESC(dma_timeout, "DMA timeout in seconds (default 2 sec)");
static unsigned int irq_timeout = 1;
module_param(irq_timeout, uint, 0644);
MODULE_PARM_DESC(irq_timeout, "IRQ timeout in seconds (default 1 sec)");
MODULE_PARM_DESC(enable_dmabuf_sync,
		 "Enable dmabuf sync : 1 enable, 0 disable");

static unsigned int single_msi = 0;
module_param(single_msi, uint, 0644);

unsigned int dma_poll = 0;
module_param(dma_poll, uint, 0644);
MODULE_PARM_DESC(dma_poll, "DMA polling mode (default 0 disabled, 1 enabled)");

static void axlaipu_dma_imwr_restore(struct axl_pcie_aipu_dev *axldev);
static void axl_aipu_config_dev_dma(struct axl_pcie_aipu_dev *axldev);
static void axl_aipu_disable_dev_dma(struct pci_dev *pdev);

static const struct vm_operations_struct axl_physical_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

/* The bridge should allocate at least 47MB; for some hosts, this is allocated only after a bridge rescan */
#define EXPECTED_MEM_BEHIND_BRIDGE_SIZE (47 * 1024 * 1024)

static int apply_resets_if_needed(void)
{
	struct pci_dev *target_device = NULL;
	struct pci_dev *bridge = NULL;
	struct pci_bus *bus = NULL;
	u32 memory_base, memory_limit;
	int ret = 0;
	bool device_found = false;
	struct resource res;
	u16 mem_base_lo, mem_limit_lo;
	unsigned long base, limit;
	struct pci_bus_region region;

	/* Check for the specified devices */
	static const struct {
		u16 device_id;
		const char *device_name;
	} devices[] = { { AXLAIPU_ALPHA_DEVICE_ID, "AXLAIPU_ALPHA_DEVICE_ID" },
			{ AXLAIPU_OMEGA_DEVICE_ID,
			  "AXLAIPU_OMEGA_DEVICE_ID" } };

	int i;
	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		target_device = pci_get_device(AXELERA_VENDOR_ID,
					       devices[i].device_id, NULL);
		if (target_device) {
			dev_info(&target_device->dev,
				 "Found target device: %s\n",
				 devices[i].device_name);
			device_found = true;
			break;
		}
	}

	if (!device_found) {
		pr_info("No target devices found, skipping bridge reset\n");
		return 0; /* No reset needed */
	}

	dev_info(&target_device->dev, "Found target device: %s\n",
		 pci_name(target_device));

	/* Find the bridge the target device is connected to */
	bridge = target_device->bus ? target_device->bus->self : NULL;
	if (!bridge) {
		pr_err("Failed to find the bridge device connected to target device\n");
		pci_dev_put(target_device);
		return -ENODEV;
	}

	dev_info(&bridge->dev, "Found bridge device: %s\n", pci_name(bridge));

	/* Read and decode Memory Behind Bridge */
	pci_read_config_word(bridge, PCI_MEMORY_BASE, &mem_base_lo);
	pci_read_config_word(bridge, PCI_MEMORY_LIMIT, &mem_limit_lo);

	base = ((unsigned long)mem_base_lo & PCI_MEMORY_RANGE_MASK) << 16;
	limit = ((unsigned long)mem_limit_lo & PCI_MEMORY_RANGE_MASK) << 16;

	if (base <= limit) {
		res.flags = (mem_base_lo & PCI_MEMORY_RANGE_TYPE_MASK) |
			    IORESOURCE_MEM;
		region.start = base;
		region.end = limit + 0xfffff;
		pcibios_bus_to_resource(bridge->bus, &res, &region);
		dev_info(&bridge->dev, "Bridge window: %pR\n", &res);
	} else {
		pr_err("Invalid memory base and limit values: base=0x%lx, limit=0x%lx\n",
		       base, limit);
		pci_dev_put(target_device);
		return -EINVAL;
	}

	memory_base = res.start;
	memory_limit = res.end;

	dev_info(&bridge->dev, "Decoded memory behind bridge: %08llx-%08llx\n",
		 res.start, res.end);

	if ((memory_limit - memory_base) >= EXPECTED_MEM_BEHIND_BRIDGE_SIZE) {
		dev_info(
			&bridge->dev,
			"Memory behind bridge is sufficient. Skipping reset.\n");
		pci_dev_put(target_device);
		return 0; /* Skip reset */
	}

	dev_info(
		&bridge->dev,
		"Memory behind bridge is insufficient. Proceeding with reset.\n");

	/* Ensure the bridge has a valid bus */
	bus = bridge->bus;
	if (!bus) {
		pr_err("Bridge device has no associated bus\n");
		pci_dev_put(target_device);
		return -ENODEV;
	}

	/* Remove the bridge */
	dev_info(&bridge->dev, "Removing bridge device: %s\n",
		 pci_name(bridge));
	pci_stop_and_remove_bus_device_locked(bridge);

	/* Rescan the PCI bus */
	ret = pci_rescan_bus(bus);
	if (ret <
	    0) { /* pci_rescan_bus returns the max subordinate bus, not an error code */
		pr_err("Unexpected error during PCI bus rescan\n");
		pci_dev_put(target_device);
		return -EIO;
	}

	dev_info(&bridge->dev,
		 "Bridge removal and PCIe rescan completed successfully\n");

	/* Release reference to the target device */
	pci_dev_put(target_device);

	return 0;
}

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

	INIT_LIST_HEAD(&sys_ctx->node);
	init_waitqueue_head(&sys_ctx->poll_wait_queue);
	atomic_set(&sys_ctx->poll_event_cnt, 0);
	sys_ctx->axldev = axldev;
	file->private_data = sys_ctx;

	dev_dbg(&pdev->dev, "open ctx by %d\n", current->pid);

	return 0;
}

static int sysctrl_release(struct inode *inode, struct file *file)
{
	struct sysctrl_ctx *sys_ctx = file->private_data;
	struct sysctrl_ctx *sctx_msi;
	struct axl_pcie_aipu_dev *axldev = sys_ctx->axldev;
	struct pci_dev *pdev = axldev->pdev;
	struct dmabuf_imp *di = &sys_ctx->di;
	struct irq_wrk *irq_wrk = axldev->irq_wrk;
	int pos, i;

	if (sys_ctx->msg_flag)
		mutex_unlock(&axldev->msg_mutex);

	spin_lock_irq(&axldev->msi_lock);
	for (i = 0; i < MAX_MSI; i++) {
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

	mutex_lock(&axldev->mutex);
	if (sys_ctx->ctx_mask) {
		pos = first_set_bit(sys_ctx->ctx_mask);
		dev_dbg(&pdev->dev, "Release (%d) 0x%llx %p by %d\n", pos,
			axldev->glob_ctx_mask, file->private_data,
			current->pid);
		axldev->glob_ctx_mask &= ~(sys_ctx->ctx_mask);
		axldev->ctx_mask[pos] = 0;
	} else
		dev_dbg(&pdev->dev, "Release no ctx allocated %p by %d\n",
			file->private_data, current->pid);

	mutex_unlock(&axldev->mutex);
	if (sys_ctx->dma_wrk) {
		flush_work(&sys_ctx->dma_wrk->work);
		kfree(sys_ctx->dma_wrk);
	}
	if (di->dmabuf) {
		dev_dbg(&pdev->dev, "Force release dmabuf\n");
		dma_buf_unmap_attachment(di->attachment, di->table,
					 DMA_BIDIRECTIONAL);
		dma_buf_detach(di->dmabuf, di->attachment);
		dma_buf_put(di->dmabuf);
	}

	wake_up_interruptible_poll(&sys_ctx->poll_wait_queue, EPOLLIN);

	kfree(sys_ctx);
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

	dev_dbg(&pdev->dev, "sysctrl_poll wait %p\n", sys_ctx);

	poll_wait(file, &sys_ctx->poll_wait_queue, wait);

	if (atomic_read(&sys_ctx->poll_event_cnt) > 0) {
		return EPOLLIN;
	}

	return 0;
}

static struct file_operations axlaipu_file_fops = {
	.owner = THIS_MODULE,
	.open = sysctrl_open,
	.release = sysctrl_release,
	.unlocked_ioctl = sysctl_ioctl,
	.compat_ioctl = sysctl_ioctl,
	.mmap = sysctl_mmap,
	.poll = sysctrl_poll,
};

static ssize_t axlaipu_restore_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct axl_pcie_aipu_dev *axldev = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", axldev->dlllarc);
}
static ssize_t axlaipu_restore_store(struct device *dev,
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
	return count;
}
static DEVICE_ATTR(axlaipu_restore, 0664, axlaipu_restore_show,
		   axlaipu_restore_store);

static struct attribute *axlaipu_pcie_axl_attrs[] = {
	&dev_attr_axlaipu_restore.attr,
	NULL,
};
ATTRIBUTE_GROUPS(axlaipu_pcie_axl);

static int axlaipu_recovery(void *data)
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
			if (!axldev->dev_state)
				dev_info(&pdev->dev, "Link down\n");
			axldev->dev_state = 1;
		}

		if (axldev->dev_state && (lnksta & PCI_EXP_LNKSTA_DLLLA)) {
			dev_info(&pdev->dev, "Link up\n");
			link = 1;
			msleep(1000);
			pci_load_saved_state(pdev, axldev->pcie_state);
			pci_restore_state(pdev);
			ret = pci_enable_device(pdev);
			if (ret)
				dev_err(&pdev->dev,
					"pci_enable_device failed (%d) ", ret);
			else
				axldev->dev_state = 0;
			axlaipu_dma_imwr_restore(axldev);
			axl_aipu_config_dev_dma(axldev);
		}

		link = (lnksta & PCI_EXP_LNKSTA_DLLLA) ? 1 : 0;
		/*The teoretical Gen1 to Gen3  switch according to spec is 100 ms .. 800 ms.
		For safety: we set this to 80 ms and this should work every time now even with our
		Omega boot code now in ROM*/
		msleep(80);
	}

	return 0;
}

static void irq_poll_check(struct axl_pcie_aipu_dev *axldev, int msi)
{
	struct sysctrl_ctx *sys_ctx;
	struct irq_wrk *irq_wrk = axldev->irq_wrk;

	spin_lock(&axldev->msi_lock);
	list_for_each_entry(sys_ctx, &irq_wrk[msi].sctx_list, node)
	{
		dev_dbg(&axldev->pdev->dev, "Wake up poll at %p for MSI%d\n",
			sys_ctx, msi);
		atomic_inc(&sys_ctx->poll_event_cnt);
		wake_up_interruptible_poll(&sys_ctx->poll_wait_queue, EPOLLIN);
	}
	spin_unlock(&axldev->msi_lock);
}

static irqreturn_t metis_irq_fn(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id = irq - axldev->irq_vec;
	dev_dbg(&pdev->dev, "%d interrupt (%d)\n", id, irq);
	complete(&irwq_elem->irq_done);

	irq_poll_check(axldev, id);
	return IRQ_HANDLED;
}

static irqreturn_t metis_irq_handler(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;

	/* Quick operations only - just acknowledge and wake thread */
	dev_dbg(&pdev->dev, "Hard IRQ %d triggered\n", irq);

	/* Return IRQ_WAKE_THREAD to run the threaded handler */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t metis_irq_common_fn(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id = irq - axldev->irq_vec;
	struct device_virt_msi_t *vmsi;

	dev_dbg(&pdev->dev, "%d interrupt (%d)  hdrv %p\n", id, irq,
		axldev->hdrv_base);

	if (axldev->hdrv_base) {
		vmsi = (struct device_virt_msi_t *)axldev->dma_va;
		for (id = 0; id < MAX_MSI; id++) {
			if (vmsi->msi[id] && VMSI_IRQ_EN) {
				dev_dbg(&pdev->dev, "VMSI (%d)\n", id);
				vmsi->msi[id] = 0;
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
				continue;
			}
			if (NULL == axldev->irq_wrk[id].check)
				continue;
			if (axldev->irq_wrk[id].check(axldev, id)) {
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
			}
		}
	} else {
		for (id = 0; id < MAX_MSI; id++) {
			if (id == MSI_MSG) {
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
			}
			if (id == MSI_DEV_AXE_MSG) {
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
			}
			if (NULL == axldev->irq_wrk[id].check)
				continue;
			if (axldev->irq_wrk[id].check(axldev, id)) {
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
			}
		}
	}

	return IRQ_HANDLED;
}

static int dma_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	return axlaipu_dma_irq_ck(axldev, id);
}
static int krn_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	struct device_sys_ctl_t *dsctl = axldev->vl2base;
	struct device_ctx_t *devctx =
		(struct device_ctx_t *)((uintptr_t)dsctl +
					dsctl->ctx_mem_ref.offset);
	struct pci_dev *pdev = axldev->pdev;
	int sts = readl(&devctx[id].sts);

	if (sts > 0)
		return 0;

	dev_dbg(&pdev->dev, "wake KRN %d (sts %s)\n", id,
		sts == 0 ? "done" : "fail");

	return 1;
}

static int axl_pci_msi_init(struct pci_dev *pdev,
			    struct axl_pcie_aipu_dev *axldev)
{
	int err = 0, i, nmsi;

	nmsi = pci_msi_vec_count(pdev);
	dev_dbg(&pdev->dev, "MSI available %d\n", nmsi);

	if (nmsi != 32) {
		dev_err(&pdev->dev, "Wrong msi number %d\n", nmsi);
		return -ENODEV;
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

	axldev->irq_wrk = devm_kcalloc(&pdev->dev, MAX_MSI,
				       sizeof(*axldev->irq_wrk), GFP_KERNEL);
	if (!axldev->irq_wrk)
		return -ENOMEM;

	axldev->irq_vec = pci_irq_vector(pdev, 0);
	dev_info(&pdev->dev, "irq vec number %d\n", axldev->irq_vec);

	if (axldev->nmsi == 1) {
		char irq_name[NAME_SIZE];
		char *msi_name;

		dev_info(&pdev->dev, "Init irq handler for single msi\n");

		for (i = MSI_KRN_0; i <= MSI_KRN_3; i++) {
			if (!axldev->hdrv_base)
				axldev->irq_wrk[i].check = krn_irq_ck;
			else
				axldev->irq_wrk[i].check = NULL;
			axldev->irq_wrk[i].id = i;
			axldev->irq_wrk[i].axldev = axldev;
			axldev->irq_wrk[i].timeout =
				get_timeout_ms(irq_timeout);
			spin_lock_init(&axldev->irq_wrk[i].irq_lock);
			init_completion(&axldev->irq_wrk[i].irq_done);
			INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);
		}
		for (i = MSI_RD_CH0; i <= MSI_WR_CH3; i++) {
			axldev->irq_wrk[i].check = dma_irq_ck;
			axldev->irq_wrk[i].id = i;
			axldev->irq_wrk[i].axldev = axldev;
			axldev->irq_wrk[i].timeout =
				get_timeout_ms(irq_timeout);
			spin_lock_init(&axldev->irq_wrk[i].irq_lock);
			init_completion(&axldev->irq_wrk[i].irq_done);
			INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);
		}
		for (i = MSI_MSG; i < MAX_MSI; i++) {
			axldev->irq_wrk[i].check = NULL;
			axldev->irq_wrk[i].id = i;
			axldev->irq_wrk[i].axldev = axldev;
			axldev->irq_wrk[i].timeout =
				get_timeout_ms(irq_timeout);
			spin_lock_init(&axldev->irq_wrk[i].irq_lock);
			init_completion(&axldev->irq_wrk[i].irq_done);
			INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);
		}

		snprintf(irq_name, NAME_SIZE - 1, "msi-%s-%d", axldev->name, 0);
		msi_name = devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
		err = devm_request_threaded_irq(&pdev->dev, pdev->irq,
						metis_irq_handler,
						metis_irq_common_fn,
						IRQF_SHARED | IRQF_ONESHOT,
						msi_name, &axldev->irq_wrk[0]);
		if (err)
			return err;

		get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
		axlaipu_dma_init_imwr(axldev);
	} else {
		// better split for each resource dma rd/wr chs kernel/log/traces
		for (i = 0; i < axldev->nmsi; i++) {
			char irq_name[NAME_SIZE];
			char *msi_name;
			axldev->irq_wrk[i].axldev = axldev;
			axldev->irq_wrk[i].id = i;
			axldev->irq_wrk[i].timeout =
				get_timeout_ms(irq_timeout);
			spin_lock_init(&axldev->irq_wrk[i].irq_lock);
			init_completion(&axldev->irq_wrk[i].irq_done);
			INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);
			snprintf(irq_name, NAME_SIZE - 1, "msi-%s-%d",
				 axldev->name, i);
			msi_name =
				devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
			err = devm_request_irq(&pdev->dev, pdev->irq + i,
					       metis_irq_fn, IRQF_SHARED,
					       msi_name, &axldev->irq_wrk[i]);
			if (err)
				return err;
		}
		get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
		axlaipu_dma_init_imwr(axldev);
	}
	dev_dbg(&pdev->dev, "msi_info 0x%x 0x%x : 0x%x\n",
		axldev->irq_msi.address_hi, axldev->irq_msi.address_lo,
		axldev->irq_msi.data);

	axlaipu_dma_enable_ctrl(axldev);

	return 0;
}
static void axlaipu_dma_imwr_restore(struct axl_pcie_aipu_dev *axldev)
{
	get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
	axlaipu_dma_init_imwr(axldev);
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

static int axlaipu_alloc_minor(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int i;

	for (i = 0; i < AXLAIPU_MAX_MINORS; i++)
		if (axlaipu_devices[i] == 0)
			break;
	if (i == AXLAIPU_MAX_MINORS) {
		dev_err(&pdev->dev, "too many devices found!\n");
		return -ENODEV;
	}
	axlaipu_devices[i] = 1;
	return i;
}
static inline void axlaipu_free_minor(struct axl_pcie_aipu_dev *axldev)
{
	axlaipu_devices[axldev->minor] = 0;
}

static void axlaipu_pci_deinit(struct pci_dev *pdev)
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
static int axlaipu_pci_init(struct pci_dev *pdev,

			    struct axl_pcie_aipu_dev *axldev)
{
	int err, mask;
	u16 reg16, lnkctl2, lnksta;
	u32 reg32, lnkcap;

	pci_aer_clear_nonfatal_status(pdev);
	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", err);
		axlaipu_free_minor(axldev);
		return err;
	}
	pci_set_master(pdev);

	err = axl_set_dma_mask(pdev);
	if (err)
		goto pci_err_out;

	pci_set_drvdata(pdev, axldev);

	mask = BIT(BAR_0) | BIT(BAR_2);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
	err = pcim_iomap_regions(pdev, mask, axldev->name);
#else
	err = pcim_iomap_regions_request_all(pdev, mask, axldev->name);
#endif
	if (err) {
		dev_err(&pdev->dev, "Failed to request resources\n");
		err = -ENOMEM;
		goto pci_err_out;
	}
	axldev->dma = pcim_iomap_table(pdev)[BAR_0];
	axldev->pdma = pci_resource_start(pdev, BAR_0);
	axldev->vl2base = pcim_iomap_table(pdev)[BAR_2];
	axldev->pl2base = pci_resource_start(pdev, BAR_2);

	if (!axldev->dma || !axldev->vl2base) {
		dev_err(&pdev->dev, "Failed to map resources %p %p\n",
			axldev->dma, axldev->vl2base);
		goto pci_err_out;
	}

	axldev->res_info->l2_base = axldev->pl2base;
	axldev->res_info->l2_size = pci_resource_len(pdev, BAR_2);

	disable_serr_bit(pdev);
	mask_all_aer_errors(pdev);

	axl_aipu_get_memwindow_info(axldev, pdev);

	if (pdev->bus->self) {
		pcie_capability_read_dword(pdev->bus->self, PCI_EXP_LNKCAP,
					   &reg32);
		if ((reg32 & PCI_EXP_LNKCAP_DLLLARC))
			axldev->dlllarc = 1;
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

	axldev->pdev = pdev;

	return 0;

pci_err_out:
	pci_clear_master(pdev);
	return err;
}

static int axlaipu_dma_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int i;
	struct workqueue_struct *wq;
	int max_dma_ch = axldev->dev_info->dma_rd_ch;

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

static void axlaipu_dma_deinit(struct axl_pcie_aipu_dev *axldev)
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
	axldev->minor = axlaipu_alloc_minor(axldev);
	if (axldev->minor < 0) {
		dev_err(&pdev->dev, "cannot allocate minor\n");
		return ERR_PTR(-ENODEV);
	}
	axldev->pdev = pdev;
	axldev->dev_info = (const struct axe_device_info *)id->driver_data;
	snprintf(axldev->name, NAME_SIZE - 1, "%s-%x:%x:%x",
		 axldev->dev_info->devname, pci_domain_nr(bus),
		 pdev->bus->number, PCI_SLOT(pdev->devfn));

	if (axldev->dev_info->dma_type == EDMA_DMA)
		edma_register_dev_fops(axldev);
	else
		hdma_register_dev_fops(axldev);

	return axldev;
}

static struct device_host_drv_t *
axl_aipu_get_hdrv_area(struct axl_pcie_aipu_dev *axldev)
{
	struct device_sys_ctl_t *dsctl = axldev->vl2base;
	if (dsctl->hdrv_mem_ref.magic != SYSCTL_HOST_DRV_AREA_MAGIC) {
		dev_warn(&axldev->pdev->dev, "Invalid hdrv magic %x\n",
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

static void axl_aipu_config_dev_dma(struct axl_pcie_aipu_dev *axldev)
{
	struct device_host_drv_t *hdrv;
	struct pci_dev *pdev = axldev->pdev;

	if (axldev->dev_info->mode == AXL_QEMU_MODE)
		return;
	hdrv = axl_aipu_get_hdrv_area(axldev);
	if (!hdrv) {
		dev_warn(&pdev->dev, "Fail to get hdrv area\n");
		return;
	}
	hdrv->target = (u64)axldev->dma_addr;
	hdrv->base = 0x0;
	hdrv->size = axldev->dma_size;
	hdrv->ctrl = 1;
	axldev->hdrv_base = hdrv;
}

static void axl_aipu_drv_dma_alloc(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	dma_addr_t dma_addr = 0;

	axldev->dma_size = axldev->dev_info->dma_size;
	axldev->dma_va = dma_alloc_wc(&pdev->dev, axldev->dma_size, &dma_addr,
				      GFP_KERNEL | __GFP_NOWARN);
	if (axldev->dma_va == NULL) {
		dev_err(&pdev->dev, "Fail to dma alloc %x\n", axldev->dma_size);
		return;
	}
	axldev->dma_enabled = 1;
	axldev->dma_addr = dma_addr;
	dev_dbg(&pdev->dev, "dma alloc 0x%llx (0x%x)\n", dma_addr,
		axldev->dma_size);
	axl_aipu_config_dev_dma(axldev);
}

static void axl_aipu_drv_dma_free(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	if (axldev->dma_enabled) {
		dev_info(&pdev->dev, "Release dma mem %s\n", axldev->name);
		dma_free_wc(&pdev->dev, axldev->dma_size, axldev->dma_va,
			    axldev->dma_addr);
	}
}

static int axl_aipu_create_device(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int err;

	dev_dbg(&pdev->dev, "Add class dev %d:%d\n", axlaipu_major,
		axldev->minor);
	cdev_init(&axldev->cdev, &axlaipu_file_fops);
	err = cdev_add(&axldev->cdev, MKDEV(axlaipu_major, axldev->minor), 1);
	if (err) {
		dev_err(&pdev->dev, "chardev registration failed\n");
		return err;
	}

	dev_dbg(&pdev->dev, "device create\n");
	if (IS_ERR(device_create(axlaipu_class, &pdev->dev,
				 MKDEV(axlaipu_major, axldev->minor), axldev,
				 "%s", axldev->name))) {
		dev_err(&pdev->dev, "can't create device\n");
		err = -ENOMEM;
		return err;
	}
	return 0;
}

static int axl_aipu_drv_recovery_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int err;

	if (axldev->dlllarc) {
		dev_info(&pdev->dev,
			 "Data Link Layer Link Active Reporting capability\n");
		axldev->recovery = kthread_run(axlaipu_recovery, axldev, "%s",
					       axldev->name);
		if (IS_ERR(axldev->recovery)) {
			err = PTR_ERR(axldev->recovery);
			pr_err("Failed to create kernel thread\n");
			return err;
		}
	} else
		axldev->recovery = NULL;
	return 0;
}

static int axl_aipu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct axl_pcie_aipu_dev *axldev;
	int err;

	axldev = axl_aipu_allocate_device(pdev, id);
	if (IS_ERR(axldev))
		return PTR_ERR(axldev);

	err = axlaipu_pci_init(pdev, axldev);
	if (err)
		goto err_out;

	mutex_init(&axldev->mutex);
	mutex_init(&axldev->msg_mutex);

	spin_lock_init(&axldev->msi_lock);

	axl_aipu_drv_dma_alloc(axldev);

	err = axl_aipu_create_device(axldev);
	if (err)
		goto err_dev_out;

	err = axl_aipu_drv_recovery_init(axldev);
	if (err)
		goto err_dev_out;

	err = axlaipu_dma_init(axldev);
	if (err)
		goto err_dev_out;

	err = axl_pci_msi_init(pdev, axldev);
	if (err)
		goto err_dev_out;

	axlaipu_dev_debugfs_init(axldev);

	return 0;

err_dev_out:
	axlaipu_dma_deinit(axldev);
	device_destroy(axlaipu_class, MKDEV(axlaipu_major, axldev->minor));
	cdev_del(&axldev->cdev);

err_out:
	axl_aipu_drv_dma_free(axldev);
	axlaipu_free_minor(axldev);
	axlaipu_pci_deinit(pdev);
	return err;
}

static void axl_aipu_remove(struct pci_dev *pdev)
{
	struct axl_pcie_aipu_dev *axldev = pci_get_drvdata(pdev);
	unsigned int minor = MINOR(axldev->cdev.dev);

	axlaipu_dev_debugfs_exit(axldev);
	device_destroy(axlaipu_class, MKDEV(axlaipu_major, axldev->minor));
	cdev_del(&axldev->cdev);
	axlaipu_free_minor(axldev);

	dev_info(&pdev->dev, "Unregistered %s (%d %d)\n", axldev->name, minor,
		 axldev->minor);

	axl_aipu_drv_dma_free(axldev);
	axlaipu_dma_deinit(axldev);
	if (axldev->recovery)
		kthread_stop(axldev->recovery);
	if (axldev->pcie_state)
		kfree(axldev->pcie_state);

	axlaipu_pci_deinit(pdev);
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

static struct axe_device_info axe_axlaipu_qemu_metis = {
	.name = "metis qemu",
	.devname = "metis",
	.mode = AXL_QEMU_MODE,
	.dma_rd_ch = EDMA_V0_MAX_NR_CH,
	.dma_wr_ch = EDMA_V0_MAX_NR_CH,
	.dma_type = EDMA_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 4,
};
static struct axe_device_info axl_aipu_metis = {
	.name = "metis silicon",
	.devname = "metis",
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
	.mode = AXL_SILICON_MODE,
	.dma_rd_ch = HDMA_V0_MAX_NR_CH,
	.dma_wr_ch = HDMA_V0_MAX_NR_CH,
	.dma_type = HYPER_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 4,
	.pve_count = 16,
};
static const struct axe_device_info axl_aipu_qemu_europa = {
	.name = "europa qemu",
	.devname = "europa",
	.mode = AXL_QEMU_MODE,
	.dma_rd_ch = HDMA_V0_MAX_NR_CH,
	.dma_wr_ch = HDMA_V0_MAX_NR_CH,
	.dma_type = HYPER_DMA,
	.dma_size = DMA_SIZE,
	.aicore_count = 4,
	.pve_count = 16,
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
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_SYNOPSYS, axe_axlaipu_qemu_metis) },
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_QEMU_OMEGA, axe_axlaipu_qemu_metis) },
	{ AXE_PCI_SIM_DEVICE_IDS(AXL_DEV_QEMU_EUROPA, axl_aipu_qemu_europa) },
	{ AXE_PCI_DEVICE_IDS(AXLAIPU_ALPHA_DEVICE_ID, axl_aipu_metis) },
	{ AXE_PCI_DEVICE_IDS(AXLAIPU_OMEGA_DEVICE_ID, axl_aipu_metis) },
	{ AXE_PCI_DEVICE_IDS(AXLAIPU_EUROPA_DEVICE_ID, axl_aipu_europa) },
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
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 14, 0)
	.dev_groups = axlaipu_pcie_axl_groups,
#endif
};

static CLASS_ATTR_STRING(version, 0444, DRIVER_VERSION);

#define AXLAIPU_BUF_LEN 32
static ssize_t axlaipu_drv_debugfs_version_show(struct file *filp,
						char __user *ubuf, size_t count,
						loff_t *offp)
{
	ssize_t pos;
	size_t buf_size;
	char buf[AXLAIPU_BUF_LEN];

	buf_size = min(count, sizeof(buf));
	pos = scnprintf(buf, buf_size, "%s\n", DRIVER_VERSION);

	return simple_read_from_buffer(ubuf, count, offp, buf, pos);
}

static const struct file_operations axlaipu_drv_debugfs_version_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = axlaipu_drv_debugfs_version_show
};

static void axlaipu_drv_debugfs_init(void)
{
	axlaipu_debugfs_root = debugfs_create_dir(DEVICE_CLASS_NAME, NULL);
	if (!axlaipu_debugfs_root) {
		pr_err("axlaipu: can't create debugfs root directoryaxlaipu\n");
		return;
	}

	pr_info("axlaipu: root directory for axlaipu\n");
	debugfs_create_file("version", 0444, axlaipu_debugfs_root, NULL,
			    &axlaipu_drv_debugfs_version_fops);
}
static void axlaipu_drv_debugfs_exit(void)
{
	debugfs_remove_recursive(axlaipu_debugfs_root);
	axlaipu_debugfs_root = NULL;
	pr_info("axlaipu: debugfs root directory axlaipu removed ");
	return;
}

static int __init axlaipu_init(void)
{
	int retval;
	dev_t dev;

	/*The bridge on some systems was observed to be incorrectly configured
	* if bridge is in this state, it will need to be rescanned to have
	  it configured properly*/
	retval = apply_resets_if_needed();
	if (retval) {
		pr_info("axl: Bridge not reset becuse of a previously reported error: %u\n",
			retval);
		pr_info("axl: This is not fatal and is normal for passtrough devices\n");
		pr_info("axl: The module will continue to load without attempting bridge reset\n");
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
	axlaipu_class = class_create(THIS_MODULE, DEVICE_CLASS_NAME);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	axlaipu_class->dev_groups = axlaipu_pcie_axl_groups;
#endif
#else
	axlaipu_class = class_create(DEVICE_CLASS_NAME);
#endif
	if (IS_ERR(axlaipu_class)) {
		retval = PTR_ERR(axlaipu_class);
		pr_err("axlaipu: can't register %s class\n", DEVICE_CLASS_NAME);
		goto err;
	}

	retval = class_create_file(axlaipu_class, &class_attr_version.attr);
	if (retval) {
		pr_err("%s: can't create sysfs version file\n",
		       DEVICE_CLASS_NAME);
		goto err_class;
	}

	retval = alloc_chrdev_region(&dev, 0, AXLAIPU_MAX_MINORS,
				     DEVICE_CLASS_NAME);
	if (retval) {
		pr_err("axlaipu: can't register character device\n");
		goto err_attr;
	}
	axlaipu_major = MAJOR(dev);

	if (debugfs_initialized())
		axlaipu_drv_debugfs_init();

	retval = pci_register_driver(&axl_aipu_pci_driver);
	if (retval) {
		pr_err("axlaipu: can't register pci driver\n");
		goto err_unchr;
	}

	pr_info("Triton Linux Driver, version " DRIVER_VERSION ", init OK\n");

	return 0;

err_unchr:
	axlaipu_drv_debugfs_exit();
	unregister_chrdev_region(dev, AXLAIPU_MAX_MINORS);
err_attr:
	class_remove_file(axlaipu_class, &class_attr_version.attr);
err_class:
	class_destroy(axlaipu_class);
err:
	return retval;
}

static void __exit axlaipu_exit(void)
{
	pci_unregister_driver(&axl_aipu_pci_driver);

	unregister_chrdev_region(MKDEV(axlaipu_major, 0), AXLAIPU_MAX_MINORS);

	class_remove_file(axlaipu_class, &class_attr_version.attr);
	class_destroy(axlaipu_class);

	axlaipu_drv_debugfs_exit();
	pr_debug("axlaipu: module successfully removed\n");
}

module_init(axlaipu_init);
module_exit(axlaipu_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
#endif
