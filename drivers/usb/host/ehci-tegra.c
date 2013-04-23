/*
 * EHCI-compliant USB host controller driver for NVIDIA Tegra SoCs
 *
 * Copyright (c) 2010 Google, Inc.
 * Copyright (c) 2009-2012 NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/platform_device.h>
#include <linux/platform_data/tegra_usb.h>
#include <linux/irq.h>
#include <linux/usb/otg.h>
#include <linux/clk.h>
#include <mach/usb_phy.h>
#include <mach/iomap.h>
#include <../tegra_usb_phy.h>
#include <linux/gpio.h>
#include <../gpio-names.h>
#include <mach/board-cardhu-misc.h>
#include <mach/clk.h>
#include "board-cardhu.h"

#if 0
#define EHCI_DBG(stuff...)	pr_info("ehci-tegra: " stuff)
#else
#define EHCI_DBG(stuff...)	do {} while (0)
#endif

static const char driver_name[] = "tegra-ehci";

static int usb3_init = 0;
static struct usb_hcd *usb3_ehci_handle;
static struct delayed_work usb3_ehci_dock_in_work;
static unsigned  int gpio_dock_in_irq = 0;

static struct tegra_ehci_hcd *modem_ehci_tegra;

#define TEGRA_USB_DMA_ALIGN 32
static struct platform_device *dock_port_device;
static struct platform_device *modem_port_device;

struct tegra_ehci_hcd {
	struct ehci_hcd *ehci;
	struct tegra_usb_phy *phy;
	struct clk *clk;
#ifdef CONFIG_USB_OTG_UTILS
	struct otg_transceiver *transceiver;
#endif
	struct mutex sync_lock;
	bool port_resuming;
	unsigned int irq;
	bool bus_suspended_fail;
};

struct dma_align_buffer {
	void *kmalloc_ptr;
	void *old_xfer_buffer;
	u8 data[0];
};

void tegra_usb3_smi_backlight_on_callback(void)
{
	int dock_in = 0;

	if(usb3_init == 1) {
		dock_in = !(gpio_get_value(TEGRA_GPIO_PU4));
		if(dock_in == 1)
			schedule_delayed_work(&usb3_ehci_dock_in_work,0.5*HZ);
	}
}
EXPORT_SYMBOL(tegra_usb3_smi_backlight_on_callback);

static void usb3_ehci_dock_in_work_handler(struct work_struct *w)
{
	printk(KERN_INFO "%s +\n", __func__);
	usb_hcd_resume_root_hub(usb3_ehci_handle);
	msleep(100);
	printk(KERN_INFO "%s -\n", __func__);
}

static irqreturn_t gpio_dock_in_irq_handler(struct usb_hcd *hcd)
{
	int dock_in = 0;

	printk(KERN_INFO "%s +\n", __func__);
	dock_in  = !(gpio_get_value(TEGRA_GPIO_PU4));
	if (usb3_ehci_handle != NULL && dock_in == 1)
		schedule_delayed_work(&usb3_ehci_dock_in_work, 0.5*HZ);
	printk(KERN_INFO "%s dock_in %d -\n", __func__, dock_in);
	return IRQ_HANDLED;
}

static void gpio_dock_in_irq_init(struct usb_hcd *hcd)
{
	int ret = 0;

	ret = gpio_request(TEGRA_GPIO_PU4, "DOCK_IN");
	if (ret < 0)
		printk(KERN_ERR "DOCK_IN GPIO%d request fault!%d\n", TEGRA_GPIO_PU4, ret);

	ret = gpio_direction_input(TEGRA_GPIO_PU4);
	if (ret)
		printk(KERN_ERR "gpio_direction_input failed for input TEGRA_GPIO_PU4=%d\n", TEGRA_GPIO_PU4);

	gpio_dock_in_irq = gpio_to_irq(TEGRA_GPIO_PU4);
	ret = request_irq(gpio_dock_in_irq, gpio_dock_in_irq_handler, IRQF_SHARED|IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "usb3_dock_in_irq_handler", hcd);
	if (ret < 0)
		printk(KERN_ERR "%s: Could not request IRQ for the GPIO dock in, irq = %d, ret = %d\n", __func__, gpio_dock_in_irq, ret);

	printk(KERN_INFO "%s: request irq = %d, ret = %d\n", __func__, gpio_dock_in_irq, ret);
	INIT_DELAYED_WORK(&usb3_ehci_dock_in_work, usb3_ehci_dock_in_work_handler);
}

void tegra_ehci_modem_port_host_reregister(void)
{
	if (!modem_port_device) {
		pr_err("%s: !modem_port_device\n", __func__);
		return -EINVAL;
	}

	tegra_cardhu_usb_utmip_host_unregister(modem_port_device);
	modem_port_device = NULL;
	mdelay(500);
	modem_port_device = tegra_cardhu_usb_utmip_host_register();
}
EXPORT_SYMBOL(tegra_ehci_modem_port_host_reregister);

struct platform_device *dock_port_device_info(void)
{
	return dock_port_device;
}
EXPORT_SYMBOL(dock_port_device_info);

static void free_align_buffer(struct urb *urb)
{
	struct dma_align_buffer *temp = container_of(urb->transfer_buffer,
						struct dma_align_buffer, data);

	if (!(urb->transfer_flags & URB_ALIGNED_TEMP_BUFFER))
		return;

	/* In transaction, DMA from Device */
	if (usb_urb_dir_in(urb))
		memcpy(temp->old_xfer_buffer, temp->data,
					urb->transfer_buffer_length);

	urb->transfer_buffer = temp->old_xfer_buffer;
	urb->transfer_flags &= ~URB_ALIGNED_TEMP_BUFFER;
	kfree(temp->kmalloc_ptr);
}

static int alloc_align_buffer(struct urb *urb, gfp_t mem_flags)
{
	struct dma_align_buffer *temp, *kmalloc_ptr;
	size_t kmalloc_size;

	if (urb->num_sgs || urb->sg ||
		urb->transfer_buffer_length == 0 ||
		!((uintptr_t)urb->transfer_buffer & (TEGRA_USB_DMA_ALIGN - 1)))
		return 0;

	/* Allocate a buffer with enough padding for alignment */
	kmalloc_size = urb->transfer_buffer_length +
		sizeof(struct dma_align_buffer) + TEGRA_USB_DMA_ALIGN - 1;
	kmalloc_ptr = kmalloc(kmalloc_size, mem_flags);

	if (!kmalloc_ptr)
		return -ENOMEM;

	/* Position our struct dma_align_buffer such that data is aligned */
	temp = PTR_ALIGN(kmalloc_ptr + 1, TEGRA_USB_DMA_ALIGN) - 1;
	temp->kmalloc_ptr = kmalloc_ptr;
	temp->old_xfer_buffer = urb->transfer_buffer;
	/* OUT transaction, DMA to Device */
	if (!usb_urb_dir_in(urb))
		memcpy(temp->data, urb->transfer_buffer,
				urb->transfer_buffer_length);

	urb->transfer_buffer = temp->data;
	urb->transfer_flags |= URB_ALIGNED_TEMP_BUFFER;

	return 0;
}

static int tegra_ehci_map_urb_for_dma(struct usb_hcd *hcd,
	struct urb *urb, gfp_t mem_flags)
{
	int ret;

	ret = alloc_align_buffer(urb, mem_flags);
	if (ret)
		return ret;

	ret = usb_hcd_map_urb_for_dma(hcd, urb, mem_flags);

	/* Control packets over dma */
	if (urb->setup_dma)
		dma_sync_single_for_device(hcd->self.controller,
			urb->setup_dma, sizeof(struct usb_ctrlrequest),
			DMA_TO_DEVICE);

	/* urb buffers over dma */
	if (urb->transfer_dma) {
		enum dma_data_direction dir;
		dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;

		dma_sync_single_for_device(hcd->self.controller,
			urb->transfer_dma, urb->transfer_buffer_length, dir);
	}

	if (ret)
		free_align_buffer(urb);

	return ret;
}

static void tegra_ehci_unmap_urb_for_dma(struct usb_hcd *hcd,
	struct urb *urb)
{

	if (urb->transfer_dma) {
		enum dma_data_direction dir;
		dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
		if (dir == DMA_FROM_DEVICE)
			dma_sync_single_for_cpu(hcd->self.controller,
				urb->transfer_dma, urb->transfer_buffer_length,
									   DMA_FROM_DEVICE);
	}

	usb_hcd_unmap_urb_for_dma(hcd, urb);
	free_align_buffer(urb);
}

static irqreturn_t tegra_ehci_irq(struct usb_hcd *hcd)
{
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	irqreturn_t irq_status;

	spin_lock(&ehci->lock);
	irq_status = tegra_usb_phy_irq(tegra->phy);
	if (irq_status == IRQ_NONE) {
		spin_unlock(&ehci->lock);
		return irq_status;
	}
	if (tegra_usb_phy_remote_wakeup(tegra->phy)) {
		ehci_info(ehci, "remote wakeup detected\n");
		usb_hcd_resume_root_hub(hcd);
		spin_unlock(&ehci->lock);
		return irq_status;
	}
	spin_unlock(&ehci->lock);

	EHCI_DBG("%s() cmd = 0x%x, int_sts = 0x%x, portsc = 0x%x\n", __func__,
		ehci_readl(ehci, &ehci->regs->command),
		ehci_readl(ehci, &ehci->regs->status),
		ehci_readl(ehci, &ehci->regs->port_status[0]));

	irq_status = ehci_irq(hcd);

	if (tegra->phy->pdata->phy_intf == TEGRA_USB_PHY_INTF_HSIC)
		ehci->controller_remote_wakeup = false;

	if (ehci->controller_remote_wakeup) {
		ehci->controller_remote_wakeup = false;
		tegra_usb_phy_pre_resume(tegra->phy, true);
		tegra->port_resuming = 1;
	}
	return irq_status;
}


static int tegra_ehci_hub_control(
	struct usb_hcd	*hcd,
	u16	typeReq,
	u16	wValue,
	u16	wIndex,
	char	*buf,
	u16	wLength
)
{
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	int	retval = 0;
	u32 __iomem	*status_reg;

	if (!tegra_usb_phy_hw_accessible(tegra->phy)) {
		if (buf)
			memset(buf, 0, wLength);
		return retval;
	}

	/* Do tegra phy specific actions based on the type request */
	switch (typeReq) {
	case GetPortStatus:
		if (tegra->port_resuming) {
			u32 cmd;
			int delay = ehci->reset_done[wIndex-1] - jiffies;
			/* Sometimes it seems we get called too soon... In that case, wait.*/
			if (delay > 0) {
				ehci_dbg(ehci, "GetPortStatus called too soon, waiting %dms...\n", delay);
				mdelay(jiffies_to_msecs(delay));
			}
			status_reg = &ehci->regs->port_status[(wIndex & 0xff) - 1];
			/* Ensure the port PORT_SUSPEND and PORT_RESUME has cleared */
			if (handshake(ehci, status_reg, (PORT_SUSPEND | PORT_RESUME), 0, 25000)) {
				EHCI_DBG("%s: timeout waiting for SUSPEND to clear\n", __func__);
			}
			tegra_usb_phy_post_resume(tegra->phy);
			tegra->port_resuming = 0;
			/* If run bit is not set by now enable it */
			cmd = ehci_readl(ehci, &ehci->regs->command);
			if (!(cmd & CMD_RUN)) {
				cmd |= CMD_RUN;
				ehci->command |= CMD_RUN;
				ehci_writel(ehci, cmd, &ehci->regs->command);
			}
			/* Now we can safely re-enable irqs */
			ehci_writel(ehci, INTR_MASK, &ehci->regs->intr_enable);
		}
		break;
	case ClearPortFeature:
		if (wValue == USB_PORT_FEAT_SUSPEND) {
			tegra_usb_phy_pre_resume(tegra->phy, false);
			tegra->port_resuming = 1;
		} else if (wValue == USB_PORT_FEAT_ENABLE) {
			u32 temp;
			temp = ehci_readl(ehci, &ehci->regs->port_status[0]) & ~PORT_RWC_BITS;
			ehci_writel(ehci, temp & ~PORT_PE, &ehci->regs->port_status[0]);
			return retval;
		}
		break;
	}

	/* handle ehci hub control request */
	retval = ehci_hub_control(hcd, typeReq, wValue, wIndex, buf, wLength);

	/* do tegra phy specific actions based on the type request */
	if (!retval) {
		switch (typeReq) {
		case SetPortFeature:
			if (wValue == USB_PORT_FEAT_SUSPEND) {
				/* Need a 4ms delay for controller to suspend */
				mdelay(4);
				tegra_usb_phy_post_suspend(tegra->phy);
			} else if (wValue == USB_PORT_FEAT_RESET) {
				if (wIndex == 1)
					tegra_usb_phy_bus_reset(tegra->phy);
			} else if (wValue == USB_PORT_FEAT_POWER) {
				if (wIndex == 1)
					tegra_usb_phy_port_power(tegra->phy);
			}
			break;
		case ClearPortFeature:
			if (wValue == USB_PORT_FEAT_SUSPEND) {
				/* tegra USB controller needs 25 ms to resume the port */
				ehci->reset_done[wIndex-1] = jiffies + msecs_to_jiffies(25);
			}
			break;
		}
	}

	return retval;
}

static void tegra_ehci_shutdown(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);

	pr_info("%s instance %d +\n", __func__, tegra->phy->inst);
	mutex_lock(&tegra->sync_lock);
	del_timer_sync(&ehci->watchdog);
	del_timer_sync(&ehci->iaa_watchdog);
	if (tegra_usb_phy_hw_accessible(tegra->phy)) {
		spin_lock_irq(&ehci->lock);
		ehci_silence_controller(ehci);
		spin_unlock_irq(&ehci->lock);
	}
	mutex_unlock(&tegra->sync_lock);
	pr_info("%s instance %d -\n", __func__, tegra->phy->inst);
}

static int tegra_ehci_setup(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);
	int retval;

	/* EHCI registers start at offset 0x100 */
	ehci->caps = hcd->regs + 0x100;
	ehci->regs = hcd->regs + 0x100 +
				HC_LENGTH(ehci, readl(&ehci->caps->hc_capbase));

	dbg_hcs_params(ehci, "reset");
	dbg_hcc_params(ehci, "reset");

	/* cache this readonly data; minimize chip reads */
	ehci->hcs_params = readl(&ehci->caps->hcs_params);
	ehci->has_hostpc = tegra_usb_phy_has_hostpc(tegra->phy) ? 1 : 0;
	ehci->broken_hostpc_phcd = true;

	hcd->has_tt = 1;

	retval = ehci_halt(ehci);
	if (retval)
		return retval;

	/* data structure init */
	retval = ehci_init(hcd);
	if (retval)
		return retval;

	ehci->sbrn = 0x20;
	ehci->controller_remote_wakeup = false;
	ehci_reset(ehci);
	tegra_usb_phy_reset(tegra->phy);

	ehci_port_power(ehci, 1);
	return retval;
}

#ifdef CONFIG_PM
static int tegra_ehci_bus_suspend(struct usb_hcd *hcd)
{
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);
	int err = 0;
	EHCI_DBG("%s() BEGIN\n", __func__);
	pr_info("%s instance %d +\n", __func__, tegra->phy->inst);

	mutex_lock(&tegra->sync_lock);
	tegra->bus_suspended_fail = false;
	pr_info("%s : ehci_bus_suspend +\n", __func__);
	err = ehci_bus_suspend(hcd);
	pr_info("%s : ehci_bus_suspend, err = %d -\n", __func__, err);
	if (err)
		tegra->bus_suspended_fail = true;
	else
		tegra_usb_phy_suspend(tegra->phy);
	mutex_unlock(&tegra->sync_lock);
	EHCI_DBG("%s() END\n", __func__);

	pr_info("%s instance %d -\n", __func__, tegra->phy->inst);
	return err;
}

static int tegra_ehci_bus_resume(struct usb_hcd *hcd)
{
	struct tegra_ehci_hcd *tegra = dev_get_drvdata(hcd->self.controller);
	int err = 0;
	EHCI_DBG("%s() BEGIN\n", __func__);

	pr_info("%s instance %d +\n", __func__, tegra->phy->inst);
	mutex_lock(&tegra->sync_lock);
	tegra_usb_phy_resume(tegra->phy);
	err = ehci_bus_resume(hcd);
	mutex_unlock(&tegra->sync_lock);
	EHCI_DBG("%s() END\n", __func__);

	pr_info("%s instance %d -\n", __func__, tegra->phy->inst);
	return err;
}
#endif

static const struct hc_driver tegra_ehci_hc_driver = {
	.description		= hcd_name,
	.product_desc		= "Tegra EHCI Host Controller",
	.hcd_priv_size		= sizeof(struct ehci_hcd),
	.flags			= HCD_USB2 | HCD_MEMORY,

	/* standard ehci functions */
	.start			= ehci_run,
	.stop			= ehci_stop,
	.urb_enqueue		= ehci_urb_enqueue,
	.urb_dequeue		= ehci_urb_dequeue,
	.endpoint_disable	= ehci_endpoint_disable,
	.endpoint_reset	= ehci_endpoint_reset,
	.get_frame_number	= ehci_get_frame,
	.hub_status_data	= ehci_hub_status_data,
	.clear_tt_buffer_complete = ehci_clear_tt_buffer_complete,
	.relinquish_port	= ehci_relinquish_port,
	.port_handed_over	= ehci_port_handed_over,

	/* modified ehci functions for tegra */
	.reset			= tegra_ehci_setup,
	.irq			= tegra_ehci_irq,
	.shutdown		= tegra_ehci_shutdown,
	.map_urb_for_dma	= tegra_ehci_map_urb_for_dma,
	.unmap_urb_for_dma	= tegra_ehci_unmap_urb_for_dma,
	.hub_control		= tegra_ehci_hub_control,
#ifdef CONFIG_PM
	.bus_suspend	= tegra_ehci_bus_suspend,
	.bus_resume	= tegra_ehci_bus_resume,
#endif
};

static int tegra_ehci_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct usb_hcd *hcd;
	struct tegra_ehci_hcd *tegra;
	int err = 0;
	int irq;

	printk(KERN_INFO "%s + #####\n", __func__);

	tegra = devm_kzalloc(&pdev->dev, sizeof(struct tegra_ehci_hcd),
		GFP_KERNEL);
	if (!tegra) {
		dev_err(&pdev->dev, "memory alloc failed\n");
		return -ENOMEM;
	}

	mutex_init(&tegra->sync_lock);

	hcd = usb_create_hcd(&tegra_ehci_hc_driver, &pdev->dev,
					dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "unable to create HCD\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, tegra);

	tegra->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(tegra->clk)) {
		dev_err(&pdev->dev, "Can't get ehci clock\n");
		err = PTR_ERR(tegra->clk);
		goto fail_io;
	}
	err = clk_enable(tegra->clk);
	if (err)
		goto fail_clock;
	tegra_periph_reset_assert(tegra->clk);
	udelay(2);
	tegra_periph_reset_deassert(tegra->clk);
	udelay(2);


	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get I/O memory\n");
		err = -ENXIO;
		goto fail_io;
	}
	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);
	hcd->regs = ioremap(res->start, resource_size(res));
	if (!hcd->regs) {
		dev_err(&pdev->dev, "failed to remap I/O memory\n");
		err = -ENOMEM;
		goto fail_io;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		err = -ENODEV;
		goto fail_irq;
	}
	set_irq_flags(irq, IRQF_VALID);
	tegra->irq = irq;

	tegra->phy = tegra_usb_phy_open(pdev);
	if (IS_ERR(tegra->phy)) {
		dev_err(&pdev->dev, "failed to open USB phy\n");
		err = -ENXIO;
		goto fail_irq;
	}

	if (tegra->phy->inst == 2) {
		usb3_ehci_handle = hcd;
		usb3_init = 1;
		gpio_dock_in_irq_init(hcd);
		dock_port_device = pdev;
	}

	err = tegra_usb_phy_power_on(tegra->phy);
	if (err) {
		dev_err(&pdev->dev, "failed to power on the phy\n");
		goto fail_phy;
	}

	err = tegra_usb_phy_init(tegra->phy);
	if (err) {
		dev_err(&pdev->dev, "failed to init the phy\n");
		goto fail_phy;
	}

	err = usb_add_hcd(hcd, irq, IRQF_SHARED | IRQF_TRIGGER_HIGH);
	if (err) {
		dev_err(&pdev->dev, "Failed to add USB HCD, error=%d\n", err);
		goto fail_phy;
	}

	/*err = enable_irq_wake(tegra->irq);
	if (err < 0) {
		dev_warn(&pdev->dev,
				"Couldn't enable USB host mode wakeup, irq=%d, "
				"error=%d\n", irq, err);
		err = 0;
		tegra->irq = 0;
	}*/

	tegra->ehci = hcd_to_ehci(hcd);

#ifdef CONFIG_USB_OTG_UTILS
	if (tegra_usb_phy_otg_supported(tegra->phy)) {
		tegra->transceiver = otg_get_transceiver();
		if (tegra->transceiver)
			otg_set_host(tegra->transceiver, &hcd->self);
	}
#endif

	if (tegra->phy->inst == 1 && tegra3_get_project_id() == TEGRA3_PROJECT_TF300TG) {
		modem_ehci_tegra = tegra;
	} else if (tegra->phy->inst == 1 && tegra3_get_project_id() == TEGRA3_PROJECT_TF300TL) {
		modem_port_device = pdev;
	}

	printk(KERN_INFO "%s - #####\n", __func__);
	return err;

fail_phy:
	tegra_usb_phy_close(tegra->phy);
fail_irq:
	iounmap(hcd->regs);
fail_clock:
	clk_put(tegra->clk);
fail_io:
	usb_put_hcd(hcd);

	return err;
}


#ifdef CONFIG_PM
static int tegra_ehci_resume(struct platform_device *pdev)
{
	struct tegra_ehci_hcd *tegra = platform_get_drvdata(pdev);
	int ret;

	if (tegra->phy->inst == 1 && tegra3_get_project_id() == TEGRA3_PROJECT_P1801)
		gpio_set_value(TEGRA_GPIO_PH7, 1);

	pr_info("%s instance %d +\n", __func__, tegra->phy->inst);
	ret = tegra_usb_phy_power_on(tegra->phy);
	tegra_usb_phy_port_power(tegra->phy);
	pr_info("%s instance %d, ret %d-\n", __func__, tegra->phy->inst, ret);
	return ret;
}

static int tegra_ehci_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct tegra_ehci_hcd *tegra = platform_get_drvdata(pdev);
	int ret;

	if (tegra->phy->inst == 1 && tegra3_get_project_id() == TEGRA3_PROJECT_P1801)
		gpio_set_value(TEGRA_GPIO_PH7, 0);

	pr_info("%s instance %d, bus_suspended_fail %d +\n", __func__, tegra->phy->inst, tegra->bus_suspended_fail);
	/* bus suspend could have failed because of remote wakeup resume */
	if (tegra->bus_suspended_fail) {
		ret = -EBUSY;
		pr_info("%s instance %d, ret %d-\n", __func__, tegra->phy->inst, ret);
		return ret;
	} else {
		ret = tegra_usb_phy_power_off(tegra->phy);
		pr_info("%s instance %d, ret %d-\n", __func__, tegra->phy->inst, ret);
		return ret;
	}
}
#endif

static int tegra_ehci_remove(struct platform_device *pdev)
{
	struct tegra_ehci_hcd *tegra = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = ehci_to_hcd(tegra->ehci);

	if (tegra == NULL || hcd == NULL)
		return -EINVAL;

#ifdef CONFIG_USB_OTG_UTILS
	if (tegra->transceiver) {
		otg_set_host(tegra->transceiver, NULL);
		otg_put_transceiver(tegra->transceiver);
	}
#endif

	if (tegra->phy->inst == 2 && usb3_init == 1) {
		free_irq(gpio_dock_in_irq, hcd);
		usb3_ehci_handle = NULL;
		usb3_init = 0;
		dock_port_device = NULL;
	}

	//if (tegra->irq)
		//disable_irq_wake(tegra->irq);

	/* Make sure phy is powered ON to access USB register */
	if(!tegra_usb_phy_hw_accessible(tegra->phy))
		tegra_usb_phy_power_on(tegra->phy);

	usb_remove_hcd(hcd);
	tegra_usb_phy_power_off(tegra->phy);
	tegra_usb_phy_close(tegra->phy);

	if (tegra->clk) {
		clk_disable(tegra->clk);
		clk_put(tegra->clk);
	}
	iounmap(hcd->regs);
	usb_put_hcd(hcd);

	return 0;
}

static void tegra_ehci_hcd_shutdown(struct platform_device *pdev)
{
	struct tegra_ehci_hcd *tegra = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = ehci_to_hcd(tegra->ehci);

	if (tegra->phy->inst == 1 && tegra3_get_project_id() == TEGRA3_PROJECT_P1801)
		gpio_free(TEGRA_GPIO_PH7);

	pr_info("%s instance %d +\n", __func__, tegra->phy->inst);

	if (tegra->phy->inst == 2 && usb3_init == 1) {
		free_irq(gpio_dock_in_irq, hcd);
		usb3_ehci_handle = NULL;
		usb3_init = 0;
		dock_port_device = NULL;
	}

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);

	pr_info("%s instance %d -\n", __func__, tegra->phy->inst);
}

static struct platform_driver tegra_ehci_driver = {
	.probe		= tegra_ehci_probe,
	.remove	= tegra_ehci_remove,
	.shutdown	= tegra_ehci_hcd_shutdown,
#ifdef CONFIG_PM
	.suspend = tegra_ehci_suspend,
	.resume  = tegra_ehci_resume,
#endif
	.driver	= {
		.name	= driver_name,
	}
};
