/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Axelera AI. All rights reserved.  */
#ifndef __METIS_HDMA_CORE_H__
#define __METIS_HDMA_CORE_H__
void hdma_register_dev_fops(struct axl_pcie_aipu_dev *axldev);

void hdma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev);
void hdma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev);
#endif
