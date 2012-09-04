/*
 * arch/arm/mach-tegra/board-cardhu-misc.c
 *
 * Copyright (C) 2011-2012 ASUSTek Computer Incorporation
 * Author: Paris Yeh <paris_yeh@asus.com>
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
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

#include <mach/board-cardhu-misc.h>
#include "gpio-names.h"
#include "fuse.h"

#define CARDHU_MISC_ATTR(module) \
static struct kobj_attribute module##_attr = { \
	.attr = { \
		.name = __stringify(module), \
		.mode = 0444, \
	}, \
	.show = module##_show, \
}

//Chip unique ID is a maximum of 17 characters including NULL termination.
unsigned char cardhu_chipid[17];
EXPORT_SYMBOL(cardhu_chipid);

//PCBID is composed of nine GPIO pins predefined by HW schematic of Tegra3-series
unsigned int cardhu_pcbid;

static const char *tegra3_project_name[TEGRA3_PROJECT_MAX] = {
	[TEGRA3_PROJECT_TF201] = "TF201",
	[TEGRA3_PROJECT_ME370T] = "ME370T",
	[TEGRA3_PROJECT_TF300T] = "TF300T",
	[TEGRA3_PROJECT_TF300TG] = "TF300TG",
	[TEGRA3_PROJECT_TF700T] = "TF700T",
	[TEGRA3_PROJECT_TF300TL] = "TF300TL",
	[TEGRA3_PROJECT_ReserveB] = "unknown",
	[TEGRA3_PROJECT_TF500T] = "TF500T",
};

static unsigned int tegra3_project_name_index = TEGRA3_PROJECT_INVALID;
static bool tegra3_misc_enabled = false;

static int __init tegra3_productid_setup(char *id)
{
	unsigned int index;

	if (!id) return 0;

	index = (unsigned int) simple_strtoul(id, NULL, 0);

	tegra3_project_name_index = (index < TEGRA3_PROJECT_MAX)
					? index : TEGRA3_PROJECT_INVALID;
	return 1;
}

__setup("androidboot.productid=", tegra3_productid_setup);

/* Deprecated function */
const char *tegra3_get_project_name(void)
{
	unsigned int project_id = tegra3_project_name_index;

	if (tegra3_misc_enabled) {
		project_id = HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW,
						PROJECT, cardhu_pcbid);

		/* WARN if project id was not matched with PCBID */
		WARN_ONCE(project_id != tegra3_project_name_index,
			"[MISC]: project ID in kernel cmdline was not matched with PCBID\n");
	}
	else {
		pr_info("[MISC]: adopt kernel cmdline prior to %s ready.\n",
				__func__);
	}

	return (project_id < TEGRA3_PROJECT_MAX) ?
		tegra3_project_name[project_id] : "unknown";
}
EXPORT_SYMBOL(tegra3_get_project_name);

unsigned int tegra3_get_project_id(void)
{
	unsigned int project_id = tegra3_project_name_index;

	if (tegra3_misc_enabled) {
		project_id = HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW,
						PROJECT, cardhu_pcbid);

		/* WARN if project id was not matched with PCBID */
		WARN_ONCE(project_id != tegra3_project_name_index,
			"[MISC]: project ID in kernel cmdline was not matched with PCBID\n");
	}
	else {
		pr_info("[MISC]: adopt kernel cmdline prior to %s ready.\n",
				__func__);
	}

	return (project_id < TEGRA3_PROJECT_MAX) ? project_id : TEGRA3_PROJECT_INVALID;
}
EXPORT_SYMBOL(tegra3_get_project_id);

unsigned int tegra3_query_touch_module_pcbid(void)
{
	unsigned int touch_pcbid = 0;
	unsigned int project = tegra3_get_project_id();
	unsigned int ret = -1;

	/* Check if running target platform */
	if ((project == TEGRA3_PROJECT_TF300T) ||
		(project == TEGRA3_PROJECT_TF300TG) ||
		(project == TEGRA3_PROJECT_TF300TL)) {
		pr_err("[MISC]: %s is not supported on %s.\n", __func__,
			tegra3_get_project_name());
		return ret;
	}

	/* Fetch PCB_ID[2] and PCB_ID[6] and recompose it */
	touch_pcbid = (HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW, TOUCHL, cardhu_pcbid)) +
		(HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW, TOUCHH, cardhu_pcbid) << 1);

	switch (project) {
	case TEGRA3_PROJECT_TF201:
		ret = touch_pcbid;
		break;
	case TEGRA3_PROJECT_TF700T:
		/* Reserve PCB_ID[2] for touch panel identification */
		ret = touch_pcbid & 0x1;
		break;
	default:
		ret = -1;
	}

	return ret;
}
EXPORT_SYMBOL(tegra3_query_touch_module_pcbid);

unsigned int tegra3_query_audio_codec_pcbid(void)
{
	unsigned int codec_pcbid = 0;
	unsigned int project = tegra3_get_project_id();
	unsigned int ret = -1;

	/* Check if running target platform */
	if ((project == TEGRA3_PROJECT_TF201) ||
		(project == TEGRA3_PROJECT_TF700T)) {
		pr_err("[MISC]: %s is not supported on %s.\n", __func__,
			tegra3_get_project_name());
		return ret;
	}

	codec_pcbid = HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW, ACODEC, cardhu_pcbid);

	if ((project == TEGRA3_PROJECT_TF300T) ||
		(project == TEGRA3_PROJECT_TF300TG) ||
		(project == TEGRA3_PROJECT_TF300TL)) {
		ret = codec_pcbid;
	}

	return ret;
}
EXPORT_SYMBOL(tegra3_query_audio_codec_pcbid);

unsigned int tegra3_query_pcba_revision_pcbid(void)
{
	unsigned int project = tegra3_get_project_id();
	unsigned int ret = -1;

	/* Check if running target platform */
	if (project != TEGRA3_PROJECT_TF700T) {
		pr_err("[MISC]: %s is not supported on %s.\n", __func__,
			tegra3_get_project_name());
		return ret;
	}

	ret = HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW, REVISION, cardhu_pcbid);

	return ret;
}
EXPORT_SYMBOL(tegra3_query_pcba_revision_pcbid);

unsigned int tegra3_query_wifi_module_pcbid(void)
{
	unsigned int wifi_pcbid = 0;
	unsigned int project = tegra3_get_project_id();
	unsigned int ret = -1;

	/* Check if running target platform is valid */
	if (project == TEGRA3_PROJECT_INVALID) {
		pr_err("[MISC]: %s is not supported on %s.\n", __func__,
			tegra3_get_project_name());
		return ret;
	}

	wifi_pcbid = HW_DRF_VAL(TEGRA3_DEVKIT, MISC_HW, WIFI, cardhu_pcbid);
	ret = wifi_pcbid;

	return ret;
}
EXPORT_SYMBOL(tegra3_query_wifi_module_pcbid);

static ssize_t cardhu_chipid_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	char *s = buf;

	s += sprintf(s, "%s\n", cardhu_chipid);
	return (s - buf);
}

#define FUSE_FAB_CODE		0x204
#define FUSE_FAB_CODE_MASK	0x3f
#define FUSE_LOT_CODE_1		0x20c
static ssize_t cardhu_backup_chipid_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	unsigned long reg;
	unsigned long fab;
	unsigned long lot;
	unsigned long i;
	unsigned long backup_uid;
	char backup_id[9];

	/* fab code */
	fab = tegra_fuse_readl(FUSE_FAB_CODE) & FUSE_FAB_CODE_MASK;

	/* Lot code must be re-encoded from a 5 digit base-36 'BCD' number
	 * to a binary number.
	 */
	lot = 0;
	reg = tegra_fuse_readl(FUSE_LOT_CODE_1) << 2;

	for (i = 0; i < 5; ++i) {
		u32 digit = (reg & 0xFC000000) >> 26;
		BUG_ON(digit >= 36);
		lot *= 36;
		lot += digit;
		reg <<= 6;
	}

	backup_uid = 0;
	/* compose 32-bit backup id by concatenating two bit fields.
	 *   <FAB:6><LOT:26>
	 */
	backup_uid = ((unsigned long) fab << 26ul) | ((unsigned long) lot);
	snprintf(backup_id, sizeof(backup_id), "%08lx", backup_uid);

	strcpy(buf, cardhu_chipid);

	/* replace 64-bit unique id starting bit#24 with 32-bit backup id */
	for (i = 0; i < strlen(backup_id); i++)
		buf[i + 2] = backup_id[i];

	s += sprintf(s, "%s\n", buf);
	return (s - buf);
}

static ssize_t cardhu_pcbid_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int i;

	for (i = TEGRA3_DEVKIT_MISC_PCBID_NUM; i > 0; i--) {
		s += sprintf(s, "%c", cardhu_pcbid & (1 << (i - 1)) ? '1' : '0');
	}
	s += sprintf(s, "b\n");
	return (s - buf);
}

static ssize_t cardhu_projectid_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
        char *s = buf;

        s += sprintf(s, "%02x\n", tegra3_get_project_id());
        return (s - buf);
}

static ssize_t cardhu_projectname_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
        char *s = buf;

        s += sprintf(s, "%s\n", tegra3_get_project_name());
        return (s - buf);
}

static DEFINE_MUTEX(vibrator_lock);

static ssize_t cardhu_vibctl_store(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf, size_t count)
{
	int ret = count;
	mutex_lock(&vibrator_lock);

	// ignore case
	if (!strncasecmp(buf, "ON", strlen("ON"))) {
		pr_info("[MISC]: turn on vibrator.\n");
		gpio_set_value(TEGRA_GPIO_PH7, 1);
	} else if (!strncasecmp(buf, "OFF", strlen("OFF"))) {
		pr_info("[MISC]: turn off vibrator.\n");
		gpio_set_value(TEGRA_GPIO_PH7, 0);
	} else {
		pr_err("[MISC]: undefined for vibrator control.\n");
		ret = 0;
	}

	mutex_unlock(&vibrator_lock);
	return ret;
}

CARDHU_MISC_ATTR(cardhu_chipid);
CARDHU_MISC_ATTR(cardhu_backup_chipid);
CARDHU_MISC_ATTR(cardhu_pcbid);
CARDHU_MISC_ATTR(cardhu_projectid);
CARDHU_MISC_ATTR(cardhu_projectname);

static struct kobj_attribute cardhu_vibctl_attr = {
        .attr = {
                .name = __stringify(cardhu_vibctl),
                .mode = 0220,
        },
        .store = cardhu_vibctl_store,
};

static struct attribute *attr_list[] = {
	&cardhu_chipid_attr.attr,
	&cardhu_backup_chipid_attr.attr,
	&cardhu_pcbid_attr.attr,
	&cardhu_projectid_attr.attr,
	&cardhu_projectname_attr.attr,
	&cardhu_vibctl_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attr_list,
};

static struct platform_device *cardhu_misc_device;

static int pcbid_init(void)
{
	int ret;

	ret = gpio_request(TEGRA_GPIO_PR4, "PCB_ID0");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PR5, "PCB_ID1");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PQ4, "PCB_ID2");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PQ7, "PCB_ID3");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PR2, "PCB_ID4");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		gpio_free(TEGRA_GPIO_PR2);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PQ5, "PCB_ID5");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		gpio_free(TEGRA_GPIO_PR2);
		gpio_free(TEGRA_GPIO_PQ5);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PJ0, "PCB_ID6");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		gpio_free(TEGRA_GPIO_PR2);
		gpio_free(TEGRA_GPIO_PQ5);
		gpio_free(TEGRA_GPIO_PJ0);
		return ret;
	}

	ret = gpio_request(TEGRA_GPIO_PJ2, "PCB_ID7");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		gpio_free(TEGRA_GPIO_PR2);
		gpio_free(TEGRA_GPIO_PQ5);
		gpio_free(TEGRA_GPIO_PJ0);
		gpio_free(TEGRA_GPIO_PJ2);
		return ret;
        }

	ret = gpio_request(TEGRA_GPIO_PK3, "PCB_ID8");
	if (ret) {
		gpio_free(TEGRA_GPIO_PR4);
		gpio_free(TEGRA_GPIO_PR5);
		gpio_free(TEGRA_GPIO_PQ4);
		gpio_free(TEGRA_GPIO_PQ7);
		gpio_free(TEGRA_GPIO_PR2);
		gpio_free(TEGRA_GPIO_PQ5);
		gpio_free(TEGRA_GPIO_PJ0);
		gpio_free(TEGRA_GPIO_PJ2);
		gpio_free(TEGRA_GPIO_PK3);
		return ret;
        }

	tegra_gpio_enable(TEGRA_GPIO_PR4);
	tegra_gpio_enable(TEGRA_GPIO_PR5);
	tegra_gpio_enable(TEGRA_GPIO_PQ4);
	tegra_gpio_enable(TEGRA_GPIO_PQ7);
	tegra_gpio_enable(TEGRA_GPIO_PR2);
	tegra_gpio_enable(TEGRA_GPIO_PQ5);
	tegra_gpio_enable(TEGRA_GPIO_PJ0);
	tegra_gpio_enable(TEGRA_GPIO_PJ2);
	tegra_gpio_enable(TEGRA_GPIO_PK3);

	gpio_direction_input(TEGRA_GPIO_PR4);
	gpio_direction_input(TEGRA_GPIO_PR5);
	gpio_direction_input(TEGRA_GPIO_PQ4);
	gpio_direction_input(TEGRA_GPIO_PQ7);
	gpio_direction_input(TEGRA_GPIO_PR2);
	gpio_direction_input(TEGRA_GPIO_PQ5);
	gpio_direction_input(TEGRA_GPIO_PJ0);
	gpio_direction_input(TEGRA_GPIO_PJ2);
	gpio_direction_input(TEGRA_GPIO_PK3);

	cardhu_pcbid = (gpio_get_value(TEGRA_GPIO_PK3) << 8) |
			(gpio_get_value(TEGRA_GPIO_PJ2) << 7) |
			(gpio_get_value(TEGRA_GPIO_PJ0) << 6) |
			(gpio_get_value(TEGRA_GPIO_PQ5) << 5) |
			(gpio_get_value(TEGRA_GPIO_PR2) << 4) |
			(gpio_get_value(TEGRA_GPIO_PQ7) << 3) |
			(gpio_get_value(TEGRA_GPIO_PQ4) << 2) |
			(gpio_get_value(TEGRA_GPIO_PR5) << 1) |
			gpio_get_value(TEGRA_GPIO_PR4);
	return 0;
}

int __init cardhu_misc_init(void)
{
	int ret = 0;

	pr_debug("%s: start\n", __func__);

	// create a platform device
	cardhu_misc_device = platform_device_alloc("cardhu_misc", -1);

        if (!cardhu_misc_device) {
		ret = -ENOMEM;
		goto fail_platform_device;
        }

	// add a platform device to device hierarchy
	ret = platform_device_add(cardhu_misc_device);
	if (ret) {
		pr_err("[MISC]: cannot add device to platform.\n");
		goto fail_platform_add_device;
	}

	ret = sysfs_create_group(&cardhu_misc_device->dev.kobj, &attr_group);
	if (ret) {
		pr_err("[MISC]: cannot create sysfs group.\n");
		goto fail_sysfs;
	}

	// acquire pcb_id info
	ret = pcbid_init();
	if (ret) {
		pr_err("[MISC]: cannot acquire PCB_ID info.\n");
		goto fail_sysfs;
	}

	// indicate misc module well-prepared
	tegra3_misc_enabled = true;

	return ret;

fail_sysfs:
	platform_device_del(cardhu_misc_device);

fail_platform_add_device:
	platform_device_put(cardhu_misc_device);

fail_platform_device:
	return ret;
}
