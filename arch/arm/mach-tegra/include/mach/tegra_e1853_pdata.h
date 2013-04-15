/*
 * arch/arm/mach-tegra/include/mach/tegra_e1853_pdata.h
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __MACH_TEGRA_E1853_PDATA_H
#define __MACH_TEGRA_E1853_PDATA_H

#define NUM_AUDIO_CONTROLLERS 4

/* data format supported */
enum i2s_data_format {
	format_i2s = 0x1,
	format_dsp = 0x2,
	format_rjm = 0x4,
	format_ljm = 0x8,
	format_tdm = 0x10
};

struct codec_info_s {
	/* Name of the Codec Dai on the system */
	char *codec_dai_name;
	/* Name of the I2S controller dai its connected to */
	char *cpu_dai_name;
	char *codec_name;	/* Name of the Codec Driver */
	char *name;			/* Name of the Codec-Dai-Link */
	char *pcm_driver;	/* Name of the PCM driver */
	enum i2s_data_format i2s_format;
	int master;			/* Codec is Master or Slave */
	/* TDM format setttings */
	int num_slots;		/* Number of TDM slots */
	int slot_width;		/* Width of each slot */
	int rx_mask;		/* Number of Rx Enabled slots */
	int tx_mask;		/* Number of Tx Enabled slots */

};

struct tegra_e1853_platform_data {
	struct codec_info_s codec_info[NUM_AUDIO_CONTROLLERS];
};
#endif
