/*
 * arch/arm/mach-tegra/e1853/board-e1853.h
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
 */

#ifndef _MACH_TEGRA_BOARD_E1853_H
#define _MACH_TEGRA_BOARD_E1853_H

int e1853_sdhci_init(void);
int e1853_pinmux_init(void);
int e1853_panel_init(void);
int e1853_gpio_init(void);
int e1853_pins_state_init(void);

#endif
