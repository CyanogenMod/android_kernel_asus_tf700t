#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/switch.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "board-cardhu.h"
#include "baseband-xmm-power.h"
#include "include/mach/board-cardhu-misc.h"

#include "pm-irq.h"
#include "ril.h"
#include "ril_sim.h"

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

static DEFINE_MUTEX(prox_enable_mtx);
#define _ATTR_MODE S_IRUSR | S_IWUSR | S_IRGRP

static struct workqueue_struct *workqueue;
static struct work_struct prox_work;
static struct work_struct crash_work;
static struct switch_dev prox_sdev;
static struct switch_dev crash_sdev;
static struct device *dev;
static struct class *ril_class;
static dev_t ril_dev;
static int ril_major = 0;
static int ril_minor = 0;
static int proximity_enabled;
static int project_id = 0;
static int do_crash_dump = 0;

static struct gpio ril_gpios_TF300TG[] = {
		{ MOD_VBUS_ON,    GPIOF_OUT_INIT_LOW,  "BB_VBUS"    },
		{ USB_SW_SEL,     GPIOF_OUT_INIT_LOW,  "BB_SW_SEL"  },
		{ SAR_DET_3G,     GPIOF_IN,            "BB_SAR_DET" },
		{ SIM_CARD_DET,   GPIOF_IN,            "BB_SIM_DET" },
};

static struct gpio ril_gpios_TF300TL[] = {
		{ MOD_VBAT_ON,    GPIOF_OUT_INIT_LOW,  "BB_VBAT"},
		{ MOD_VBUS_ON,    GPIOF_OUT_INIT_LOW,  "BB_VBUS"},
		{ USB_SW_SEL,     GPIOF_OUT_INIT_LOW,  "BB_SW_SEL"},
		{ SAR_DET_3G,     GPIOF_IN,            "BB_SAR_DET" },
		{ SIM_CARD_DET,   GPIOF_IN,            "BB_SIM_DET" },
		{ MOD_POWER_KEY,  GPIOF_OUT_INIT_LOW,  "BB_MOD_PWR"},
		{ DL_MODE,        GPIOF_OUT_INIT_LOW,  "BB_DL_MODE"},
		{ AP_TO_MOD_RST,  GPIOF_OUT_INIT_LOW,  "BB_MOD_RST"},
};

static int store_gpio(size_t count, const char *buf, int gpio, char *gpio_name)
{
	int enable;

	if (sscanf(buf, "%u", &enable) != 1)
		return -EINVAL;

	if ((enable != 1) && (enable != 0))
		return -EINVAL;

	gpio_set_value(gpio, enable);
	RIL_INFO("Set %s to %d\n", gpio_name, enable);
	return count;
}

/* Common sysfs functions */
static ssize_t show_prox_enabled(struct device *class,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", proximity_enabled);
}

static ssize_t store_prox_enabled(struct device *class, struct device_attribute *attr,
		const char *buf, size_t count)
{
	int enable;

	if (sscanf(buf, "%u", &enable) != 1)
		return -EINVAL;

	if ((enable != 1) && (enable != 0))
		return -EINVAL;

	RIL_INFO("enable: %d\n", enable);

	/* when enabled, report the current status immediately.
	   when disabled, set state to 0 to sync with RIL */
	mutex_lock(&prox_enable_mtx);
	if (enable != proximity_enabled) {
		if (enable) {
			enable_irq(gpio_to_irq(SAR_DET_3G));
			queue_work(workqueue, &prox_work);
		} else {
			disable_irq(gpio_to_irq(SAR_DET_3G));
			switch_set_state(&prox_sdev, 0);
		}
		proximity_enabled = enable;
	}
	mutex_unlock(&prox_enable_mtx);

	return strnlen(buf, count);
}
/* TF300TG sysfs functions */
static ssize_t show_vbus_state(struct device *class,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", gpio_get_value(MOD_VBUS_ON));
}

static ssize_t store_vbus_state(struct device *class, struct device_attribute *attr,
		const char *buf, size_t count)
{
	return store_gpio(count, buf, MOD_VBUS_ON, "MOD_VBUS_ON");
}

static ssize_t show_cdump_state(struct device *class,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", do_crash_dump);
}

static ssize_t store_cdump_state(struct device *class, struct device_attribute *attr,
		const char *buf, size_t count)
{
	int enable;

	if (sscanf(buf, "%u", &enable) != 1)
		return -EINVAL;

	if ((enable != 1) && (enable != 0))
		return -EINVAL;

	RIL_INFO("enable: %d\n", enable);

	if (enable)
		do_crash_dump = 1;
	else
		do_crash_dump = 0;

	return strnlen(buf, count);
}

/* TF300TL sysfs functions */
static ssize_t show_power_state(struct device *class, struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%d\n", gpio_get_value(MOD_POWER_KEY));
}

static ssize_t store_power_state(struct device *class, struct device_attribute *attr,
		const char *buf, size_t count)
{
	return store_gpio(count, buf, MOD_POWER_KEY, "MOD_POWER_KEY");
}

static ssize_t show_vbat_state(struct device *class, struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%d\n", gpio_get_value(MOD_VBAT_ON));
}

static ssize_t store_vbat_state(struct device *class, struct device_attribute *attr,
		const char *buf, size_t count)
{
	return store_gpio(count, buf, MOD_VBAT_ON, "MOD_VBAT_ON");
}

/* TF300TG sysfs array */
static struct device_attribute device_attr_TF300TG[] = {
		__ATTR(prox_onoff, _ATTR_MODE, show_prox_enabled, store_prox_enabled),
		__ATTR(bb_vbus, _ATTR_MODE, show_vbus_state, store_vbus_state),
		__ATTR(crash_dump_onoff, _ATTR_MODE, show_cdump_state, store_cdump_state),
		__ATTR_NULL,
};

/* TF300TL sysfs array */
static struct device_attribute device_attr_TF300TL[] = {
		__ATTR(mod_power, _ATTR_MODE, show_power_state, store_power_state),
		__ATTR(vbat, _ATTR_MODE, show_vbat_state, store_vbat_state),
		__ATTR(prox_onoff, _ATTR_MODE, show_prox_enabled, store_prox_enabled),
		__ATTR_NULL,
};

/* Suspend request switch functions */
static ssize_t crash_print_name(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", "crash_dump_det");
}

static ssize_t crash_print_state(struct switch_dev *sdev, char *buf)
{
	int state = -1;
	if (switch_get_state(sdev))
		state = 1;
	else
		state = 0;

	return sprintf(buf, "%d\n", state);
}

static int ril_crash_det_init(void)
{
	int rc = 0;
	/* register switch class */
	crash_sdev.name = "crash_dump_det";
	crash_sdev.print_name = crash_print_name;
	crash_sdev.print_state = crash_print_state;

	rc = switch_dev_register(&crash_sdev);

	if (rc < 0)
		goto failed;

	return 0;

failed:
	return rc;
}

static void ril_crash_det_exit(void)
{
	switch_dev_unregister(&crash_sdev);
}

/* Proximity switch functions */
static ssize_t prox_print_name(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", "prox_sar_det");
}

static ssize_t prox_print_state(struct switch_dev *sdev, char *buf)
{
	int state = -1;
	if (switch_get_state(sdev))
		state = 1;
	else
		state = 0;

	return sprintf(buf, "%d\n", state);
}

static int ril_proximity_init(void)
{
	int rc = 0;
	/* register switch class */
	prox_sdev.name = "prox_sar_det";
	prox_sdev.print_name = prox_print_name;
	prox_sdev.print_state = prox_print_state;

	rc = switch_dev_register(&prox_sdev);

	if (rc < 0)
		goto failed;

	return 0;

failed:
	return rc;
}

static void ril_proximity_exit(void)
{
	switch_dev_unregister(&prox_sdev);
}

static void ril_proximity_work(struct work_struct *work)
{
	int value;

	value = gpio_get_value(SAR_DET_3G);
	RIL_INFO("SAR_DET_3G: %d\n", value);

	if (!value)
		switch_set_state(&prox_sdev, 1);
	else
		switch_set_state(&prox_sdev, 0);
}

static void ril_crash_dump_work(struct work_struct *work)
{
	disable_irq(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ));

	baseband_modem_crash_dump(0);
	msleep(200);
	gpio_set_value(USB_SW_SEL, 1);
	mdelay(5);
	gpio_set_value(MOD_VBUS_ON, 1);
	mdelay(5);
	baseband_modem_crash_dump(1);

	switch_set_state(&crash_sdev, 1);
}

/* IRQ Handlers */
irqreturn_t ril_ipc_sus_req_irq(int irq, void *dev_id)
{
	int value;

	if (do_crash_dump) {
		value = gpio_get_value(XMM_GPIO_IPC_HSIC_SUS_REQ);
		if (value) {
			RIL_INFO("do_crash_dump is on!\n");
			queue_work(workqueue, &crash_work);
		}
	}

	return IRQ_HANDLED;
}

irqreturn_t ril_ipc_sar_det_irq(int irq, void *dev_id)
{
	queue_work(workqueue, &prox_work);

	return IRQ_HANDLED;
}

irqreturn_t ril_ipc_sim_det_irq(int irq, void *dev_id)
{
	return sim_interrupt_handle(irq, dev_id);
}

static int create_ril_files(void)
{
	int rc = 0, i = 0;

	rc = alloc_chrdev_region(&ril_dev, ril_minor, 1, "ril");
	ril_major = MAJOR(ril_dev);
	if (rc < 0) {
		RIL_ERR("allocate char device failed\n");
		goto failed;
	}
	RIL_INFO("rc = %d, ril_major = %d\n", rc, ril_major);

	ril_class = class_create(THIS_MODULE, "ril");
	if (IS_ERR(ril_class)) {
		RIL_ERR("ril_class create fail\n");
		rc = PTR_ERR(ril_class);
		goto create_class_failed;
	}

	dev = device_create(ril_class, NULL, MKDEV(ril_major, ril_minor),
			NULL, "files");
	if (IS_ERR(dev)) {
		RIL_ERR("dev create fail\n");
		rc = PTR_ERR(dev);
		goto create_device_failed;
	}

	if (project_id == TEGRA3_PROJECT_TF300TG) {
		for (i = 0; i < (ARRAY_SIZE(device_attr_TF300TG) - 1); i++) {
			rc = device_create_file(dev, &device_attr_TF300TG[i]);
			if (rc < 0) {
				RIL_ERR("create file of [%d] failed, err = %d\n", i, rc);
				goto create_files_failed;
			}
		}
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		for (i = 0; i < (ARRAY_SIZE(device_attr_TF300TL) - 1); i++) {
			rc = device_create_file(dev, &device_attr_TF300TL[i]);
			if (rc < 0) {
				RIL_ERR("create file of [%d] failed, err = %d\n", i, rc);
				goto create_files_failed;
			}
		}
	}

	RIL_INFO("add_ril_dev success\n");
	return 0;

create_files_failed:
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		while (i--)
			device_remove_file(dev, &device_attr_TF300TG[i]);
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		while (i--)
			device_remove_file(dev, &device_attr_TF300TL[i]);
	}
create_device_failed:
	class_destroy(ril_class);
create_class_failed:
	unregister_chrdev_region(ril_dev, 1);
failed:
	return rc;
}

static void remove_ril_files(void)
{
	int i;
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		for (i = 0; i < (ARRAY_SIZE(device_attr_TF300TG) - 1); i++)
			device_remove_file(dev, &device_attr_TF300TG[i]);
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		for (i = 0; i < (ARRAY_SIZE(device_attr_TF300TL) - 1); i++)
			device_remove_file(dev, &device_attr_TF300TL[i]);
	}
	device_destroy(ril_class, MKDEV(ril_major, ril_minor));
	class_destroy(ril_class);
	unregister_chrdev_region(ril_dev, 1);
}

static void power_on_TF300TL(void)
{
	RIL_INFO("TF300TL power_on\n");
	gpio_set_value(MOD_VBAT_ON, 1);
	RIL_INFO("Set MOD_VBAT_ON to %d\n", gpio_get_value(MOD_VBAT_ON));
	mdelay(100);
	gpio_set_value(MOD_POWER_KEY, 1);
	RIL_INFO("Set MOD_POWER_KEY to %d\n", gpio_get_value(MOD_POWER_KEY));
	msleep(200);
	gpio_set_value(USB_SW_SEL, 1);
	RIL_INFO("Set USB_SW_SEL to %d\n", gpio_get_value(USB_SW_SEL));
	mdelay(50);
	gpio_set_value(MOD_VBUS_ON, 1);
	RIL_INFO("Set MOD_VBUS_ON to %d\n", gpio_get_value(MOD_VBUS_ON));
}

static int __init ril_init(void)
{
	int err, i;
	RIL_INFO("RIL init\n");

	project_id = tegra3_get_project_id();

	/* enable and request gpio(s) */
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		RIL_INFO("project_id = TF300TG\n");
		for (i = 0; i < ARRAY_SIZE(ril_gpios_TF300TG); i++) {
			tegra_gpio_enable(ril_gpios_TF300TG[i].gpio);
		}
		err = gpio_request_array(ril_gpios_TF300TG,
				ARRAY_SIZE(ril_gpios_TF300TG));
		if (err < 0) {
			pr_err("%s - request gpio(s) failed\n", __func__);
			return err;
		}
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		RIL_INFO("project_id = TF300TL\n");
		for (i = 0; i < ARRAY_SIZE(ril_gpios_TF300TL); i++) {
			tegra_gpio_enable(ril_gpios_TF300TL[i].gpio);
		}
		err = gpio_request_array(ril_gpios_TF300TL,
				ARRAY_SIZE(ril_gpios_TF300TL));
		if (err < 0) {
			pr_err("%s - request gpio(s) failed\n", __func__);
			return err;
		}
		mdelay(100);
		power_on_TF300TL();
	} else {
		RIL_ERR("Ril driver doesn't support this project\n");
		return -1;
	}

	/* create device file(s) */
	err = create_ril_files();
	if (err < 0)
		goto failed1;

	/* request ril irq(s) */
	err = request_irq(gpio_to_irq(SAR_DET_3G),
		ril_ipc_sar_det_irq,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
		"IPC_SAR_DET_3G",
		NULL);
	if (err < 0) {
		pr_err("%s - request irq IPC_SAR_DET_3G failed\n",
			__func__);
		goto failed2;
	}
	disable_irq(gpio_to_irq(SAR_DET_3G));
	proximity_enabled = 0;

	err = request_irq(gpio_to_irq(SIM_CARD_DET),
		ril_ipc_sim_det_irq,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
		"IPC_SIM_CARD_DET",
		NULL);
	if (err < 0) {
		pr_err("%s - request irq IPC_SIM_CARD_DET failed\n",
			__func__);
		goto failed3;
	}
	tegra_pm_irq_set_wake_type(gpio_to_irq(SIM_CARD_DET),
		IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING);
	enable_irq_wake(gpio_to_irq(SIM_CARD_DET));

	if (project_id == TEGRA3_PROJECT_TF300TG) {
		err = request_irq(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ),
			ril_ipc_sus_req_irq,
			IRQF_TRIGGER_RISING,
			"IPC_MOD_SUS_REQ",
			NULL);
		if (err < 0) {
			pr_err("%s - request irq IPC_MOD_SUS_REQ failed\n",
				__func__);
			goto failed4;
		}
		tegra_pm_irq_set_wake_type(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ),
			IRQF_TRIGGER_RISING);
		enable_irq_wake(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ));
	}

	/* register proximity switch */
	err = ril_proximity_init();
	if (err < 0) {
		pr_err("%s - register proximity switch failed\n",
			__func__);
		goto failed5;
	}

	if (project_id == TEGRA3_PROJECT_TF300TG) {
		/* register crash dump switch */
		err = ril_crash_det_init();
		if (err < 0) {
			pr_err("%s - register crash dump switch failed\n",
				__func__);
			goto failed6;
		}
	}

	/* init work queue */
	workqueue = create_singlethread_workqueue
		("ril_workqueue");
	if (!workqueue) {
		pr_err("%s - cannot create workqueue\n", __func__);
		err = -1;
		goto failed7;
	}

	/* init work objects */
	INIT_WORK(&prox_work, ril_proximity_work);
	INIT_WORK(&crash_work, ril_crash_dump_work);

	/* init SIM plug functions */
	err = init_sim_hot_plug(dev, workqueue);
	if (err < 0) {
		pr_err("%s - init SIM hotplug failed\n",
			__func__);
		goto failed8;
	}

	RIL_INFO("RIL init successfully\n");
	return 0;


failed8:
	destroy_workqueue(workqueue);
failed7:
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		ril_crash_det_exit();
	}
failed6:
	ril_proximity_exit();
failed5:
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		free_irq(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ), NULL);
	}
failed4:
	free_irq(gpio_to_irq(SIM_CARD_DET), NULL);
failed3:
	free_irq(gpio_to_irq(SAR_DET_3G), NULL);
failed2:
	remove_ril_files();
failed1:
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		gpio_free_array(ril_gpios_TF300TG,
				ARRAY_SIZE(ril_gpios_TF300TG));
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		gpio_free_array(ril_gpios_TF300TL,
				ARRAY_SIZE(ril_gpios_TF300TL));
	}
	return err;
}

static void __exit ril_exit(void)
{
	RIL_INFO("RIL exit\n");

	/* free irq(s) */
	free_irq(gpio_to_irq(SAR_DET_3G), NULL);
	free_irq(gpio_to_irq(SIM_CARD_DET), NULL);
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		free_irq(gpio_to_irq(XMM_GPIO_IPC_HSIC_SUS_REQ), NULL);
	}

	/* free gpio(s) */
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		gpio_free_array(ril_gpios_TF300TG,
				ARRAY_SIZE(ril_gpios_TF300TG));
	} else if (project_id == TEGRA3_PROJECT_TF300TL) {
		gpio_free_array(ril_gpios_TF300TL,
				ARRAY_SIZE(ril_gpios_TF300TL));
	}

	/* delete device file(s) */
	remove_ril_files();

	/* unregister switches */
	ril_proximity_exit();
	if (project_id == TEGRA3_PROJECT_TF300TG) {
		ril_crash_det_exit();
	}

	/* destroy workqueue */
	destroy_workqueue(workqueue);

	/* free resources for SIM hot plug */
	free_sim_hot_plug();
}

module_init(ril_init);
module_exit(ril_exit);
