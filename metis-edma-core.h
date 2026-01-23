/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __METIS_EDMA_CORE_H__
#define __METIS_EDMA_CORE_H__
void edma_register_dev_fops(struct axl_pcie_aipu_dev *axldev);

void edma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev);
void edma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev);
#endif
