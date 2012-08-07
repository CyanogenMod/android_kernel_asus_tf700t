/*
 * linux/drivers/video/backlight/pwm_bl.c
 *
 * simple PWM based backlight control, board code has to setup
 * 1) pin configuration so PWM waveforms can output
 * 2) platform_data being correctly configured
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <linux/gpio.h>
#include "../gpio-names.h"
#include "../tegra/dc/dc_priv.h"
#include <linux/i2c.h>
#include <mach/board-cardhu-misc.h>


/* For MIPI bridge IC */
#define DISPLAY_TABLE_END        1 /* special number to indicate this is end of table */
#define DISPLAY_MAX_RETRIES   3 /* max counter for retry I2C access */
#define DISPLAY_WAIT_MS          0 /* special number to indicate this is wait time require */
static int client_count = 0;
static struct i2c_client        *client_panel;
static struct display_reg {
        u16 addr;
        u16 val;
};

int I2C_command_flag = 1;


#define cardhu_bl_enb			TEGRA_GPIO_PH2

static atomic_t sd_brightness = ATOMIC_INIT(255);
extern struct tegra_dc *tegra_dcs[TEGRA_MAX_DC];

struct pwm_bl_data {
	struct pwm_device	*pwm;
	struct device		*dev;
	unsigned int		period;
	unsigned int		lth_brightness;
	int			(*notify)(struct device *,
					  int brightness);
	int			(*check_fb)(struct device *, struct fb_info *);
};

static int display_write_reg(struct i2c_client *client, u16 addr, u16 val)
{
        int err;
        struct i2c_msg msg;
        unsigned char data[4];
        int retry = 0;

        if (!client->adapter)
                return -ENODEV;

        data[0] = (u8) (addr >> 8);
        data[1] = (u8) (addr & 0xff);
        data[2] = (u8) (val >> 8);
        data[3] = (u8) (val & 0xff);

        msg.addr = client->addr;
        msg.flags = 0;
        msg.len = 4;
        msg.buf = data;
        do {
                err = i2c_transfer(client->adapter, &msg, 1);
                if (err == 1)
                        return 0;
                retry++;
                pr_err("display bridge IC : i2c transfer failed, retrying %x %x\n",
                               addr, val);
                pr_err("display bridge IC : i2c transfer failed, count %x \n",
                               msg.addr);
                //      msleep(3);
        } while (retry <= DISPLAY_MAX_RETRIES);

        return err;
}

static int display_write_table(struct i2c_client *client,
                                                                  const struct display_reg table[])
{
               int err;
               const struct display_reg *next;
               u16 val;
               int command_count=0;

               pr_info("DISPLAY_write_table %s\n",__func__);
               for (next = table; next->addr != DISPLAY_TABLE_END; next++) {
                               if (next->addr == DISPLAY_WAIT_MS) {
                                               msleep(next->val);
                                               continue;
                               }

                               val = next->val;

                               err = display_write_reg(client, next->addr, val);
                               if (err)
                                               return err;
               }
               return 0;
}

static int display_read_reg(struct i2c_client *client, u16 addr, u16 *val)
{
               int err;
               struct i2c_msg msg[2];
               unsigned char data[4];

               if (!client->adapter)
                               return -ENODEV;

               msg[0].addr = 0x07;
               msg[0].flags = 0;
               msg[0].len = 2;
               msg[0].buf = data;

               /* high byte goes out first */
               data[0] = (u8) (addr >> 8);
               data[1] = (u8) (addr & 0xff);

               msg[1].addr = 0x07;
               msg[1].flags = 1;

               msg[1].len = 2;
               msg[1].buf = data + 2;

               err = i2c_transfer(client->adapter, msg, 2);

               printk("Check register high=%x \n",data[2]);
               printk("Check register low=%x \n",data[3]);

               if (err != 2)
                               return -EINVAL;
                               /*
                                               memcpy(val, data+2, 1);
                                               *val=*val&0xff;
                               */
               return 0;
}

static int display_read_table(struct i2c_client *client,
                                   const struct display_reg table[])
{
        int err;
        const struct display_reg *next;
        u16 val;
        int command_count=0;

        pr_info("DISPLAY_read_table %s\n",__func__);
        for (next = table; next->addr != DISPLAY_TABLE_END; next++) {
                if (next->addr == DISPLAY_WAIT_MS) {
                        msleep(next->val);
                        continue;
                }

                val = 0;

                err = display_read_reg(client, next->addr, val);
                if (err)
                        return err;
        }
        return 0;
}


static int pwm_backlight_update_status(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);
	int brightness = bl->props.brightness;
	int max = bl->props.max_brightness;
	static int bl_enable_sleep_control = 0;	// sleep only when suspend or resume

	if (bl->props.power != FB_BLANK_UNBLANK)
		brightness = 0;

	if (bl->props.fb_blank != FB_BLANK_UNBLANK)
		brightness = 0;

	if (brightness == 0) {
		if (pb->notify)
			brightness = pb->notify(pb->dev, brightness);
		if(bl_enable_sleep_control)
		{
			msleep(5);
			bl_enable_sleep_control = 0;
		}
		pwm_config(pb->pwm, 0, pb->period);
		pwm_disable(pb->pwm);
		I2C_command_flag=0;
	} else {
		if(tegra_dcs[0])
			sd_brightness = *(tegra_dcs[0]->out->sd_settings->sd_brightness);
		int cur_sd_brightness = atomic_read(&sd_brightness);
		brightness = (brightness * cur_sd_brightness) / 255;

		if (tegra3_get_project_id()==0x4 ){
			int err;
			int ret;
			int bus = 0;
			struct i2c_msg msg[2];
			unsigned char data[4]={0,0,0,0};
			struct i2c_board_info	*info;
			struct i2c_adapter				*adapter;
			struct display_reg display_table[71] =
			{
				{0x0002, 0x0001}, //SYSctl, S/W Reset
				{DISPLAY_WAIT_MS, 0x05},
				{0x0002, 0x0000}, //SYSctl, S/W Reset release
				//{0x0010, 0xFFFD}, //GPIO1
				//{0x0014, 0x0002},
				{0x0016, 0x309F}, //PLL Control Register 0 (PLL_PRD,PLL_FBD)
				{0x0018, 0x0203}, //PLL_FRS,PLL_LBWS, PLL oscillation enable
				{DISPLAY_WAIT_MS, 0x05},
				{0x0018, 0x0213}, //PLL_FRS,PLL_LBWS, PLL clock out enable
				{0x0006, 0x012C}, //FIFO Control Register
				{0x0008, 0x0037}, //DSI-TX Format setting
				{0x0050, 0x003E}, //DSI-TX Pixel stream packet Data Type setting
				{0x0140, 0x0000}, //D-PHY Clock lane enable
				{0x0142, 0x0000},
				{0x0144, 0x0000}, //D-PHY Data lane0 enable
				{0x0146, 0x0000},
				{0x0148, 0x0000}, //D-PHY Data lane1 enable
				{0x014A, 0x0000},
				{0x014C, 0x0000}, //D-PHY Data lane2 enable
				{0x014E, 0x0000},
				{0x0150, 0x0000}, //D-PHY Data lane3 enable
				{0x0152, 0x0000},
				{0x0100, 0x0203},
				{0x0102, 0x0000},
				{0x0104, 0x0203},
				{0x0106, 0x0000},
				{0x0108, 0x0203},
				{0x010A, 0x0000},
				{0x010C, 0x0203},
				{0x010E, 0x0000},
				{0x0110, 0x0203},
				{0x0112, 0x0000},
				{0x0210, 0x1964}, //LINEINITCNT
				{0x0212, 0x0000},
				{0x0214, 0x0005}, //LPTXTIMECNT
				{0x0216, 0x0000},
				{0x0218, 0x2801}, //TCLK_HEADERCNT
				{0x021A, 0x0000},
				{0x021C, 0x0000}, //TCLK_TRAILCNT
				{0x021E, 0x0000},
				{0x0220, 0x0C06}, //THS_HEADERCNT
				{0x0222, 0x0000},
				{0x0224, 0x4E20}, //TWAKEUPCNT
				{0x0226, 0x0000},
				{0x0228, 0x000B}, //TCLK_POSTCNT
				{0x022A, 0x0000},
				{0x022C, 0x0005}, //THS_TRAILCNT
				{0x022E, 0x0000},
				{0x0230, 0x0005}, //HSTXVREGCNT
				{0x0232, 0x0000},
				{0x0234, 0x001F}, //HSTXVREGEN enable
				{0x0236, 0x0000},
				{0x0238, 0x0001}, //DSI clock Enable/Disable during LP
				{0x023A, 0x0000},
				{0x023C, 0x0005}, //BTACNTRL1
				{0x023E, 0x0005}, //Lucien something wrong
				{0x0204, 0x0001}, //STARTCNTRL
				{0x0206, 0x0000},
				{0x0620, 0x0001}, //Sync Pulse/Sync Event mode setting
				{0x0622, 0x0020}, //V Control Register1
				{0x0624, 0x001A}, //V Control Register2
				{0x0626, 0x04B0}, //V Control Register3
				{0x0628, 0x015E}, //H Control Register1
				{0x062A, 0x00FA}, //H Control Register2
				{0x062C, 0x1680}, //H Control Register3
				{0x0518, 0x0001}, //DSI Start
				{0x051A, 0x0000},
				{0x0500, 0x0086}, //DSI lane setting, DSI mode=HS
				{0x0502, 0xA300}, //bit set
				{0x0500, 0x8000}, //Switch to DSI mode
				{0x0502, 0xC300},
				{0x0004, 0x0044}, //Configuration Control Register
				{DISPLAY_TABLE_END, 0x0000}
			};

			if (client_count==0) {
				printk("Check create a new adapter \n");
				info = kzalloc(sizeof(struct i2c_board_info), GFP_KERNEL);
				info->addr = 0x07;
				adapter = i2c_get_adapter(bus);
				if (!adapter) {
					printk("can't get adpater for bus %d\n", bus);
					err = -EBUSY;
					kfree(info);
				}

				client_panel = i2c_new_device(adapter, info);
				i2c_put_adapter(adapter);
				client_count++;
			}

			if (I2C_command_flag==0){
				I2C_command_flag=1;
				msg[0].addr = 0x07;
				msg[0].flags = 0;
				msg[0].len = 2;
				msg[0].buf = data;

				// high byte goes out first
				data[0] = 0;
				data[1] = 0;

				msg[1].addr = 0x07;
				msg[1].flags = 1;

				msg[1].len = 2;
				msg[1].buf = data + 2;

				err = i2c_transfer(client_panel->adapter, msg, 2);
				printk("Check Chip ID=%x \n",data[2]);
				printk("Check RevID=%x \n",data[3]);

				err = display_write_table(client_panel, display_table);
				printk("========================\n");
				printk("========================\n");
				printk("Check start to read register\n");
				//err = display_read_table(client_panel, display_table);
				//mdelay(1000);

				if (gpio_get_value(TEGRA_GPIO_PI6)==0){	//panel is panasonic
					printk("Panel is panasonic");
					mdelay(35);
				}
				else{								//panel is hydis
					printk("Panel is hydis");
					mdelay(70);
				}
			}
		}

		brightness = pb->lth_brightness +
			(brightness * (pb->period - pb->lth_brightness) / max);

		pwm_config(pb->pwm, brightness, pb->period);
		pwm_enable(pb->pwm);
		if(!bl_enable_sleep_control)
		{
			msleep(10);
			bl_enable_sleep_control = 1;
		}

		gpio_set_value(cardhu_bl_enb, !!brightness);
	}

	return 0;
}

static int pwm_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static int pwm_backlight_check_fb(struct backlight_device *bl,
				  struct fb_info *info)
{
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	return !pb->check_fb || pb->check_fb(pb->dev, info);
}

static const struct backlight_ops pwm_backlight_ops = {
	.update_status	= pwm_backlight_update_status,
	.get_brightness	= pwm_backlight_get_brightness,
	.check_fb	= pwm_backlight_check_fb,
};

static int pwm_backlight_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct platform_pwm_backlight_data *data = pdev->dev.platform_data;
	struct backlight_device *bl;
	struct pwm_bl_data *pb;
	int ret;

	if (!data) {
		dev_err(&pdev->dev, "failed to find platform data\n");
		return -EINVAL;
	}

	if (data->init) {
		ret = data->init(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	pb = kzalloc(sizeof(*pb), GFP_KERNEL);
	if (!pb) {
		dev_err(&pdev->dev, "no memory for state\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	pb->period = data->pwm_period_ns;
	pb->notify = data->notify;
	pb->check_fb = data->check_fb;
	pb->lth_brightness = data->lth_brightness *
		(data->pwm_period_ns / data->max_brightness);
	pb->dev = &pdev->dev;

	pb->pwm = pwm_request(data->pwm_id, "backlight");
	if (IS_ERR(pb->pwm)) {
		dev_err(&pdev->dev, "unable to request PWM for backlight\n");
		ret = PTR_ERR(pb->pwm);
		goto err_pwm;
	} else
		dev_dbg(&pdev->dev, "got pwm for backlight\n");

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness;
	bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
				       &pwm_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto err_bl;
	}

	bl->props.brightness = data->dft_brightness;
	backlight_update_status(bl);

	platform_set_drvdata(pdev, bl);
	return 0;

err_bl:
	pwm_free(pb->pwm);
err_pwm:
	kfree(pb);
err_alloc:
	if (data->exit)
		data->exit(&pdev->dev);
	return ret;
}

static int pwm_backlight_remove(struct platform_device *pdev)
{
	struct platform_pwm_backlight_data *data = pdev->dev.platform_data;
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	backlight_device_unregister(bl);
	pwm_config(pb->pwm, 0, pb->period);
	pwm_disable(pb->pwm);
	pwm_free(pb->pwm);
	kfree(pb);
	if (data->exit)
		data->exit(&pdev->dev);
	return 0;
}

#ifdef CONFIG_PM
static int pwm_backlight_suspend(struct platform_device *pdev,
				 pm_message_t state)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = dev_get_drvdata(&bl->dev);

	if (pb->notify)
		pb->notify(pb->dev, 0);
	pwm_config(pb->pwm, 0, pb->period);
	pwm_disable(pb->pwm);
	return 0;
}

static int pwm_backlight_resume(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);

	backlight_update_status(bl);
	return 0;
}
#else
#define pwm_backlight_suspend	NULL
#define pwm_backlight_resume	NULL
#endif

static struct platform_driver pwm_backlight_driver = {
	.driver		= {
		.name	= "pwm-backlight",
		.owner	= THIS_MODULE,
	},
	.probe		= pwm_backlight_probe,
	.remove		= pwm_backlight_remove,
	.suspend	= pwm_backlight_suspend,
	.resume		= pwm_backlight_resume,
};

static int __init pwm_backlight_init(void)
{
	return platform_driver_register(&pwm_backlight_driver);
}
module_init(pwm_backlight_init);

static void __exit pwm_backlight_exit(void)
{
	platform_driver_unregister(&pwm_backlight_driver);
}
module_exit(pwm_backlight_exit);

MODULE_DESCRIPTION("PWM based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-backlight");

