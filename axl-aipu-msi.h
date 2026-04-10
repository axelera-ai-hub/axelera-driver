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

#ifndef __AXL_AIPU_MSI_H__
#define __AXL_AIPU_MSI_H__

struct axl_pcie_aipu_dev;

/**
 * axl_aipu_register_msi_fops() - Register MSI function pointers
 * @axldev: Device context
 *
 * Registers VMSI handlers with trigger group mapping.
 */
void axl_aipu_register_msi_fops(struct axl_pcie_aipu_dev *axldev);

/**
 * axl_aipu_register_msi_metis_fops() - Register Metis MSI function pointers
 * @axldev: Device context
 *
 * Registers MSI/VMSI handlers without trigger config.
 */
void axl_aipu_register_msi_metis_fops(struct axl_pcie_aipu_dev *axldev);

/**
 * irq_poll_check() - Wake up poll waiters for MSI
 * @axldev: Device context
 * @msi: MSI/VMSI index
 */
void irq_poll_check(struct axl_pcie_aipu_dev *axldev, int msi);

/**
 * is_vmsi_enabled() - Check if VMSI is enabled for device
 * @axldev: Device context
 *
 * Return: true if VMSI is enabled, false otherwise
 */
bool is_vmsi_enabled(struct axl_pcie_aipu_dev *axldev);

/**
 * get_vmsi_count() - Get VMSI interrupt count
 * @axldev: Device context
 * @id: VMSI index
 *
 * Return: Number of interrupts received for this VMSI
 */
int get_vmsi_count(struct axl_pcie_aipu_dev *axldev, int id);

/**
 * clear_vmsi_count() - Clear VMSI interrupt count
 * @axldev: Device context
 * @id: VMSI index
 */
void clear_vmsi_count(struct axl_pcie_aipu_dev *axldev, int id);

/**
 * axl_aipu_irq_handler() - Hard IRQ handler
 * @irq: IRQ number
 * @data: Pointer to irq_wrk structure
 *
 * Return: IRQ_WAKE_THREAD
 */
irqreturn_t axl_aipu_irq_handler(int irq, void *data);

/* Module parameters */
extern unsigned int irq_timeout;

#endif /* __AXL_AIPU_MSI_H__ */
