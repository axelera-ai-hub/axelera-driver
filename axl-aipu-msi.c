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

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static irqreturn_t axl_aipu_irq_fn(int irq, void *data);
static irqreturn_t axl_aipu_irq_common_fn(int irq, void *data);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

void irq_poll_check(struct axl_pcie_aipu_dev *axldev, int msi)
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

bool is_vmsi_enabled(struct axl_pcie_aipu_dev *axldev)
{
	return axldev->hdrv_base != NULL;
}

int get_vmsi_count(struct axl_pcie_aipu_dev *axldev, int id)
{
	return atomic_read(&axldev->vmsi_count[id]);
}

void clear_vmsi_count(struct axl_pcie_aipu_dev *axldev, int id)
{
	atomic_set(&axldev->vmsi_count[id], 0);
}

/* ============================================================================
 * MSI Initialization with Trigger Mapping
 * ============================================================================ */

/**
 * axl_aipu_msi_init() - Initialize and register IRQ handlers with trigger mapping
 * @axldev: Device context
 *
 * Registers IRQ handlers based on trigger group configuration.
 * For single-MSI mode: registers one handler for all triggers.
 * For multi-MSI mode: registers handlers for each configured trigger range.
 *
 * Return: 0 on success, negative error code on failure
 */
static int axl_aipu_msi_init(struct axl_pcie_aipu_dev *axldev)
{
	struct pci_dev *pdev = axldev->pdev;
	struct device_vmsi_config_t *msi_cfg = axldev->msi_cfg;
	int err, i;

	if (msi_cfg)
		dev_info(
			&pdev->dev,
			"Initializing MSI with VMSI config (nmsi=%d, pmsi=%d)\n",
			axldev->nmsi, msi_cfg->num_pmsi);
	else
		dev_info(
			&pdev->dev,
			"Initializing MSI without VMSI config (nmsi=%d, max_msi=%d)\n",
			axldev->nmsi, axldev->max_msi);

	/* Initialize all irq_wrk entries */
	for (i = 0; i < axldev->max_msi; i++) {
		axldev->irq_wrk[i].axldev = axldev;
		axldev->irq_wrk[i].id = i;
		axldev->irq_wrk[i].timeout = get_timeout_ms(irq_timeout);
		spin_lock_init(&axldev->irq_wrk[i].irq_lock);
		init_completion(&axldev->irq_wrk[i].irq_done);
		atomic_set(&axldev->irq_wrk[i].dma_done, 0);
		INIT_LIST_HEAD(&axldev->irq_wrk[i].sctx_list);

		/* Set up DMA check callbacks for single-MSI mode */
		if (axldev->nmsi == 1) {
			/* Check DMA trigger ranges */
			if (i >= PMSI_DMA_RD_CH0 && i <= PMSI_DMA_WR_CH3)
				axldev->irq_wrk[i].check = axl_aipu_dma_irq_ck;
			else
				axldev->irq_wrk[i].check = NULL;
		} else {
			/* Multi-MSI mode: no check callbacks needed */
			axldev->irq_wrk[i].check = NULL;
		}
	}

	if (axldev->nmsi == 1) {
		/* Single-MSI mode: one handler scans all VMSI entries */
		char irq_name[NAME_SIZE];
		char *msi_name;

		dev_dbg(&pdev->dev, "Init irq handler for single msi\n");

		snprintf(irq_name, NAME_SIZE - 1, "msi-%s-%d", axldev->name, 0);
		msi_name = devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
		err = devm_request_threaded_irq(&pdev->dev, pdev->irq,
						axl_aipu_irq_handler,
						axl_aipu_irq_common_fn,
						IRQF_SHARED | IRQF_ONESHOT,
						msi_name, &axldev->irq_wrk[0]);
		if (err)
			return err;
	} else if (msi_cfg) {
		/* Multi-MSI mode with VMSI config: use trigger mapping */
		dev_dbg(&pdev->dev,
			"Init irq handlers for multi-msi with VMSI mapping\n");

		for (i = 0; i < msi_cfg->num_pmsi && i < axldev->nmsi; i++) {
			struct vmsi_info_t *vmsi_info =
				&msi_cfg->pmsi_to_vmsi_info[i];
			char irq_name[NAME_SIZE];
			char *msi_name;

			/* Skip disabled entries */
			if (!vmsi_info->enabled)
				continue;

			if (vmsi_info->source == VMSI_SOURCE_FW) {
				dev_dbg(&pdev->dev,
					"Registering PMSI %d -> VMSI [%d-%d] (FW)\n",
					i, vmsi_info->vmsi_range.start,
					vmsi_info->vmsi_range.end);
			} else {
				dev_dbg(&pdev->dev,
					"Registering PMSI %d -> trigger %u (HW)\n",
					i, vmsi_info->pmsi_trigger);
			}

			snprintf(irq_name, NAME_SIZE - 1, "msi-%s-p%d",
				 axldev->name, i);
			msi_name =
				devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
			err = devm_request_threaded_irq(
				&pdev->dev, pdev->irq + i, axl_aipu_irq_handler,
				axl_aipu_irq_fn, IRQF_SHARED | IRQF_ONESHOT,
				msi_name, &axldev->irq_wrk[i]);
			if (err)
				return err;
		}
	} else {
		/* Multi-MSI mode without VMSI config: 1:1 PMSI-to-VMSI mapping */
		dev_dbg(&pdev->dev,
			"Init irq handlers for multi-msi with 1:1 mapping\n");

		for (i = 0; i < axldev->nmsi; i++) {
			char irq_name[NAME_SIZE];
			char *msi_name;

			snprintf(irq_name, NAME_SIZE - 1, "msi-%s-p%d",
				 axldev->name, i);
			msi_name =
				devm_kstrdup(&pdev->dev, irq_name, GFP_KERNEL);
			err = devm_request_threaded_irq(
				&pdev->dev, pdev->irq + i, axl_aipu_irq_handler,
				axl_aipu_irq_fn, IRQF_SHARED | IRQF_ONESHOT,
				msi_name, &axldev->irq_wrk[i]);
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

static struct axl_msi_fops msi_fops = {
	.init = axl_aipu_msi_init,
};

void axl_aipu_register_msi_fops(struct axl_pcie_aipu_dev *axldev)
{
	axldev->msi_fops = &msi_fops;
}

/* ============================================================================
 * VMSI Interrupt Handlers (with trigger config)
 * ============================================================================ */

/**
 * axl_aipu_irq_handler() - Hard IRQ handler
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Quick handler that wakes the threaded handler.
 *
 * Return: IRQ_WAKE_THREAD
 */
irqreturn_t axl_aipu_irq_handler(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;

	/* Quick operations only - just acknowledge and wake thread */
	dev_dbg(&pdev->dev, "Hard IRQ %d triggered\n", irq);

	/* Return IRQ_WAKE_THREAD to run the threaded handler */
	return IRQ_WAKE_THREAD;
}

/**
 * axl_aipu_irq_fn() - Multi-MSI interrupt handler with VMSI config
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Handles interrupts when using multi-MSI mode with trigger grouping.
 * Uses firmware-configured trigger mapping to efficiently scan VMSI ranges.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t axl_aipu_irq_fn(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id = irq - axldev->irq_vec;
	struct device_virt_msi_t *vmsi =
		(struct device_virt_msi_t *)axldev->dma_va;
	struct device_vmsi_config_t *msi_cfg = axldev->msi_cfg;

	dev_dbg(&pdev->dev, "%d interrupt (%d)\n", id, irq);

	if (!msi_cfg) {
		/* No VMSI config: 1:1 PMSI-to-VMSI mapping */
		if (id < axldev->max_msi) {
			atomic_inc(&axldev->vmsi_count[id]);
			complete(&axldev->irq_wrk[id].irq_done);
			irq_poll_check(axldev, id);
		}
		return IRQ_HANDLED;
	}

	if (id < PMSI_MAX) {
		struct vmsi_info_t *info = &msi_cfg->pmsi_to_vmsi_info[id];

		if (!info->enabled)
			return IRQ_HANDLED;

		if (info->source == VMSI_SOURCE_HW) {
			/* HW-sourced: 1:1 mapping, no VMSI_IRQ_EN check */
			int vmsi_id = id; /* PMSI == VMSI for HW sources */

			if (vmsi_id < axldev->max_msi) {
				atomic_inc(&axldev->vmsi_count[vmsi_id]);
				complete(&axldev->irq_wrk[vmsi_id].irq_done);
				irq_poll_check(axldev, vmsi_id);
				dev_dbg(&pdev->dev, "HW VMSI %d (trigger %d)\n",
					vmsi_id, id);
			}
			axl_fwtrace_hw_pmsi_handler(axldev, id);
		} else {
			/* FW-sourced: scan range for VMSI_IRQ_EN */
			int vmsi_id;

			for (vmsi_id = info->vmsi_range.start;
			     vmsi_id <= info->vmsi_range.end; vmsi_id++) {
				if (vmsi_id >= axldev->max_msi)
					break;
				if (vmsi->msi[vmsi_id] & VMSI_IRQ_EN) {
					vmsi->msi[vmsi_id] = 0;
					atomic_inc(
						&axldev->vmsi_count[vmsi_id]);
					complete(&axldev->irq_wrk[vmsi_id]
							  .irq_done);
					irq_poll_check(axldev, vmsi_id);
					if (fwtrace_vmsi_is_source(
						    &axldev->fwtrace, vmsi_id))
						axl_fwtrace_msi_handler(
							axldev, vmsi_id);
					dev_dbg(&pdev->dev,
						"FW VMSI %d in trigger %d\n",
						vmsi_id, id);
				}
			}
		}
	}

	return IRQ_HANDLED;
}

/**
 * axl_aipu_irq_common_fn() - Single-MSI interrupt handler with VMSI config
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Handles interrupts when using single-MSI mode.
 * Uses trigger mapping when available for efficient VMSI scanning.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t axl_aipu_irq_common_fn(int irq, void *data)
{
	struct irq_wrk *irwq_elem = data;
	struct axl_pcie_aipu_dev *axldev = irwq_elem->axldev;
	struct pci_dev *pdev = axldev->pdev;
	int id = irq - axldev->irq_vec;
	struct device_virt_msi_t *vmsi =
		(struct device_virt_msi_t *)axldev->dma_va;
	struct device_vmsi_config_t *msi_cfg = axldev->msi_cfg;
	int trigger_id;

	dev_dbg(&pdev->dev, "%d interrupt (%d)  hdrv %p\n", id, irq,
		axldev->hdrv_base);

	/* Loop through configured PMSI mappings */
	for (trigger_id = 0; trigger_id < PMSI_MAX; trigger_id++) {
		struct vmsi_info_t *info =
			&msi_cfg->pmsi_to_vmsi_info[trigger_id];

		if (!info->enabled)
			continue;

		if (info->source == VMSI_SOURCE_HW) {
			/* HW-sourced: 1:1 mapping, no VMSI_IRQ_EN check */
			int vmsi_id = trigger_id;

			if (vmsi_id >= axldev->max_msi)
				continue;

			atomic_inc(&axldev->vmsi_count[vmsi_id]);
			if (axldev->irq_wrk[vmsi_id].check) {
				if (axldev->irq_wrk[vmsi_id].check(axldev,
								   vmsi_id)) {
					complete(&axldev->irq_wrk[vmsi_id]
							  .irq_done);
					irq_poll_check(axldev, vmsi_id);
				}
			} else {
				complete(&axldev->irq_wrk[vmsi_id].irq_done);
				irq_poll_check(axldev, vmsi_id);
			}
			axl_fwtrace_hw_pmsi_handler(axldev, trigger_id);
		} else {
			/* FW-sourced: scan range for VMSI_IRQ_EN */
			int vmsi_id;

			for (vmsi_id = info->vmsi_range.start;
			     vmsi_id <= info->vmsi_range.end; vmsi_id++) {
				if (vmsi_id >= axldev->max_msi)
					break;
				if (vmsi->msi[vmsi_id] & VMSI_IRQ_EN) {
					dev_dbg(&pdev->dev,
						"VMSI %d in PMSI %d\n", vmsi_id,
						trigger_id);
					vmsi->msi[vmsi_id] = 0;
					atomic_inc(
						&axldev->vmsi_count[vmsi_id]);

					if (axldev->irq_wrk[vmsi_id].check) {
						if (axldev->irq_wrk[vmsi_id]
							    .check(axldev,
								   vmsi_id)) {
							complete(
								&axldev->irq_wrk[vmsi_id]
									 .irq_done);
							irq_poll_check(axldev,
								       vmsi_id);
						}
					} else {
						complete(
							&axldev->irq_wrk[vmsi_id]
								 .irq_done);
						irq_poll_check(axldev, vmsi_id);
					}
					if (fwtrace_vmsi_is_source(
						    &axldev->fwtrace, vmsi_id))
						axl_fwtrace_msi_handler(
							axldev, vmsi_id);
				}
			}
		}
	}

	return IRQ_HANDLED;
}
