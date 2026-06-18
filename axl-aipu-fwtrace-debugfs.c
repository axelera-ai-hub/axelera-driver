// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Axelera AI
 * Firmware log/trace debugfs interface
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/msi.h>
#include <linux/idr.h>
#include <linux/kref.h>

#include "axl-aipu-dmabuf.h"
#include "axl-aipu.h"
#include "axl-aipu-fwtrace.h"

/**
 * fwtrace_stats_show - Show firmware trace statistics
 */
static int fwtrace_stats_show(struct seq_file *m, void *v)
{
	struct axl_pcie_aipu_dev *dev = m->private;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	int i;

	if (!consumer->consumer_enabled) {
		seq_puts(m, "Firmware trace consumer disabled\n");
		return 0;
	}

	seq_printf(m, "Firmware Trace Statistics (%s)\n", dev->dev_info->name);
	seq_printf(m, "=========================\n\n");

	seq_printf(m, "Kernel buffer size: %zu bytes (%zu KB)\n",
		   consumer->kfifo_size, consumer->kfifo_size / 1024);
	seq_printf(m, "DMA buffer size: %zu bytes (%zu KB)\n\n",
		   consumer->dma_buffer_size, consumer->dma_buffer_size / 1024);

	if (!consumer->datastream_base) {
		seq_puts(m, "Firmware datastream area not attached yet\n");
		return 0;
	}

	seq_printf(m,
		   "%-*s %-14s %-12s %-10s %-10s %-10s %-4s %-15s %-15s %-8s\n",
		   DS_NAME_LEN, "Source", "Consumer(kbuf)", "Device(ctrl)",
		   "read_pos", "write_pos", "capacity", "msi", "Total Bytes",
		   "Overruns", "Sessions");
	seq_printf(
		m,
		"-------------------------------------------------------------------------------------------------------------\n");

	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		stream_source_t src = (stream_source_t)i;
		struct fwtrace_buffer *kbuf = &consumer->buffers[src];
		struct fwtrace_ring_state rs = {};
		char name[DS_NAME_LEN];
		bool dev_enabled;

		if (!axl_fwtrace_source_on_dev(dev, src))
			continue;

		axl_fwtrace_get_stream_name(dev, src, name, sizeof(name));

		dev_enabled = axl_fwtrace_get_dev_enabled(dev, src);
		axl_fwtrace_get_ring_state(dev, src, &rs);

		seq_printf(
			m,
			"%-*s %-14s %-12s %-10u %-10u %-10u %-4u %-15llu %-15llu %-8d\n",
			DS_NAME_LEN, name,
			atomic_read(&kbuf->session_count) > 0 ? "yes" : "no",
			dev_enabled ? "yes" : "no", rs.read_pos, rs.write_pos,
			rs.capacity, rs.msi, atomic64_read(&kbuf->total_bytes),
			atomic64_read(&kbuf->overruns),
			atomic_read(&kbuf->session_count));
	}

	return 0;
}

static int fwtrace_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fwtrace_stats_show, inode->i_private);
}

static const struct file_operations fwtrace_stats_fops = {
	.owner = THIS_MODULE,
	.open = fwtrace_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * fwtrace_enable_write - Enable/disable trace sources via debugfs
 */
static ssize_t fwtrace_enable_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct axl_pcie_aipu_dev *dev = file->private_data;
	struct fwtrace_consumer *consumer = &dev->fwtrace;
	char kbuf[64];
	char *cmd, *source_str;
	int source, i;
	bool enable;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	/* Parse: "enable <source>" or "disable <source>" */
	cmd = strim(kbuf);
	if (strncmp(cmd, "enable ", 7) == 0) {
		enable = true;
		source_str = cmd + 7;
	} else if (strncmp(cmd, "disable ", 8) == 0) {
		enable = false;
		source_str = cmd + 8;
	} else {
		return -EINVAL;
	}

	source_str = strim(source_str);

	if (!consumer->datastream_base)
		return -ENODEV;

	/* Find source by firmware-assigned name */
	source = -1;
	for (i = 0; i < STREAM_SOURCE_MAX; i++) {
		char name[DS_NAME_LEN];

		if (!axl_fwtrace_source_on_dev(dev, (stream_source_t)i))
			continue;

		axl_fwtrace_get_stream_name(dev, (stream_source_t)i, name,
					    sizeof(name));
		if (strcmp(source_str, name) == 0) {
			source = i;
			break;
		}
	}

	if (source < 0)
		return -EINVAL;

	if (enable)
		axl_fwtrace_enable(dev, (stream_source_t)source);
	else
		axl_fwtrace_disable(dev, (stream_source_t)source);

	{
		char name[DS_NAME_LEN];

		axl_fwtrace_get_stream_name(dev, (stream_source_t)source, name,
					    sizeof(name));
		dev_info(&dev->pdev->dev, "%s firmware trace source %s\n",
			 enable ? "Enabled" : "Disabled", name);
	}

	return count;
}

static const struct file_operations fwtrace_enable_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = fwtrace_enable_write,
};

/**
 * axl_fwtrace_debugfs_init - Initialize firmware trace debugfs entries
 * @dev: Device structure
 * @parent: Parent debugfs directory
 */
void axl_fwtrace_debugfs_init(struct axl_pcie_aipu_dev *dev,
			      struct dentry *parent)
{
	struct dentry *fwtrace_dir;

	if (!dev->fwtrace.consumer_enabled)
		return;

	fwtrace_dir = debugfs_create_dir("fwtrace", parent);
	if (!fwtrace_dir) {
		dev_warn(&dev->pdev->dev,
			 "Failed to create fwtrace debugfs dir\n");
		return;
	}

	debugfs_create_file("stats", 0444, fwtrace_dir, dev,
			    &fwtrace_stats_fops);
	debugfs_create_file("enable", 0200, fwtrace_dir, dev,
			    &fwtrace_enable_fops);
}
