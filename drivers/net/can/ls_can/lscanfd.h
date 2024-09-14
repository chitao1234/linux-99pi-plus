/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LOONGSON LSCANFD controller
 *
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
#ifndef __LSCANFD_H__
#define __LSCANFD_H__

#include <linux/netdevice.h>
#include <linux/can/dev.h>
#include <linux/list.h>

enum lscanfd_can_registers;

struct lscanfd_priv {
	struct can_priv can; /* must be first member! */

	void __iomem *mem_base;
	u32 (*read_reg)(struct lscanfd_priv *priv,
				 enum lscanfd_can_registers reg);
	void (*write_reg)(struct lscanfd_priv *priv,
			  enum lscanfd_can_registers reg, u32 val);

	volatile u8 read_idx;        
	u16 last_res;
	volatile u8 txtb_flags;        
	bool canfd_dmarx;        
	unsigned int ntxbufs;
	spinlock_t tx_lock; /* spinlock to serialize allocation and processing of TX buffers */

	struct napi_struct napi;
	struct device *dev;
	struct clk *can_clk;

	int irq_flags;
	struct dma_chan *rx_ch;  /* dma rx channel            */
	dma_addr_t rx_dma_buf;   /* dma rx buffer bus address */
	unsigned int *rx_buf;   /* dma rx buffer cpu address */
	resource_size_t	 mapbase;		/* for ioremap */
	resource_size_t	 mapsize;
};

/**
 * lscanfd_probe_common - Device type independent registration call
 *
 * This function does all the memory allocation and registration for the CAN
 * device.
 *
 * @dev:	Handle to the generic device structure
 * @addr:	Base address of LS CAN FD core address
 * @irq:	Interrupt number
 * @ntxbufs:	Number of implemented Tx buffers
 * @can_clk_rate: Clock rate, if 0 then clock are taken from device node
 * @pm_enable_call: Whether pm_runtime_enable should be called
 * @set_drvdata_fnc: Function to set network driver data for physical device
 *
 * Return: 0 on success and failure value on error
 */
int lscanfd_probe_common(struct device *dev, void __iomem *addr, resource_size_t mapbase,
			int irq, unsigned int ntxbufs,
			unsigned long can_clk_rate, bool canfd_dmarx,
			int pm_enable_call,
			void (*set_drvdata_fnc)(struct device *dev,
						struct net_device *ndev));

int lscanfd_suspend(struct device *dev) __maybe_unused;
int lscanfd_resume(struct device *dev) __maybe_unused;

#endif /*__LSCANFD__*/
