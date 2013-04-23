/*
 * arch/arm/mach-tegra/latency_allowance.c
 *
 * Copyright (C) 2011-2012, NVIDIA CORPORATION. All rights reserved. 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/spinlock_types.h>
#include <linux/spinlock.h>
#include <linux/stringify.h>
#include <asm/bug.h>
#include <asm/io.h>
#include <asm/string.h>
#include <mach/iomap.h>
#include <mach/io.h>
#include <mach/latency_allowance.h>
#include "la_priv_common.h"
#include "tegra3_la_priv.h"

#define ENABLE_LA_DEBUG		0
#define TEST_LA_CODE		0

#define la_debug(fmt, ...) \
	if (ENABLE_LA_DEBUG) { \
		printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__); \
	}

static struct dentry *latency_debug_dir;
static DEFINE_SPINLOCK(safety_lock);
static unsigned short id_to_index[ID(MAX_ID) + 1];
static struct la_scaling_info scaling_info[TEGRA_LA_MAX_ID];
static int la_scaling_enable_count;

#define VALIDATE_ID(id) \
	do { \
		if (id >= TEGRA_LA_MAX_ID || id_to_index[id] == 0xFFFF) { \
			pr_err("%s: invalid Id=%d", __func__, id); \
			return -EINVAL; \
		} \
		BUG_ON(la_info_array[id_to_index[id]].id != id); \
	} while (0)

#define VALIDATE_BW(bw_in_mbps) \
	do { \
		if (bw_in_mbps >= 4096) \
			return -EINVAL; \
	} while (0)

#define VALIDATE_THRESHOLDS(tl, tm, th) \
	do { \
		if (tl > 100 || tm > 100 || th > 100) \
			return -EINVAL; \
	} while (0)

static void set_thresholds(struct la_scaling_reg_info *info,
			    enum tegra_la_id id)
{
	unsigned long reg_read;
	unsigned long reg_write;
	unsigned int thresh_low;
	unsigned int thresh_mid;
	unsigned int thresh_high;
	int la_set;
	int idx = id_to_index[id];

	reg_read = readl(la_info_array[idx].reg_addr);
	la_set = (reg_read & la_info_array[idx].mask) >>
		 la_info_array[idx].shift;
	/* la should be set before enabling scaling. */
	BUG_ON(la_set != scaling_info[idx].la_set);

	thresh_low = (scaling_info[idx].threshold_low * la_set) / 100;
	thresh_mid = (scaling_info[idx].threshold_mid * la_set) / 100;
	thresh_high = (scaling_info[idx].threshold_high * la_set) / 100;
	la_debug("%s: la_set=%d, thresh_low=%d(%d%%), thresh_mid=%d(%d%%),"
		" thresh_high=%d(%d%%) ", __func__, la_set,
		thresh_low, scaling_info[idx].threshold_low,
		thresh_mid, scaling_info[idx].threshold_mid,
		thresh_high, scaling_info[idx].threshold_high);

	reg_read = readl(info->tl_reg_addr);
	reg_write = (reg_read & ~info->tl_mask) |
		(thresh_low << info->tl_shift);
	writel(reg_write, info->tl_reg_addr);
	la_debug("reg_addr=0x%x, read=0x%x, write=0x%x",
		(u32)info->tl_reg_addr, (u32)reg_read, (u32)reg_write);

	reg_read = readl(info->tm_reg_addr);
	reg_write = (reg_read & ~info->tm_mask) |
		(thresh_mid << info->tm_shift);
	writel(reg_write, info->tm_reg_addr);
	la_debug("reg_addr=0x%x, read=0x%x, write=0x%x",
		(u32)info->tm_reg_addr, (u32)reg_read, (u32)reg_write);

	reg_read = readl(info->th_reg_addr);
	reg_write = (reg_read & ~info->th_mask) |
		(thresh_high << info->th_shift);
	writel(reg_write, info->th_reg_addr);
	la_debug("reg_addr=0x%x, read=0x%x, write=0x%x",
		(u32)info->th_reg_addr, (u32)reg_read, (u32)reg_write);
}

static void set_disp_latency_thresholds(enum tegra_la_id id)
{
	set_thresholds(&disp_info[id - ID(DISPLAY_0A)], id);
}

static void set_vi_latency_thresholds(enum tegra_la_id id)
{
	set_thresholds(&vi_info[id - ID(VI_WSB)], id);
}

/* Sets latency allowance based on clients memory bandwitdh requirement.
 * Bandwidth passed is in mega bytes per second.
 */
int tegra_set_latency_allowance(enum tegra_la_id id,
				unsigned int bandwidth_in_mbps)
{
	int ideal_la;
	int la_to_set;
	unsigned long reg_read;
	unsigned long reg_write;
	int bytes_per_atom = normal_atom_size;
	struct la_client_info *ci;
	int idx = id_to_index[id];

	VALIDATE_ID(id);
	VALIDATE_BW(bandwidth_in_mbps);

	ci = &la_info_array[idx];

	if (bandwidth_in_mbps == 0) {
		la_to_set = MC_LA_MAX_VALUE;
	} else {
		ideal_la = (ci->fifo_size_in_atoms * bytes_per_atom * 1000) /
			   (bandwidth_in_mbps * ns_per_tick);
		la_to_set = ideal_la - (ci->expiration_in_ns/ns_per_tick) - 1;
	}

	la_debug("\n%s:id=%d,idx=%d, bw=%dmbps, la_to_set=%d",
		__func__, id, idx, bandwidth_in_mbps, la_to_set);
	la_to_set = (la_to_set < 0) ? 0 : la_to_set;
	la_to_set = (la_to_set > MC_LA_MAX_VALUE) ? MC_LA_MAX_VALUE : la_to_set;
	scaling_info[idx].actual_la_to_set = la_to_set;

	/* until display can use latency allowance scaling, use a more
	 * aggressive LA setting. Bug 862709 */
	if (id >= ID(DISPLAY_0A) && id <= ID(DISPLAY_HCB))
		la_to_set /= 3;

	spin_lock(&safety_lock);
	reg_read = readl(ci->reg_addr);
	reg_write = (reg_read & ~ci->mask) |
			(la_to_set << ci->shift);
	writel(reg_write, ci->reg_addr);
	scaling_info[idx].la_set = la_to_set;
	la_debug("reg_addr=0x%x, read=0x%x, write=0x%x",
		(u32)ci->reg_addr, (u32)reg_read, (u32)reg_write);
	spin_unlock(&safety_lock);
	return 0;
}

/* Thresholds for scaling are specified in % of fifo freeness.
 * If threshold_low is specified as 20%, it means when the fifo free
 * between 0 to 20%, use la as programmed_la.
 * If threshold_mid is specified as 50%, it means when the fifo free
 * between 20 to 50%, use la as programmed_la/2 .
 * If threshold_high is specified as 80%, it means when the fifo free
 * between 50 to 80%, use la as programmed_la/4.
 * When the fifo is free between 80 to 100%, use la as 0(highest priority).
 */
int tegra_enable_latency_scaling(enum tegra_la_id id,
				    unsigned int threshold_low,
				    unsigned int threshold_mid,
				    unsigned int threshold_high)
{
	unsigned long reg;
	unsigned long scaling_enable_reg = MC_RA(ARB_OVERRIDE);
	int idx = id_to_index[id];

	VALIDATE_ID(id);
	VALIDATE_THRESHOLDS(threshold_low, threshold_mid, threshold_high);

	if (la_info_array[idx].scaling_supported == false)
		goto exit;

	spin_lock(&safety_lock);

	la_debug("\n%s: id=%d, tl=%d, tm=%d, th=%d", __func__,
		id, threshold_low, threshold_mid, threshold_high);
	scaling_info[idx].threshold_low = threshold_low;
	scaling_info[idx].threshold_mid = threshold_mid;
	scaling_info[idx].threshold_high = threshold_high;
	scaling_info[idx].scaling_ref_count++;

	if (id >= ID(DISPLAY_0A) && id <= ID(DISPLAY_1BB))
		set_disp_latency_thresholds(id);
	else if (id >= ID(VI_WSB) && id <= ID(VI_WY))
		set_vi_latency_thresholds(id);
	if (!la_scaling_enable_count++) {
		reg = readl(scaling_enable_reg);
		reg |= (1 << GLOBAL_LATENCY_SCALING_ENABLE_BIT);
		writel(reg,  scaling_enable_reg);
		la_debug("enabled scaling.");
	}
	spin_unlock(&safety_lock);
exit:
	return 0;
}

void tegra_disable_latency_scaling(enum tegra_la_id id)
{
	unsigned long reg;
	unsigned long scaling_enable_reg = MC_RA(ARB_OVERRIDE);
	int idx;

	BUG_ON(id >= TEGRA_LA_MAX_ID);
	idx = id_to_index[id];
	BUG_ON(la_info_array[idx].id != id);

	if (la_info_array[idx].scaling_supported == false)
		return;
	spin_lock(&safety_lock);
	la_debug("\n%s: id=%d", __func__, id);
	scaling_info[idx].scaling_ref_count--;
	BUG_ON(scaling_info[idx].scaling_ref_count < 0);

	if (!--la_scaling_enable_count) {
		reg = readl(scaling_enable_reg);
		reg = reg & ~(1 << GLOBAL_LATENCY_SCALING_ENABLE_BIT);
		writel(reg, scaling_enable_reg);
		la_debug("disabled scaling.");
	}
	spin_unlock(&safety_lock);
}

void tegra_latency_allowance_update_tick_length(unsigned int new_ns_per_tick)
{
	int i = 0;
	int la;
	unsigned long reg_read;
	unsigned long reg_write;
	unsigned long scale_factor = new_ns_per_tick / ns_per_tick;

	if (scale_factor > 1) {
		spin_lock(&safety_lock);
		ns_per_tick = new_ns_per_tick;
		for (i = 0; i < ARRAY_SIZE(la_info_array) - 1; i++) {
			reg_read = readl(la_info_array[i].reg_addr);
			la = ((reg_read & la_info_array[i].mask) >>
				la_info_array[i].shift) / scale_factor;

			reg_write = (reg_read & ~la_info_array[i].mask) |
					(la << la_info_array[i].shift);
			writel(reg_write, la_info_array[i].reg_addr);
			scaling_info[i].la_set = la;
		}
		spin_unlock(&safety_lock);

		/* Re-scale G2PR, G2SR, G2DR, G2DW with updated ns_per_tick */
		tegra_set_latency_allowance(TEGRA_LA_G2PR, 20);
		tegra_set_latency_allowance(TEGRA_LA_G2SR, 20);
		tegra_set_latency_allowance(TEGRA_LA_G2DR, 20);
		tegra_set_latency_allowance(TEGRA_LA_G2DW, 20);
	}
}

static int la_regs_show(struct seq_file *s, void *unused)
{
	unsigned i;
	unsigned long la;

	/* iterate the list, but don't print MAX_ID */
	for (i = 0; i < ARRAY_SIZE(la_info_array) - 1; i++) {
		la = (readl(la_info_array[i].reg_addr) & la_info_array[i].mask)
			>> la_info_array[i].shift;
		seq_printf(s, "%-16s: %4lu\n", la_info_array[i].name, la);
	}

	return 0;
}

static int dbg_la_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, la_regs_show, inode->i_private);
}

static const struct file_operations regs_fops = {
	.open           = dbg_la_regs_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int __init tegra_latency_allowance_debugfs_init(void)
{
	if (latency_debug_dir)
		return 0;

	latency_debug_dir = debugfs_create_dir("tegra_latency", NULL);

	debugfs_create_file("la_info", S_IRUGO, latency_debug_dir, NULL,
		&regs_fops);

	return 0;
}

late_initcall(tegra_latency_allowance_debugfs_init);

static int __init tegra_latency_allowance_init(void)
{
	unsigned int i;

	la_scaling_enable_count = 0;
	memset(&id_to_index[0], 0xFF, sizeof(id_to_index));

	for (i = 0; i < ARRAY_SIZE(la_info_array); i++)
		id_to_index[la_info_array[i].id] = i;

	tegra_set_latency_allowance(TEGRA_LA_G2PR, 20);
	tegra_set_latency_allowance(TEGRA_LA_G2SR, 20);
	tegra_set_latency_allowance(TEGRA_LA_G2DR, 20);
	tegra_set_latency_allowance(TEGRA_LA_G2DW, 20);
	return 0;
}

core_initcall(tegra_latency_allowance_init);

#if TEST_LA_CODE
#define PRINT_ID_IDX_MAPPING 0
static int __init test_la(void)
{
	int i;
	int err;
	enum tegra_la_id id = 0;
	int repeat_count = 5;

#if PRINT_ID_IDX_MAPPING
	for (i = 0; i < ID(MAX_ID); i++)
		pr_info("ID=0x%x, Idx=0x%x", i, id_to_index[i]);
#endif

	do {
		for (id = 0; id < TEGRA_LA_MAX_ID; id++) {
			err = tegra_set_latency_allowance(id, 200);
			if (err)
				la_debug("\n***tegra_set_latency_allowance,"
					" err=%d", err);
		}

		for (id = 0; id < TEGRA_LA_MAX_ID; id++) {
			if (id >= ID(DISPLAY_0AB) && id <= ID(DISPLAY_HCB))
				continue;
			if (id >= ID(VI_WSB) && id <= ID(VI_WY))
				continue;
			err = tegra_enable_latency_scaling(id, 20, 50, 80);
			if (err)
				la_debug("\n***tegra_enable_latency_scaling,"
					" err=%d", err);
		}

		la_debug("la_scaling_enable_count =%d",
			la_scaling_enable_count);
		for (id = 0; id < TEGRA_LA_MAX_ID; id++) {
			if (id >= ID(DISPLAY_0AB) && id <= ID(DISPLAY_HCB))
				continue;
			if (id >= ID(VI_WSB) && id <= ID(VI_WY))
				continue;
			tegra_disable_latency_scaling(id);
		}
		la_debug("la_scaling_enable_count=%d",
			la_scaling_enable_count);
	} while (--repeat_count);
	return 0;
}

late_initcall(test_la);
#endif
