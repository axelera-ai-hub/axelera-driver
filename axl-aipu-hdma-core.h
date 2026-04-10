/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Axelera AI. All rights reserved.  */
#ifndef __AXL_AIPU_HDMA_CORE_H__
#define __AXL_AIPU_HDMA_CORE_H__
void axl_aipu_hdma_register_dev_fops(struct axl_pcie_aipu_dev *axldev);

void axl_aipu_hdma_dev_debugfs_exit(struct axl_pcie_aipu_dev *axldev);
void axl_aipu_hdma_dev_debugfs_init(struct axl_pcie_aipu_dev *axldev);
#endif // __AXL_AIPU_HDMA_CORE_H__
