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
#include <linux/msi.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/poll.h>
#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-msi.h"
#include "axl-aipu-fwtrace.h"

static irqreturn_t axl_aipu_irq_fn_metis(int irq, void *data);
static irqreturn_t axl_aipu_irq_common_fn_metis(int irq, void *data);

static int krn_irq_ck(struct axl_pcie_aipu_dev *axldev, int id)
{
	struct device_sys_ctl_t *dsctl = axldev->vbase;
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

static int axl_aipu_msi_metis_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	int err, i;

	dev_info(&pdev->dev, "Initializing Metis MSI (nmsi=%d, max_msi=%d)\n",
		 axldev->nmsi, axldev->max_msi);

	/* Initialize all irq_wrk entries with appropriate check callbacks */
	for (i = 0; i < axldev->max_msi; i++) {
		axldev->irq_wrk[i].axldev = axldev;
		axldev->irq_wrk[i].id = i;
		axldev->irq_wrk[i].timeout = get_timeout_ms(irq_timeout);
		spin_lock_init(&axldev->irq_wrk[i].irq_lock);
		init_completion(&axldev->irq_wrk[i].irq_done);
		atomic_set(&axldev->irq_wrk[i].dma_done, 0);
		INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);

		axldev->irq_wrk[i].check = NULL;
		/* DMA has internal status check in single MSI mode */
		if (axldev->nmsi == 1) {
			if (i >= PMSI_METIS_RD_CH0 && i <= PMSI_METIS_WR_CH3) {
				axldev->irq_wrk[i].check = axl_aipu_dma_irq_ck;
			} else if (!is_vmsi_enabled(axldev)) {
				if (i >= PMSI_METIS_KRN_0 &&
				    i <= PMSI_METIS_KRN_3)
					axldev->irq_wrk[i].check = krn_irq_ck;
			}
		}
	}

	if (axldev->nmsi == 1) {
		/* Single-MSI mode: one handler scans all VMSIs */
		char irq_name[NAME_SIZE];
		char *msi_name;

		dev_dbg(&pdev->dev,
			"Init irq handler for single msi (metis)\n");

		snprintf(irq_name, NAME_SIZE - 1, "msi-%s-%d", axldev->name, 0);
		msi_name = devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
		err = devm_request_threaded_irq(&pdev->dev, pdev->irq,
						axl_aipu_irq_handler,
						axl_aipu_irq_common_fn_metis,
						IRQF_SHARED | IRQF_ONESHOT,
						msi_name, &axldev->irq_wrk[0]);
		if (err)
			return err;
	} else {
		/* Multi-MSI mode: register handler for each physical MSI */
		dev_dbg(&pdev->dev,
			"Init irq handlers for multi-msi (metis)\n");

		for (i = 0; i < axldev->nmsi; i++) {
			char irq_name[NAME_SIZE];
			char *msi_name;

			snprintf(irq_name, NAME_SIZE - 1, "msi-%s-%d",
				 axldev->name, i);
			msi_name =
				devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
			err = devm_request_irq(&pdev->dev, pdev->irq + i,
					       axl_aipu_irq_fn_metis,
					       IRQF_SHARED, msi_name,
					       &axldev->irq_wrk[i]);
			if (err)
				return err;
		}
	}

	get_cached_msi_msg(axldev->irq_vec, &axldev->irq_msi);
	axl_aipu_dma_init_imwr(axldev);

	return 0;
}

/* ============================================================================
 * MSI Function Operations Registration
 * ============================================================================ */

static struct axl_msi_fops msi_metis_fops = {
	.init = axl_aipu_msi_metis_init,
};

void axl_aipu_register_msi_metis_fops(struct axl_pcie_aipu_dev *axldev)
{
	axldev->msi_fops = &msi_metis_fops;
}

/* ============================================================================
 * Metis MSI/VMSI Interrupt Handlers (without trigger config)
 * ============================================================================ */

/**
 * axl_aipu_irq_fn_metis() - Metis multi-MSI interrupt handler without VMSI config
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Handles interrupts when using multi-MSI mode without trigger mapping.
 * Simple 1:1 mapping between physical MSI trigger and VMSI index.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t axl_aipu_irq_fn_metis(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id = irq - axldev->irq_vec;
	struct device_virt_msi_t *vmsi =
		(struct device_virt_msi_t *)axldev->dma_va;

	dev_dbg(&pdev->dev, "%d interrupt (%d) [metis multi-MSI]\n", id, irq);

	/* Simple 1:1 mapping: trigger id == VMSI id */
	if (vmsi->msi[id] & VMSI_IRQ_EN) {
		vmsi->msi[id] = 0;
		atomic_inc(&axldev->vmsi_count[id]);
	}

	complete(&irwq_elem->irq_done);
	irq_poll_check(axldev, id);
	if (fwtrace_vmsi_is_source(&axldev->fwtrace, id))
		axl_fwtrace_msi_handler(axldev, id);
	return IRQ_HANDLED;
}

/**
 * axl_aipu_irq_common_fn_metis() - Metis single-MSI handler without VMSI config
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Handles interrupts when using single-MSI mode without trigger mapping.
 * Scans all VMSIs sequentially (up to max_msi) or handles Metis MSI.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t axl_aipu_irq_common_fn_metis(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id;

	if (is_vmsi_enabled(axldev)) {
		struct device_virt_msi_t *vmsi =
			(struct device_virt_msi_t *)axldev->dma_va;

		dev_dbg(&pdev->dev, "Single-MSI interrupt [metis VMSI scan]\n");

		for (id = 0; id < axldev->max_msi; id++) {
			if (vmsi->msi[id] & VMSI_IRQ_EN) {
				dev_dbg(&pdev->dev, "VMSI (%d)\n", id);
				vmsi->msi[id] = 0;
				atomic_inc(&axldev->vmsi_count[id]);
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
				if (fwtrace_vmsi_is_source(&axldev->fwtrace,
							   id))
					axl_fwtrace_msi_handler(axldev, id);
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
		dev_dbg(&pdev->dev, "Metis MSI interrupt\n");

		for (id = 0; id < axldev->max_msi; id++) {
			if (id == PMSI_METIS_MSG) {
				complete(&axldev->irq_wrk[id].irq_done);
				irq_poll_check(axldev, id);
			}
			if (id == PMSI_METIS_DEV_AXE_MSG) {
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
