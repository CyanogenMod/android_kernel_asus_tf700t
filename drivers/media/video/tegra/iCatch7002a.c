/*
 * kernel/drivers/media/video/tegra
 *
 * iCatch SPCA7002A ISP driver
 *
 * Copyright (C) 2012 ASUS Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <media/yuv_sensor.h>

#undef _CAM_SENSOR_DETECT_
#ifdef _CAM_SENSOR_DETECT_
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <mach/clk.h>
#endif
#include <linux/switch.h>
#include <mach/board-cardhu-misc.h>

#define I7002A_SDEV_NAME "camera"

#define SPI_CMD_BYTE_READ 	0x03
#define SPI_CMD_RD_ID 		0x9F
#define SPI_CMD_WRT_EN		0x06
#define SPI_CMD_BYTE_PROG 	0x02
#define SPI_CMD_RD_STS		0x05
#define SPI_CMD_BYTE_PROG_AAI	0xAD
#define SPI_CMD_WRT_STS_EN	0x50
#define SPI_CMD_WRT_STS 	0x01
#define SPI_CMD_WRT_DIS 	0x04
#define SPI_CMD_ERASE_ALL	0xC7

#define SENSOR_ID_MI1040	0x2481
#define SENSOR_ID_OV2720	0x2720
#define SENSOR_ID_IMX175	0x175

struct switch_dev   i7002a_sdev;
static unsigned int version_num_in_isp = 0xffffff;
static unsigned int front_chip_id = 0xABCD;

/* mi1040 format:
 * 0: YUV
 * 1: RGB
 * 2: Bayer
 */
static unsigned int mi1040_output_format = 0xFF;
static char g_i7002a_binfile_path[80];

static int dbg_i7002a_page_index = 0;

/* iCatch Camera Firmware Header
 * It locates on the end of the bin file.
 * Total: 32 bytes.
 * byte[0] ~ byte[7]: 0xFF's
 * byte[8] ~ byte[11]: Compensation for Overall Checksum
 * byte[12] ~ byte[15]: Overall Checksum

 * byte[16] ~ byte[20]: 0xFF's
 * byte[21]: Front Format
 * byte[22]: Rear Lane#
 * byte[23]: Front Lane#
 * byte[24] ~ byte[25]: Rear Sensor ID
 * byte[26] ~ byte[27]: Front sensor ID
 * byte[28] ~ byte[31]: FW Version
 */
#define BIN_FILE_HEADER_SIZE 32

#define ICATCH7002A_DELAY_TEST
#ifdef ICATCH7002A_DELAY_TEST
static u32 iCatch7002a_init_delay= 5;
static u32 touch_focus_enable=0;
#endif
#define _ENABLE_WRITE_TABLE_2_GROUP_LATCH_
#define _AVOID_GROUP_LATCH_AFTER_SET_MODE_
//#undef _AVOID_GROUP_LATCH_AFTER_SET_MODE_
#ifdef _AVOID_GROUP_LATCH_AFTER_SET_MODE_
#define _AVOID_GROUP_LATCH_TIME_MS_ 200
static int g_last_set_mode_jiffies;
#endif
#define SENSOR_WIDTH_REG 0x2703
#define SENSOR_640_WIDTH_VAL 0x0280
#define ICATCH7002A_SENSOR_NAME "i7002a"
#define CAM_TWO_MODE
#ifdef CAM_TWO_MODE
static unsigned int g_div = 100;
static int g_initialized_1280_960=0;
static int g_initialized_1080p=0;
#endif
static bool sensor_opened = false;
static bool first_open = true;
/* Used for calculating the iCatch fw update progress */
static int page_count = -1;
static int total_page_count = -1;

enum iCatch_fw_update_status{
	ICATCH_FW_NO_CMD,
	ICATCH_FW_IS_BURNING,
	ICATCH_FW_UPDATE_SUCCESS,
	ICATCH_FW_UPDATE_FAILED,
};
static enum iCatch_fw_update_status fw_update_status = ICATCH_FW_NO_CMD;

enum iCatch_flash_type{
	ICATCH_FLASH_TYPE_ST,
	ICATCH_FLASH_TYPE_SST,
};
static enum iCatch_flash_type flash_type = ICATCH_FLASH_TYPE_ST;

struct sensor_reg {
	u16 addr;
	u16 val;
};

struct sensor_reg_2 {
  u16 cmd;
	u16 addr;
	u16 val;
  u16 val2;
};

struct sensor_info {
	int mode;
	struct i2c_client *i2c_client;
	struct yuv_sensor_platform_data *pdata;
#ifdef _CAM_SENSOR_DETECT_
	struct device dev;
#endif
};

static struct sensor_info *info;
static int touch_mode = TOUCH_STATUS_OFF;
static bool caf_mode = false;
static int focus_control = 0;

static struct sensor_reg Autofocus_Trigger[] = {
{0x7140, 0x00}, //ROI Size_H
{0x7141, 0x50}, //ROI Size_H
{0x7142, 0x02}, //ROI X_H
{0x7143, 0x58}, //ROI X_L
{0x7144, 0x02}, //ROI Y_H
{0x7145, 0x58}, //ROI Y_L
{SENSOR_TABLE_END, 0x0000}
};

enum {
	SENSOR_MODE_3264x2448,
	SENSOR_MODE_2592x1944,
	SENSOR_MODE_1920x1080,
	SENSOR_MODE_1280x960,
	SENSOR_MODE_1280x720,
	SENSOR_MODE_640x480,
};

int tegra_camera_mclk_on_off(int on);

void i7002a_isp_on(int power_on)
{
	printk("%s(%d)++\n", __FUNCTION__, power_on);
	if (power_on == 0) {
		if (sensor_opened == true) {
			if (info->pdata && info->pdata->power_off) {
				tegra_camera_mclk_on_off(0);
				info->pdata->power_off();
				sensor_opened = false;
			} else {
				printk("%s: iCatch7002a info isn't enough for power_off.\n", __FUNCTION__);
			}
		} else
			printk("%s: No action. sensor is already OFF\n", __FUNCTION__);
	} else {
		/* Power on ISP */
		if (sensor_opened == false) {
			if (info->pdata && info->pdata->power_on) {
				info->pdata->power_on();
				tegra_camera_mclk_on_off(1);
				msleep(100);
				sensor_opened = true;
			} else {
				printk("%s: iCatch7002a info isn't enough for power_on.\n", __FUNCTION__);
			}
		} else {
			printk("%s: sensor is already ON?\n", __FUNCTION__);
			if (info->pdata && info->pdata->power_off) {
				printk("%s: Turn off first.\n", __FUNCTION__);
				info->pdata->power_off();
				sensor_opened = false;
			} else {
				printk("%s: iCatch7002a info isn't enough for power_off.\n", __FUNCTION__);
			}
			msleep(100);
			printk("%s: Re-power_on.\n", __FUNCTION__);
			i7002a_isp_on(1);
		}
	}
	printk("%s(%d)--\n", __FUNCTION__, power_on);
}

static int sensor_read_reg(struct i2c_client *client, u16 addr, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[4];
	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8) (addr >> 8);;
	data[1] = (u8) (addr & 0xff);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;

	msg[1].len = 1;
	msg[1].buf = data + 2;

	err = i2c_transfer(client->adapter, msg, 2);

	if (err != 2)
		return -EINVAL;

	memcpy(val, data+2, 1);
	*val=*val&0xff;

	return 0;
}

static int sensor_write_reg(struct i2c_client *client, u16 addr, u16 val)
{
	int err;
	struct i2c_msg msg;
	unsigned char data[4];
	int retry = 0;
	if (!client->adapter)
		return -ENODEV;

	data[0] = (u8) (addr >> 8);
	data[1] = (u8) (addr & 0xff);
	data[2] = (u8) (val & 0xff);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = data;
	do {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			return 0;
		retry++;
		pr_err("yuv_sensor : i2c transfer failed, retrying %x %x\n",
		       addr, val);
		pr_err("yuv_sensor : i2c transfer failed, count %x, err= 0x%x\n",
		       __FUNCTION__, __LINE__, msg.addr, err);
//		msleep(3);
	} while (retry <= SENSOR_MAX_RETRIES);

	if(err == 0) {
		printk("%s(%d): i2c_transfer error, but return 0!?\n", __FUNCTION__, __LINE__);
		err = 0xAAAA;
	}

	return err;
}

static int sensor_sequential_write_reg(struct i2c_client *client, unsigned char *data, u16 datasize)
{
	int err;
	struct i2c_msg msg;
	int retry = 0;

              return 0;
	if (datasize==0)
		return 0;
	if (!client->adapter)
		return -ENODEV;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = datasize;
	msg.buf = data;
	do {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			return 0;
		retry++;
		//pr_err("yuv_sensor : i2c transfer failed, retrying %x %x\n",
		      // addr, val);
		pr_err("yuv_sensor : i2c transfer failed, count %x \n",
		       msg.addr);
//		msleep(3);
	} while (retry <= SENSOR_MAX_RETRIES);

	return err;
}

static int build_sequential_buffer(unsigned char *pBuf, u16 width, u16 value) {
	u32 count = 0;

	switch (width)
	{
	  case 0:
	  // possibly no address, some focusers use this
	  break;

	  // cascading switch
	  case 32:
	    pBuf[count++] = (u8)((value>>24) & 0xFF);
	  case 24:
	    pBuf[count++] = (u8)((value>>16) & 0xFF);
	  case 16:
	    pBuf[count++] = (u8)((value>>8) & 0xFF);
	  case 8:
	    pBuf[count++] = (u8)(value & 0xFF);
	    break;

	  default:
	    printk("Unsupported Bit Width %d\n", width);
	    break;
	}
	return count;

}

static int sensor_write_table(struct i2c_client *client,
			      const struct sensor_reg table[])
{
	int err;
	const struct sensor_reg *next;
	u16 val;
	unsigned char data[10];
	u16 datasize = 0;

	//for (next = table; next->addr != SENSOR_TABLE_END; next++) {
	next = table;
	while (next->addr != SENSOR_TABLE_END) {
		if (next->addr == SENSOR_WAIT_MS) {
			msleep(next->val);
			next +=1;
			continue;
		}
		if (next->addr == SEQ_WRITE_START) {
			next += 1;
			while (next->addr !=SEQ_WRITE_END) {
				if (datasize==0) {//
					datasize += build_sequential_buffer(&data[datasize], 16, next->addr);
					datasize += build_sequential_buffer(&data[datasize], 8, next->val);
				}
				else
					datasize += build_sequential_buffer(&data[datasize], 8, next->val);
				if (datasize==10) {
					sensor_sequential_write_reg(client, data, datasize);
					datasize = 0;
				}
				next += 1;
			}
			sensor_sequential_write_reg(client, data, datasize); //flush out the remaining buffer.
			datasize = 0;
		}
		else {
			val = next->val;

			err = sensor_write_reg(client, next->addr, val);
			if (err) {
				printk("%s(%d): isensor_write_reg ret= 0x%x\n", __FUNCTION__, __LINE__, err);
				return err;
			}
		}
		next += 1;
	}
	return 0;
}
/*
static int sensor_write_table_2(struct i2c_client *client,
			      const struct sensor_reg_2 table[])
{
	int err;
	const struct sensor_reg_2 *next;
	u16 val,mask;
  int b_group_latch=1;
  return 0;
#ifdef _AVOID_GROUP_LATCH_AFTER_SET_MODE_
  if ((jiffies - g_last_set_mode_jiffies) > (HZ * _AVOID_GROUP_LATCH_TIME_MS_ / 1000) )
  {
    printk("do group latch: jiffies=%d, last_set_mode_jiffies=%d, threshold=%d\n",
     jiffies, g_last_set_mode_jiffies , (HZ * _AVOID_GROUP_LATCH_TIME_MS_ / 1000)
     );
    b_group_latch=1;
  }
  else
  {
    printk("skip group latch: jiffies=%d, last_set_mode_jiffies=%d, threshold=%d\n",
     jiffies, g_last_set_mode_jiffies , (HZ * _AVOID_GROUP_LATCH_TIME_MS_ / 1000)
     );
    b_group_latch=0;
  }
#endif

	pr_info("yuv %s\n",__func__);
#ifdef _ENABLE_WRITE_TABLE_2_GROUP_LATCH_
    if (b_group_latch) {
	err = sensor_write_reg(client, 0x3004, 0xdf);//;Disable MCU clock
	err = sensor_write_reg(client, 0x3212, 0x00);//;Enable group0
	if (err)
  		return err;
	}
#endif
	for (next = table; next->cmd != SENSOR_TABLE_END; next++) {
		if (next->cmd == SENSOR_WAIT_MS) {
			msleep(next->val);
			continue;
		}
    if (next->cmd == SENSOR_BYTE_WRITE)
    {
      val = next->val;
      err = sensor_write_reg(client, next->addr, val);
      if (err)
      {
        printk("write [0x%X] as 0x%X error, err=%d",next->addr, val, err);
      }
      else
      {
        printk("write [0x%X] as 0x%X\n",next->addr,val);
      }

    }
		else if (next->cmd == SENSOR_MASK_BYTE_WRITE)
    {
      mask = next->val2;
      err = sensor_read_reg(client, next->addr,&val);
      if (err)
      {
        printk("read [0x%X] error, err=%d",next->addr,err);
        //return err;
      }
      else
      {
        printk("read [0x%X] as 0x%X\n",next->addr,val);
      }
      val = (val & ~mask) | next->val;
      err = sensor_write_reg(client, next->addr,val);
      if (err)
      {
        printk("mask write [0x%X] as 0x%X error, err=%d",next->addr, val, err);
        //return err;
      }
      else
      {
        printk("mask write [0x%X] as 0x%X\n",next->addr,val);
      }

    }
	}
#ifdef _ENABLE_WRITE_TABLE_2_GROUP_LATCH_
    if (b_group_latch) {
	err = sensor_write_reg(client, 0x3004, 0xff);//;Enable MCU clock
	err = sensor_write_reg(client, 0x3212, 0x10);//;End group0
	if (err)
		return err;
	err = sensor_write_reg(client, 0x3212, 0xa0);//;latch group0
	if (err)
		return err;
	}
	//group_latch = 1;
#endif
	//msleep(100);
#ifdef _AVOID_GROUP_LATCH_AFTER_SET_MODE_
  g_last_set_mode_jiffies = jiffies;
#endif
	return 0;
}

static int get_sensor_current_width(struct i2c_client *client, u16 *val)
{
        int err;

        err = sensor_write_reg(client, 0x098c, 0x2703);
        if (err)
          return err;

        err = sensor_read_reg(client, 0x0990, val);

        if (err)
          return err;

        return 0;
}
*/

struct sensor_reg query_mi1040_id_msb_seq[] = {
	/*Start - Power on sensor & enable clock*/
	{0x0084, 0x14},		/* To sensor clock divider */
	{0x0034, 0xFF},		/* Turn on all clock */
	{0x9030, 0x3f},
	{0x9031, 0x04},
	{0x9034, 0xf2},
	{0x9035, 0x04},
	{0x9033, 0x04},
	{0x9032, 0x3c},
	{SENSOR_WAIT_MS, 10},	/* 10ms */
	{0x9033, 0x00},
	{SENSOR_WAIT_MS, 10},	/* 10ms */
	{0x9033, 0x04},
	{0x9032, 0x3e},
	{SENSOR_WAIT_MS, 10},	/* 10ms */
	{0x9032, 0x3c},
	/*End - Power on sensor & enable clock */

	/*Start - I2C Read ID*/
	{0x9138, 0x30},  /* Sub address enable */
	{0x9140, 0x90},  /* Slave address      */
	{0x9100, 0x03},  /* Read mode          */
	{0x9110, 0x00},  /* Register addr MSB  */
	{0x9112, 0x00},  /* Register addr LSB  */
	{0x9104, 0x01},  /* Trigger I2C read   */
	{SENSOR_WAIT_MS, 1},	/* 1ms */
	{SENSOR_TABLE_END, 0x0000}
};

struct sensor_reg query_mi1040_id_lsb_seq[] = {
	{0x9110, 0x00},  /* Register addr MSB  */
	{0x9112, 0x01},  /* Register addr LSB  */
	{0x9104, 0x01},  /* Trigger I2C read   */
	{SENSOR_WAIT_MS, 1},	/* 1ms */
	{SENSOR_TABLE_END, 0x0000}
};

struct sensor_reg query_mi1040_output_format_seq[] = {
	/*Start - I2C Read YUV/RGB mode*/
	{0x9138, 0x30},  /* Sub address enable */
	{0x9140, 0x90},  /* Slave address      */
	{0x9100, 0x03},  /* Read mode          */
	{0x9110, 0xC8},  /* Register addr MSB  */
	{0x9112, 0x6C},  /* Register addr LSB  */
	{0x9104, 0x01},  /* Trigger I2C read   */
	{SENSOR_WAIT_MS, 1},	/* 1ms */
	{SENSOR_TABLE_END, 0x0000}
};
/*
facing: 0 for back; 1 for front
*/
static unsigned int i7002a_get_sensor_id(int facing)
{
	u16 tmp;
	int chip_id = 0;

	if (facing == 0) {
		/* back camera: SONY IMX175, chip_id=0x175; */
		sensor_write_reg(info->i2c_client, 0x0084, 0x14); /* To sensor clock divider */
		sensor_write_reg(info->i2c_client, 0x0034, 0xFF); /* Turn on all clock */
		sensor_write_reg(info->i2c_client, 0x9030, 0x3f);
		sensor_write_reg(info->i2c_client, 0x9031, 0x04);
		sensor_write_reg(info->i2c_client, 0x9034, 0xf2);
		sensor_write_reg(info->i2c_client, 0x9035, 0x04);
		sensor_write_reg(info->i2c_client, 0x9032, 0x00);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9032, 0x20);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9032, 0x30);
		msleep(10);
		/*End - Power on sensor & enable clock */
		sensor_write_reg(info->i2c_client, 0x9008, 0x00); /* Need to check with vincent */
		sensor_write_reg(info->i2c_client, 0x9009, 0x00);
		sensor_write_reg(info->i2c_client, 0x900A, 0x00);
		sensor_write_reg(info->i2c_client, 0x900B, 0x00);

		/*Start - I2C Read*/
		sensor_write_reg(info->i2c_client, 0x9238, 0x30); /* Sub address enable */
		sensor_write_reg(info->i2c_client, 0x9240, 0x20); /* Slave address      */
		sensor_write_reg(info->i2c_client, 0x9200, 0x03); /* Read mode          */
		sensor_write_reg(info->i2c_client, 0x9210, 0x00); /* Register addr MSB  */
		sensor_write_reg(info->i2c_client, 0x9212, 0x00); /* Register addr LSB  */
		sensor_write_reg(info->i2c_client, 0x9204, 0x01); /* Trigger I2C read   */

		msleep(10);
		sensor_read_reg(info->i2c_client, 0x9211, &tmp);
		// printk("0x%x\n", tmp);
		chip_id = (tmp << 8) & 0xFF00;

		sensor_write_reg(info->i2c_client, 0x9210, 0x00); /* Register addr MSB  */
		sensor_write_reg(info->i2c_client, 0x9212, 0x01); /* Register addr LSB  */
		sensor_write_reg(info->i2c_client, 0x9204, 0x01); /* Trigger I2C read   */

		msleep(10);
		sensor_read_reg(info->i2c_client, 0x9211, &tmp);
		// printk("0x%x\n", tmp);
		chip_id = chip_id  | (tmp & 0xFF);
	} else if (facing == 1){
		/* Start - Power on sensor & enable clock - Front I2C (OV2720);
		 * ov2720: chip_id= 0x2720;
		 */
		sensor_write_reg(info->i2c_client, 0x0084, 0x14); /* To sensor clock divider */
		sensor_write_reg(info->i2c_client, 0x0034, 0xFF); /* Turn on all clock */
		sensor_write_reg(info->i2c_client, 0x9030, 0x3f);
		sensor_write_reg(info->i2c_client, 0x9031, 0x04);
		sensor_write_reg(info->i2c_client, 0x9034, 0xf3);
		sensor_write_reg(info->i2c_client, 0x9035, 0x04);

		sensor_write_reg(info->i2c_client, 0x9032, 0x02);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9032, 0x00);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9033, 0x00);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9033, 0x04);
		msleep(10);
		sensor_write_reg(info->i2c_client, 0x9034, 0xf2);
		/*End - Power on sensor & enable clock */

		sensor_write_reg(info->i2c_client, 0x9008, 0x00); /* Need to check with vincent */
		sensor_write_reg(info->i2c_client, 0x9009, 0x00);
		sensor_write_reg(info->i2c_client, 0x900A, 0x00);
		sensor_write_reg(info->i2c_client, 0x900B, 0x00);

		/*Start - I2C Read*/
		sensor_write_reg(info->i2c_client, 0x9138, 0x30); /* Sub address enable */
		sensor_write_reg(info->i2c_client, 0x9140, 0x6C); /* Slave address      */
		sensor_write_reg(info->i2c_client, 0x9100, 0x03); /* Read mode          */
		sensor_write_reg(info->i2c_client, 0x9110, 0x30); /* Register addr MSB  */
		sensor_write_reg(info->i2c_client, 0x9112, 0x0a); /* Register addr LSB  */
		sensor_write_reg(info->i2c_client, 0x9104, 0x01); /* Trigger I2C read   */

		msleep(10);
		sensor_read_reg(info->i2c_client, 0x9111, &tmp);

		//printk("0x%x\n", tmp);
		chip_id = (tmp << 8) & 0xFF00;

		sensor_write_reg(info->i2c_client, 0x9110, 0x30); /* Register addr MSB  */
		sensor_write_reg(info->i2c_client, 0x9112, 0x0b); /* Register addr LSB  */
		sensor_write_reg(info->i2c_client, 0x9104, 0x01); /* Trigger I2C read   */

		msleep(10);
		sensor_read_reg(info->i2c_client, 0x9111, &tmp);
		//printk("0x%x\n", tmp);
		chip_id = chip_id  | (tmp & 0xFF);

		if (chip_id != SENSOR_ID_OV2720) {
			/* Check if mi1040 is available. */
			sensor_write_table(info->i2c_client, query_mi1040_id_msb_seq);
			sensor_read_reg(info->i2c_client, 0x9111, &tmp);

			chip_id = (tmp << 8) & 0xFF00;

			sensor_write_table(info->i2c_client, query_mi1040_id_lsb_seq);
			sensor_read_reg(info->i2c_client, 0x9111, &tmp);
			chip_id = chip_id  | (tmp & 0xFF);
		}
	} else {
		/* Unknown */
		chip_id = 0xabcdef;
	}

	return chip_id;
}

int I2C_SPIInit(void)
{
	int ret = 0;
	//  I2CDataWrite(0x0026,0xc0);
	//  I2CDataWrite(0x4051,0x01); /* spien */
	//  I2CDataWrite(0x40e1,0x00); /* spi mode */
	//  I2CDataWrite(0x40e0,0x11); /* spi freq */
	struct sensor_reg SPI_init_seq[] = {
		{0x0026, 0xc0},
		{0x4051, 0x01},
		{0x40e1, 0x00},
		{0x40e0, 0x11},
		{SENSOR_TABLE_END, 0x0000}
	};

	ret = sensor_write_table(info->i2c_client, SPI_init_seq);
	if(ret) {
		printk("%s: init fail. ret= 0x%x\n", __FUNCTION__, ret);
	}
	return ret;
}

u32 I2C_SPIFlashPortRead(void)
{
	u16 ret;

	// ret = hsI2CDataRead(0x40e4);
      sensor_read_reg(info->i2c_client, 0x40e4, &ret);
	/* polling SPI state machine ready */
#if 0
    if (I2C_SPIFlashPortWait() != SUCCESS) {
        return 0;
    }
#endif
	//ret = hsI2CDataRead(0x40e5);
      sensor_read_reg(info->i2c_client, 0x40e5, &ret);

    return (u32)ret;
}

u32 I2C_SPIFlashRead(
	u32 addr,
	u32 pages,
	u8 *pbuf
)
{
	u32 err = 0;
	u32 i, size=0;
	u32 pageSize = 0x100;

	addr = addr * pageSize;
	size = pages*pageSize;

	// I2CDataWrite(0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e7, 0x00);
	// I2C_SPIFlashPortWrite(SPI_CMD_BYTE_READ);               /* Write one byte command*/
	sensor_write_reg(info->i2c_client, 0x40e3, SPI_CMD_BYTE_READ);
	// I2C_SPIFlashPortWrite((u8)(addr >> 16));               /* Send 3 bytes address*/
	// I2C_SPIFlashPortWrite((u8)(addr >> 8));
	// I2C_SPIFlashPortWrite((u8)(addr));
	sensor_write_reg(info->i2c_client, 0x40e3, (u8)(addr >> 16));
	sensor_write_reg(info->i2c_client, 0x40e3, (u8)(addr >> 8));
	sensor_write_reg(info->i2c_client, 0x40e3, (u8)(addr));

	for (i = 0; i < size ; i++) {
		*pbuf = I2C_SPIFlashPortRead();
		if((i%256)==0)
			printk("%s: page count: 0x%x\n", __FUNCTION__, (i/256));
		pbuf ++;
	}

	sensor_write_reg(info->i2c_client, 0x40e7, 0x01);

	return err;
}

u32 I2C_SPIFlashReadId(void)
{
	u8 id[3];
	u32 ID;

	id[0] = 0;
	id[1] = 0;
	id[2] = 0;

	//hsI2CDataWrite(0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);

	//err = I2C_SPIFlashPortWrite(SPI_CMD_RD_ID); /*read ID command*/
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_RD_ID);

#if 0
	if (err != SUCCESS) {
		printf("Get serial flash ID failed\n");
		return 0;
	}
#endif

	id[0] = I2C_SPIFlashPortRead();    /* Manufacturer's  ID */
	id[1] = I2C_SPIFlashPortRead();    /* Device ID          */
	id[2] = I2C_SPIFlashPortRead();    /* Manufacturer's  ID */

	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	printk("ID %2x %2x %2x\n", id[0], id[1], id[2]);

	ID = ((u32)id[0] << 16) | ((u32)id[1] << 8) | \
    ((u32)id[2] << 0);

	return ID;
}

static const u32 stSpiIdInfo[29] =
{
	/*EON*/
	0x001C3117,
	0x001C2016,
	0x001C3116,
	0x001C3115,
	0x001C3114,
	0x001C3113,
	/*Spansion*/
	0x00012018,
	0x00010216,
	0x00010215,
	0x00010214,
	/*ST*/
	0x00202018,
	0x00202017,
	0x00202016,
	0x00202015,
	0x00202014,
	/*MXIC*/
	0x00C22018,
	0x00C22017,
	0x00C22016,
	0x00C25e16,
	0x00C22015,
	0x00C22014,
	0x00C22013,
	/*Winbond*/
	0x00EF3017,
	0x00EF3016,
	0x00EF3015,
	0x00EF3014,
	0x00EF3013,
	0x00EF5013,
	/*Fail*/
	0x00000000,
};

static const u32 sstSpiIdInfo[6] =
{
	/*ESMT*/
	0x008C4016,
	/*SST*/
	0x00BF254A,
	0x00BF2541,
	0x00BF258E,
	0x00BF258D,
	/*Fail*/
	0x00000000,
};

u32
BB_SerialFlashTypeCheck(
	u32 id
)
{
	u32 i=0;
	u32 fullID = 1;
	u32 shift = 0, tblId, type = 0;

	/* check whether SST type serial flash */
	while( 1 ){
		tblId = sstSpiIdInfo[i] >> shift;
		if( id == tblId ) {
			printk("SST type serial flash\n");
			type = 2;
			break;
		}
		if( id == 0x00FFFFFF || id == 0x00000000) {
			return 0;
		}
		if( sstSpiIdInfo[i] == 0x00000000 ) {
			if( fullID ){
				fullID = 0;/* sarch partial ID */
				i = 0;
				shift = 16;
				id = id >> shift;
				continue;
			}
			type = 3;
			break;
		}
		i ++;
	}
	if( type == 2 )
		return type;

	i = 0;
	/* check whether ST type serial flash */
	while( 1 ){
		tblId = stSpiIdInfo[i] >> shift;
		if( id == tblId ) {
			printk("ST Type serial flash\n");
			type = 1;
			break;
		}
		if( id == 0x00FFFFFF || id == 0x00000000) {
			return 0;
		}
		if( stSpiIdInfo[i] == 0x00000000 ) {
			if( fullID ){
				fullID = 0;/* sarch partial ID */
				i = 0;
				shift = 16;
				id = id >> shift;
				continue;
			}
			type = 3;
			break;
		}
		i ++;
	}

	return type;
}

int I2C_SPIFlashWrEnable(void)
{
	int ret = 0;
	//hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_EN);
	//hsI2CDataWrite(0x40e7,0x01);
	struct sensor_reg I2C_SPIFlashWrEnable_seq[] = {
		{0x40e7, 0x00},
		{0x40e3, SPI_CMD_WRT_EN},
		{0x40e7, 0x01},
		{SENSOR_TABLE_END, 0x0000}
	};

	ret = sensor_write_table(info->i2c_client, I2C_SPIFlashWrEnable_seq);

	if(ret) {
		printk("%s: fail. ret= 0x%x\n", __FUNCTION__, ret);
	}
	return ret;
}

u32 I2C_SPIStsRegRead(void)
{
	u32 ret;

	//hsI2CDataWrite(0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_RD_STS);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_RD_STS);
	ret = I2C_SPIFlashPortRead();

	// hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	return ret;
}

void I2C_SPITimeOutWait(u32 poll, u32 *ptimeOut)
{
    /* MAX_TIME for SECTOR/BLOCK ERASE is 25ms */
    u32 sts;
    u32 time = 0;
    while (1) {
        sts = I2C_SPIStsRegRead();
        if (!(sts & poll))	/* sfStatusRead() > 4.8us */ {
            break;
        }
        time ++;
        if( *ptimeOut < time ) {
            printk("iCatch: TimeOut %d, sts=0x%x, poll=0x%x\n",time,sts,poll);
            break;
        }
    }
}

int I2C_SPIStChipErase(
	void
)
{
	u32 timeout;
	int ret = 0;
	printk("iCatch: ST Chip Erasing...\n");

	//hsI2CDataWrite(0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_STS);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_STS);
	//I2C_SPIFlashPortWrite(0x02);
	sensor_write_reg(info->i2c_client, 0x40e3,0x02);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	ret = I2C_SPIFlashWrEnable();
	if (ret) {
		printk("iCatch: ST Chip Erase fail, ret= 0x%x\n", ret);
		return ret;
	}

	//hsI2CDataWrite(0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_ERASE_ALL);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_ERASE_ALL);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	timeout = 0xffffffff;
	I2C_SPITimeOutWait(0x01, &timeout);
#if 0
	ros_thread_sleep(1);
#endif
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);
	printk("iCatch: ST Chip Erased\n");
	return 0;
}

int I2C_SPISstChipErase()
{
	u32 timeout;
	int ret = 0;
	printk("iCatch: SST Chip Erasing...\n");

	ret = I2C_SPIFlashWrEnable();
	if (ret) {
		printk("iCatch: SST Chip Erase fail, ret= 0x%x\n", ret);
		return ret;
	}

	//hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_STS_EN); /*Write Status register command*/
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_STS_EN);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	//hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_STS);
	//I2C_SPIFlashPortWrite(0x02);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_STS);
	sensor_write_reg(info->i2c_client, 0x40e3,0x02);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	I2C_SPIFlashWrEnable();

	//hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_ERASE_ALL);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_ERASE_ALL);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	timeout = 0xffffffff;
	I2C_SPITimeOutWait(0x01, &timeout);
	//msleep(500);
	printk("iCatch: SST Chip Erased\n");
	return 0;
}

void writeUpdateProgresstoFile(int page_left, int total_page_num)
{
	struct file *fp_progress = NULL;
	mm_segment_t old_fs;
	loff_t offset = 0;
	char str_progress[4];
	int percentage = 0;

	percentage = 100 * (total_page_num - page_left + 1)/total_page_num;

	if(page_left % 32 == 1){
		printk("%s: page:0x%x; percentage= %d;\n", __FUNCTION__, page_left, percentage);
		fp_progress = filp_open("/data/isp_fw_update_progress", O_RDWR | O_CREAT, 0);
		if ( IS_ERR_OR_NULL(fp_progress) ){
			filp_close(fp_progress, NULL);
			printk("%s: open %s fail\n", __FUNCTION__, "/data/isp_fw_update_progress");
		}
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		offset = 0;
		if (fp_progress->f_op != NULL && fp_progress->f_op->write != NULL){
			sprintf(str_progress, "%d\n", percentage);
			fp_progress->f_op->write(fp_progress,
				str_progress,
				strlen(str_progress),
				&offset);
		}else
			pr_err("%s: f_op might be null\n", __FUNCTION__);
		set_fs(old_fs);
		filp_close(fp_progress, NULL);
	}
}

u32 I2C_SPIFlashWrite(
	u32 addr,
	u32 pages,
	u8 *pbuf
)
{
	u32 i, err = 0;
	u32 pageSize = 0x100;

	addr = addr * pageSize;

	printk("iCatch: ST type writing...\n");
	total_page_count = (int)pages;

	while( pages ) {
		page_count = (int)pages;
		writeUpdateProgresstoFile(page_count, total_page_count);

		I2C_SPIFlashWrEnable();
		//hsI2CDataWrite(0x40e7,0x00);
		sensor_write_reg(info->i2c_client, 0x40e7,0x00);
		//I2C_SPIFlashPortWrite(SPI_CMD_BYTE_PROG); /* Write one byte command*/
		sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_BYTE_PROG);
		// I2C_SPIFlashPortWrite((UINT8)(addr >> 16)); /* Send 3 bytes address*/
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr >> 16));
		// I2C_SPIFlashPortWrite((UINT8)(addr >> 8));
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr >> 8));
		// I2C_SPIFlashPortWrite((UINT8)(addr));
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr));

		for (i = 0; i < pageSize ; i++) {
			// How about "Early return" here?
			// I2C_SPIFlashPortWrite(*pbuf);
			sensor_write_reg(info->i2c_client, 0x40e3,(u8)(*pbuf));
			pbuf++;
		}
		// hsI2CDataWrite(0x40e7,0x01);
		sensor_write_reg(info->i2c_client, 0x40e7,0x01);
		addr += pageSize;
		pages --;
		// tmrUsWait(2000);
		udelay(2000);
	}
	printk("iCatch: ST type writing Done\n");
	return err;
}

void I2C_SPISstStatusWrite(u8 dat)
{
	u32 timeout, poll;

	I2C_SPIFlashWrEnable();

	//hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_STS_EN);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_STS_EN);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	// hsI2CDataWrite(0x40e7,0x00);
	//I2C_SPIFlashPortWrite(SPI_CMD_WRT_STS);
	//I2C_SPIFlashPortWrite(dat);
	//hsI2CDataWrite(0x40e7,0x01);
	sensor_write_reg(info->i2c_client, 0x40e7,0x00);
	sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_STS);
	printk("%s: dat=%d\n", __FUNCTION__, dat);
	sensor_write_reg(info->i2c_client, 0x40e3,dat);
	printk("%s: dat=%d; Done.\n", __FUNCTION__, dat);
	sensor_write_reg(info->i2c_client, 0x40e7,0x01);

	poll = 0x01;
#if 0
	if( spiDev.bus != SPI_1BIT_MODE ) {/* 1 bit mode */
		poll = 0x80;
	} else {
		poll = 0x01;
	}
#endif
    timeout = 100000;
    I2C_SPITimeOutWait(poll, &timeout);
    //msleep(500);
    return;
}

u32 I2C_SPISstFlashWrite(
	u32 addr,
	u32 pages,
	u8 *pbuf
)
{
	u32 i, err = 0;
	u32 pageSize = 0x100;
	u32 timeout = 100000;

	addr = addr * pageSize;

	printk("iCatch: SST type writing...\n");
	I2C_SPISstStatusWrite(0x40);

	total_page_count = (int)pages;

	while( pages ) {
		page_count = (int)pages;
		writeUpdateProgresstoFile(page_count, total_page_count);

		I2C_SPIFlashWrEnable();
		//hsI2CDataWrite(0x40e7,0x00);
		sensor_write_reg(info->i2c_client, 0x40e7,0x00);
		//I2C_SPIFlashPortWrite(SPI_CMD_BYTE_PROG_AAI); /* Write one byte command*/
		sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_BYTE_PROG_AAI);
		//I2C_SPIFlashPortWrite((UINT8)(addr >> 16)); /* Send 3 bytes address*/
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr >> 16));
		//I2C_SPIFlashPortWrite((UINT8)(addr >> 8));
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr >> 8));
		//I2C_SPIFlashPortWrite((UINT8)(addr));
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(addr));
		//I2C_SPIFlashPortWrite(*pbuf);
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(*pbuf));
		pbuf++;
		//I2C_SPIFlashPortWrite(*pbuf);
		sensor_write_reg(info->i2c_client, 0x40e3,(u8)(*pbuf));
		pbuf++;
		//hsI2CDataWrite(0x40e7,0x01);
		sensor_write_reg(info->i2c_client, 0x40e7,0x01);
		timeout = 100000;
		I2C_SPITimeOutWait(0x01,&timeout);

		for (i = 2; i < pageSize ; i = i+2) {
			//hsI2CDataWrite(0x40e7,0x00);
			sensor_write_reg(info->i2c_client, 0x40e7,0x00);
			//I2C_SPIFlashPortWrite(SPI_CMD_BYTE_PROG_AAI);
			sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_BYTE_PROG_AAI);
			// I2C_SPIFlashPortWrite(*pbuf);
			sensor_write_reg(info->i2c_client, 0x40e3,(u8)(*pbuf));
			pbuf++;
			// I2C_SPIFlashPortWrite(*pbuf);
			sensor_write_reg(info->i2c_client, 0x40e3,(u8)(*pbuf));
			pbuf++;
			// hsI2CDataWrite(0x40e7,0x01);
			sensor_write_reg(info->i2c_client, 0x40e7,0x01);
			timeout = 100000;
			I2C_SPITimeOutWait(0x01,&timeout);
		}

		// hsI2CDataWrite(0x40e7,0x00);
		sensor_write_reg(info->i2c_client, 0x40e7,0x00);
		//I2C_SPIFlashPortWrite(SPI_CMD_WRT_DIS);
		sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_DIS);
		//hsI2CDataWrite(0x40e7,0x01);
		sensor_write_reg(info->i2c_client, 0x40e7,0x01);

		addr += pageSize;
		pages --;

		//hsI2CDataWrite(0x40e7,0x00);
		sensor_write_reg(info->i2c_client, 0x40e7,0x00);
		//I2C_SPIFlashPortWrite(SPI_CMD_WRT_DIS);
		sensor_write_reg(info->i2c_client, 0x40e3,SPI_CMD_WRT_DIS);
		//hsI2CDataWrite(0x40e7,0x01);
		sensor_write_reg(info->i2c_client, 0x40e7,0x01);
	}
	printk("iCatch: SST type writing Done.\n");
	return err;
}

/* get_one_page_from_i7002a():
 *   Dump the ISP page whose index is "which_page" to "pagebuf".
 *   mclk, power & rst are requisite for getting correct page data.
 */
void get_one_page_from_i7002a(int which_page, u8* pagebuf)
{
	int i = 0;
	int ret = 0;
	//I2CDataWrite(0x70c4,0x00);
	//I2CDataWrite(0x70c5,0x00);
	sensor_write_reg(info->i2c_client, 0x70c4,0x00);
	sensor_write_reg(info->i2c_client, 0x70c5,0x00);

	ret = I2C_SPIInit();
	if (ret) {
		printk("%s: get nothing. ret= %d", __FUNCTION__, ret);
		return;
	}

	I2C_SPIFlashReadId();

	I2C_SPIFlashRead(which_page, 1, pagebuf);

#if 1 // dump to kmsg ?
	printk("page#%d:\n", which_page);
	for(i=0; i < 0x100; i++) {
		if(i%16 == 0)
			printk("[%04x]", i);
		printk("%02X ",  pagebuf[i]);
		if(i%16 == 15)
			printk("\n");
	}
#endif
}

unsigned int get_fw_version_in_isp(void)
{
	u8 tmp_page[0x100];
	unsigned int vn = 0xABCDEF;
	int i = 0;
	int retry = 10;
	bool b_ok;

	for (i = 0; i < retry; i++) {
		int j =0;
		b_ok = true;

		/* The fw veriosn is in the page with the index, 2047.*/
		get_one_page_from_i7002a(2047, tmp_page);

		/* The header format looks like:
		 * FF FF FF FF FF FF FF XX XX XX XX XX XX XX XX
		 * FF FF FF FF FF XX XX XX XX XX XX XX XX XX XX
		 */
		for (j = 0; j < 8; j++) {
			if (tmp_page[0x100 - BIN_FILE_HEADER_SIZE +j] != 0xFF) {
				b_ok = false;
				break;
			}
		}
		if (b_ok == true)
			break;
		else {
			printk("%s: wrong page data? Try again (%d).\n", __FUNCTION__, i);
			msleep(10);
		}
	}

	if (b_ok == true)
		vn = (tmp_page[0xFF - 1] <<16) | (tmp_page[0xFF - 2] << 8) | tmp_page[0xFF -3];
	printk("%s: vn=0x%X\n", __FUNCTION__, vn);
	return vn;
}

void
BB_WrSPIFlash(char* binfile_path)
{
	u32 id, type;
	u32 pages;

	u8 *pbootBuf;
	u8 bin_file_header[BIN_FILE_HEADER_SIZE];
	u8 checksum1_in_bin[2], checksum2_in_bin[2];
	u8 checksum1_in_isp[2], checksum2_in_isp[2];
	unsigned int version_num_in_bin = 0xFFFFFF;
	int firmware2_offset;
	u8 tmp_page[0x100];

	struct file *fp = NULL;
	mm_segment_t old_fs;
	struct inode *inode;
	int bootbin_size = 0;
	int i, ret = 0;

	fw_update_status = ICATCH_FW_IS_BURNING;

	/* Calculate BOOT.BIN file size. */
	fp = filp_open(binfile_path, O_RDONLY, 0);

	if ( !IS_ERR_OR_NULL(fp) ){
		pr_info("filp_open success fp:%p\n", fp);
		inode = fp->f_dentry->d_inode;
		bootbin_size = inode->i_size;
		printk("%s: fp->f_dentry->d_inode->i_size=%d\n", __FUNCTION__, bootbin_size);
		pbootBuf = kmalloc(bootbin_size, GFP_KERNEL);
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		if(fp->f_op != NULL && fp->f_op->read != NULL){
			int byte_count= 0;
			printk("Start to read %s\n", binfile_path);

			byte_count = fp->f_op->read(fp, pbootBuf, bootbin_size, &fp->f_pos);

			if (byte_count <= 0) {
				printk("iCatch: EOF or error. last byte_count= %d;\n", byte_count);
				kfree(pbootBuf);
				fw_update_status = ICATCH_FW_UPDATE_FAILED;
				return;
			} else
				printk("iCatch: BIN file size= %d bytes\n", bootbin_size);

#if 0
			for(i=0; i < bootbin_size; i++) {
				printk("%c", pbootBuf[i]);
			}
			printk("\n");
#endif
		}
		set_fs(old_fs);
		filp_close(fp, NULL);
	} else if(PTR_ERR(fp) == -ENOENT) {
		pr_err("iCatch \"%s\" not found error\n", binfile_path);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		return;
	} else{
		pr_err("iCatch \"%s\" open error\n", binfile_path);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		return;
	}

	for (i=0; i < BIN_FILE_HEADER_SIZE; i++)
	{
		bin_file_header[i] = pbootBuf[bootbin_size - BIN_FILE_HEADER_SIZE + i];
		printk("%s: bin_file_header[%d]= 0x%x\n", __FUNCTION__, i,bin_file_header[i]);
	}
	version_num_in_bin = (bin_file_header[30] << 16) | (bin_file_header[29] << 8) | bin_file_header[28];

	/* Get the checksum in bin file.
	 *   firmware2_offset
	 *     = fw1 header size
	 *     + fw1 DMEM FICDMEM size
	 *     + fw1 IMEM size
	 */
	memcpy(checksum1_in_bin, pbootBuf + 10, 2);

	firmware2_offset = 16 +
		((pbootBuf[3] << 24) | (pbootBuf[2] << 16) | (pbootBuf[1] << 8) | pbootBuf[0]) +
		((pbootBuf[7] << 24) | (pbootBuf[6] << 16) | (pbootBuf[5] << 8) | pbootBuf[4]);
	memcpy(checksum2_in_bin, pbootBuf + firmware2_offset + 10, 2);

	printk("%s: checksum in bin:%02X %02X; %02X %02X\n", __FUNCTION__,
		checksum1_in_bin[0],checksum1_in_bin[1],checksum2_in_bin[0], checksum2_in_bin[1]);

	ret = I2C_SPIInit();
	if (ret) {
		printk("%s: SPI init fail. ret= 0x%x", __FUNCTION__, ret);
		kfree(pbootBuf);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		return;
	}

	id = I2C_SPIFlashReadId();

	if(id==0) {
		printk("read id failed\n");
		kfree(pbootBuf);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		return;
	}

	type = BB_SerialFlashTypeCheck(id);
	if(type == 0) {
		printk("BB_SerialFlashTypeCheck(%d) failed\n", id);
		kfree(pbootBuf);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		return;
	}

	pages = bootbin_size/0x100;

	printk("%s: pages:0x%x\n", __FUNCTION__, pages);

	/* Writing Flash here */
	if( type == 2 ) {
		flash_type = ICATCH_FLASH_TYPE_SST;
		printk("SST operation\n");
		ret = I2C_SPISstChipErase();
		if(ret) {
			printk("%s: SST erase fail.\n", __FUNCTION__);
			kfree(pbootBuf);
			fw_update_status = ICATCH_FW_UPDATE_FAILED;
			return;
		}
		I2C_SPISstFlashWrite(0, pages, pbootBuf);
	} else if( type == 1 || type == 3 ) {
		flash_type = ICATCH_FLASH_TYPE_ST;
		printk("ST operation\n");
		ret = I2C_SPIStChipErase();
		if(ret) {
			printk("%s: ST erase fail.\n", __FUNCTION__);
			kfree(pbootBuf);
			fw_update_status = ICATCH_FW_UPDATE_FAILED;
			return;
		}
		I2C_SPIFlashWrite(0, pages, pbootBuf);
	} else {
		printk("type unknown: %d; Won't update iCatch FW.\n", type);
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		kfree(pbootBuf);
		return;
	}
	kfree(pbootBuf);

	/* Check the update reult. */
	/* Compare Check sum here */
	get_one_page_from_i7002a(0, tmp_page);
	memcpy(checksum1_in_isp, tmp_page + 10, 2);

	if (memcmp(checksum1_in_isp, checksum1_in_bin, 2) == 0) {
		/* checksum1 PASS */
		firmware2_offset = 16 +
			((tmp_page[3] << 24) | (tmp_page[2] << 16) | (tmp_page[1] << 8) | tmp_page[0]) +
			((tmp_page[7] << 24) | (tmp_page[6] << 16) | (tmp_page[5] << 8) | tmp_page[4]);

		get_one_page_from_i7002a(firmware2_offset >> 8, tmp_page);
		memcpy(checksum2_in_isp, tmp_page + 10, 2);

		if (memcmp(checksum2_in_isp, checksum2_in_bin, 2) == 0) {
			/* checksum2 PASS */
			version_num_in_isp = get_fw_version_in_isp();
			if (version_num_in_isp == version_num_in_bin) {
				/* version number PASS */
				fw_update_status = ICATCH_FW_UPDATE_SUCCESS;
				printk("%s: ICATCH FW UPDATE SUCCESS.\n", __FUNCTION__);
			} else {
				/* version number FAIL */
				fw_update_status = ICATCH_FW_UPDATE_FAILED;
				printk("%s: check version FAIL: ISP(0x%06X) != BIN(0x%06X)\n", __FUNCTION__, version_num_in_isp, version_num_in_bin);
				version_num_in_isp = 0xABCDEF;
			}
		} else {
			/* checksum2 FAIL */
			fw_update_status = ICATCH_FW_UPDATE_FAILED;
			printk("%s: checksum2 FAIL: ISP(%02X %02X) != BIN(%02X %02X)\n",
				__FUNCTION__, checksum2_in_isp[0], checksum2_in_isp[1],
				checksum2_in_bin[0], checksum2_in_bin[1]);
			version_num_in_isp = 0xABCDEF;
		}
	} else {
		/* checksum1 FAIL */
		fw_update_status = ICATCH_FW_UPDATE_FAILED;
		printk("%s: checksum1 FAIL: ISP(%02X %02X) != BIN(%02X %02X)\n",
			__FUNCTION__, checksum1_in_isp[0], checksum1_in_isp[1],
			checksum1_in_bin[0], checksum1_in_bin[1]);
		version_num_in_isp = 0xABCDEF;
	}
}

static int sensor_set_mode(struct sensor_info *info, struct sensor_mode *mode)
{

	int sensor_table;
	int err;
	u16 val;
	u16 testval, i;

	pr_info("%s: xres %u yres %u\n",__func__, mode->xres, mode->yres);

              if (mode->xres == 3264 && mode->yres == 2448) {
		sensor_table = SENSOR_MODE_3264x2448;
		//sensor_write_reg(info->i2c_client, 0x7120, 0x00);//preview mode
		//sensor_write_reg(info->i2c_client, 0x7106, 0x01);
		//sensor_write_reg(info->i2c_client, 0x7106, 0x01);//preview mode
		//sensor_write_reg(info->i2c_client, 0x7120, 0x00);
		//sensor_write_reg(info->i2c_client, 0x7161, 0x00);//bypass ISP
		//sensor_write_reg(info->i2c_client, 0x7161, 0x01);
		sensor_write_reg(info->i2c_client, 0x71EB, 0x01);//AE/AWB lock
		sensor_write_reg(info->i2c_client, 0x710f, 0x00);//capture mode
		sensor_write_reg(info->i2c_client, 0x7120, 0x01);
		printk("%s: resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
		for (i=0;i<200;i++)
		{
		  sensor_read_reg(info->i2c_client, 0x72f8, &testval);
		  printk("testval=0x%X, i=%d",testval,i);
                              if (testval & 0x04) {
		    sensor_write_reg(info->i2c_client, 0x72f8, 0x04);
		    sensor_read_reg(info->i2c_client, 0x72f8, &testval);
		    printk("Clear testval=0x%X, i=%d\n",testval,i);
                                break;
                              }
                              printk("testval=0x%X, i=%d",testval,i);
                              msleep(iCatch7002a_init_delay);
                            }
	}
	else if (mode->xres == 2592 && mode->yres == 1944) {
		sensor_table = SENSOR_MODE_2592x1944;
		printk("%s: resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
	}
	else if (mode->xres == 1920 && mode->yres == 1080) {
		sensor_table = SENSOR_MODE_1920x1080;
		sensor_write_reg(info->i2c_client, 0x7106, 0x02);//preview mode
		sensor_write_reg(info->i2c_client, 0x7120, 0x00);//1920x1080
		printk("%s: resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
		if (!first_open) {
 		  for (i=0;i<200;i++) {
 		  sensor_read_reg(info->i2c_client, 0x72f8, &testval);
 		  printk("testval=0x%X, i=%d",testval,i);
 		    if (testval & 0x04) {
 		      sensor_write_reg(info->i2c_client, 0x72f8, 0x04);
 		      sensor_read_reg(info->i2c_client, 0x72f8, &testval);
 		      printk("Clear testval=0x%X, i=%d\n",testval,i);
 		      break;
 		    }
 		      printk("testval=0x%X, i=%d",testval,i);
 		      msleep(iCatch7002a_init_delay);
 		  }
		}
		else
		  first_open=false;
	}
	else if (mode->xres == 1280 && mode->yres == 960) {
		sensor_table = SENSOR_MODE_1280x960;
		//sensor_write_reg(info->i2c_client, 0x7120, 0x00);//preview mode
		//sensor_write_reg(info->i2c_client, 0x7106, 0x00);
		sensor_write_reg(info->i2c_client, 0x7106, 0x00);//preview mode
		sensor_write_reg(info->i2c_client, 0x7120, 0x00);
		//sensor_write_reg(info->i2c_client, 0x7106, 0x00);//workaround for ov2720 output size, not affect IMX175
		printk("%s: resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
		if (!first_open) {
		for (i=0;i<200;i++)
		{
		  sensor_read_reg(info->i2c_client, 0x72f8, &testval);
		  printk("testval=0x%X, i=%d",testval,i);
                              if (testval & 0x04) {
		    sensor_write_reg(info->i2c_client, 0x72f8, 0x04);
		    sensor_read_reg(info->i2c_client, 0x72f8, &testval);
		    printk("Clear testval=0x%X, i=%d\n",testval,i);
                                break;
                              }
                              printk("testval=0x%X, i=%d",testval,i);
                              msleep(iCatch7002a_init_delay);
                            }
		}
		else
		  first_open=false;
	}
	else if (mode->xres == 1280 && mode->yres == 720) {
		sensor_table = SENSOR_MODE_1280x720;
		sensor_write_reg(info->i2c_client, 0x7120, 0x00);//preview mode
		sensor_write_reg(info->i2c_client, 0x7106, 0x02);//1280x720
		printk("%s: resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
	}
	else {
		pr_err("%s: invalid resolution supplied to set mode %d %d\n",
		       __func__, mode->xres, mode->yres);
		return -EINVAL;
	}
  	info->mode = sensor_table;
	return 0;
}

static long sensor_ioctl(struct file *file,
			 unsigned int cmd, unsigned long arg)
{
    struct sensor_info *info = file->private_data;
    int err=0;

    pr_info("yuv %s\n",__func__);
    switch (cmd)
    {
    case SENSOR_IOCTL_SET_MODE:
    {
      struct sensor_mode mode;
      if (copy_from_user(&mode,(const void __user *)arg,
        sizeof(struct sensor_mode))) {
        return -EFAULT;
      }
        return sensor_set_mode(info, &mode);
    }
    case SENSOR_IOCTL_GET_STATUS:
    {
      return 0;
    }
    case SENSOR_IOCTL_SET_CAMERA:
    {
      u8 is_front_camera = 0;
      //u16 testval;
      if (copy_from_user(&is_front_camera,(const void __user *)arg,
        sizeof(is_front_camera))) {
          return -EFAULT;
      }
      printk("SET_CAMERA as 0x%X\n", is_front_camera);
      msleep(100);
      //sensor_read_reg(info->i2c_client, 0x002c, &testval);
      //printk("%s: test val is %d\n", __func__, testval);
      if (is_front_camera) {
        sensor_write_reg(info->i2c_client, 0x1011, 0x01);//cpu reset
        sensor_write_reg(info->i2c_client, 0x941C, 0x04);
        sensor_write_reg(info->i2c_client, 0x9010, 0x01);
        sensor_write_reg(info->i2c_client, 0x9010, 0x00);
        sensor_write_reg(info->i2c_client, 0x1306, 0x01);//front camera
        sensor_write_reg(info->i2c_client, 0x1011, 0x00);
      }
      else {
        sensor_write_reg(info->i2c_client, 0x1011, 0x01);//cpu reset
        sensor_write_reg(info->i2c_client, 0x941C, 0x04);
        sensor_write_reg(info->i2c_client, 0x9010, 0x01);
        sensor_write_reg(info->i2c_client, 0x9010, 0x00);
        sensor_write_reg(info->i2c_client, 0x1306, 0x00);//rear camera
        sensor_write_reg(info->i2c_client, 0x1011, 0x00);
      }
      msleep(100);
      sensor_write_reg(info->i2c_client, 0x7188, 0x01);//let AF windows work
      break;
    }
    case SENSOR_IOCTL_SET_COLOR_EFFECT:
    {
      u8 coloreffect;
      if (copy_from_user(&coloreffect,(const void __user *)arg,
        sizeof(coloreffect))) {
        return -EFAULT;
      }
            printk("SET_COLOR_EFFECT as %d\n", coloreffect);
            switch(coloreffect)
            {
                u16 val;
                case YUV_ColorEffect_None:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x00);//auto
                    break;
                case YUV_ColorEffect_Sepia:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x03);//sepia
                    break;
                case YUV_ColorEffect_Mono:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x04);//grayscale
                    break;
                case YUV_ColorEffect_Negative:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x02);//negative
                    break;
                case YUV_ColorEffect_Vivid:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x05);//vivid
                    break;
                case YUV_ColorEffect_WaterColor:
                    err = sensor_write_reg(info->i2c_client, 0x7102, 0x01);//aqua
                    break;
                default:
                    break;
            }

            if (err)
                return err;
      return 0;
    }
    case SENSOR_IOCTL_SET_WHITE_BALANCE:
    {
      u8 whitebalance;

      if (copy_from_user(&whitebalance,(const void __user *)arg,
        sizeof(whitebalance))) {
        return -EFAULT;
      }
      printk("SET_WHITE_BALANCE as %d\n", whitebalance);
            switch(whitebalance)
            {
                case YUV_Whitebalance_Auto:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x00);//auto
                    break;
                case YUV_Whitebalance_Incandescent:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x06);//Incandescent
                    break;
                case YUV_Whitebalance_Daylight:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x01);//Daylight
                    break;
                case YUV_Whitebalance_Fluorescent:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x05);//Fluorescent_H
                    break;
                case YUV_Whitebalance_CloudyDaylight:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x02);//Cloudy
                    break;
                case YUV_Whitebalance_WarmFluorescent:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x04);//Fluorescent_L
                    break;
                case YUV_Whitebalance_Shade:
                    err = sensor_write_reg(info->i2c_client, 0x710A, 0x03);//Shade
                    break;
                default:
                    break;
            }
            if (err)
                return err;
      return 0;
    }
    case SENSOR_CUSTOM_IOCTL_SET_SCENEMODE:
    {
            u8 scene_mode;
            if (copy_from_user(&scene_mode,(const void __user *)arg,
                    sizeof(scene_mode))) {
                return -EFAULT;
            }
            printk("SET_SCENEMODE as %d\n", scene_mode);
            switch(scene_mode)
            {
                u16 val;
                case YUV_SceneMode_Invalid:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x00);//need to confirm
                    break;
                case YUV_SceneMode_Auto:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x00);
                    break;
                case YUV_SceneMode_Portrait:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x0A);
                    break;
                case YUV_SceneMode_Landscape:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x06);//The same with vivid
                    break;
                case YUV_SceneMode_Sports:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x0C);
                    break;
                case YUV_SceneMode_Night:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x07);
                    break;
                case YUV_SceneMode_Sunset:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x0E);
                    break;
                case YUV_SceneMode_Snow:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x18);
                    break;
                case YUV_SceneMode_Party:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x09);
                    break;
                case YUV_SceneMode_BackLight:
                    err = sensor_write_reg(info->i2c_client, 0x7109, 0x16);
                    break;
                default:
                    break;
            }
            break;
    }

    case SENSOR_CUSTOM_IOCTL_SET_AF_MODE:
    {
      custom_af_cmd_package AF_cmd;
      //return 0;//Bill: disable AF temprarily
      if (copy_from_user(&AF_cmd,(const void __user *)arg,
        sizeof(AF_cmd)))
      {
        return -EFAULT;
      }
      switch(AF_cmd.cmd)
      {
        case AF_CMD_START:
        {
          u16 FW_Status = 1;
          u16 i = 0;
          pr_info("AF cmd start!\n");
          /*
          err = sensor_read_reg(info->i2c_client, 0x3029, &FW_Status);
          if (err)
            return err;
          while (FW_Status != 0 && FW_Status != 0x10 && FW_Status !=0x70) {
            if (i < 5)
              i++;
            else
              break;
            msleep(10);
            err = sensor_read_reg(info->i2c_client, 0x3029, &FW_Status);
            if (err)
              return err;
            pr_info("FW_Status is %x\n", FW_Status);
          }
          pr_info("FW_Status is %x\n", FW_Status);
          */
          //err = sensor_write_table(info->i2c_client, Autofocus_Trigger);
          if (touch_mode==TOUCH_STATUS_OFF) {
            err = sensor_write_reg(info->i2c_client, 0x7188, 0x01);//ROI on
            err = sensor_write_reg(info->i2c_client, 0x7140, 0x00);
            err = sensor_write_reg(info->i2c_client, 0x7141, 0xC0);//0x50
            err = sensor_write_reg(info->i2c_client, 0x7142, 0x01);
            err = sensor_write_reg(info->i2c_client, 0x7143, 0xA0);//0xD8
            err = sensor_write_reg(info->i2c_client, 0x7144, 0x01);
            err = sensor_write_reg(info->i2c_client, 0x7145, 0xA0);//0xD8
            err = sensor_write_reg(info->i2c_client, 0x7146, 0x01);
          }
          else
            err = sensor_write_reg(info->i2c_client, 0x7146, 0x01);
          if (err)
              return err;
          break;
        }
        case AF_CMD_ABORT:
        {
/*
          u16 FW_Status = 1;
          u16 MAIN_ACK = 1;
          u16 i = 0;

          err = sensor_write_table(info->i2c_client, Autofocus_Release);
          if (err)
            return err;
          //pr_info("fail to write i2c seq!");        //return NV_FALSE;
          for (i=0; i<10; i++) { //wait for ACK = 0
            sensor_read_reg(info->i2c_client, 0x3023, &MAIN_ACK);
            if (!MAIN_ACK) {
              //NvOdmImagerI2cRead(&pContext->I2c, 0x3022, &MAIN);
              //NvOsDebugPrintf("MAIN is %d\n", MAIN);
              //pr_info("ACK is %d\n", MAIN_ACK);
              sensor_read_reg(info->i2c_client, 0x3029, &FW_Status);
              pr_info("FW_Status is %x\n", FW_Status);
              break;
            }
            msleep(10);
          }
*/
          err = sensor_write_reg(info->i2c_client, 0x714F, 0x00);//release focus
          err = sensor_write_reg(info->i2c_client, 0x710E, 0x00);//Seems default AE window size
          touch_mode = TOUCH_STATUS_OFF;
          if (err)
            return err;
          break;
        }
        case AF_CMD_SET_POSITION:
        case AF_CMD_SET_WINDOW_POSITION:
        case AF_CMD_SET_WINDOW_SIZE:
        case AF_CMD_SET_AFMODE:
        case AF_CMD_SET_CAF:
        default:
          pr_info("AF cmd %d not implemented yet\n",AF_cmd.cmd);
          return -1;
      }
	return 0; //Bill
    }
    case SENSOR_CUSTOM_IOCTL_GET_AF_MODE:
    {
      custom_af_cmd_package AF_cmd;
      if (copy_from_user(&AF_cmd,(const void __user *)arg,
        sizeof(AF_cmd)))
      {
        return -EFAULT;
      }
      #if 0 //disable AF temporarily.
      AF_cmd.data = 1; //Locked
      copy_to_user((const void __user *)arg, &AF_cmd, sizeof(AF_cmd));
      return 0;
      #endif
      switch(AF_cmd.cmd)
      {
        case AF_CMD_GET_AF_STATUS:
        {
          u16 AF_status;
          u16 AF_result;
          sensor_read_reg(info->i2c_client, 0x72A0, &AF_status);
          if (AF_status) {
            pr_info("AF searching... %d\n", AF_status);
            AF_cmd.data = 0; //busy
            copy_to_user((const void __user *)arg, &AF_cmd, sizeof(AF_cmd));
            break;
          }
          sensor_read_reg(info->i2c_client, 0x72A1, &AF_result);
          pr_info("AF result... %d\n", AF_result);
          if (AF_result==0)
            AF_cmd.data = 1; //Locked
          else
            AF_cmd.data = 2; //failed to find
          //err = sensor_write_reg(info->i2c_client, 0x71EB, 0x01);//release AWB/AE lock
          copy_to_user((const void __user *)arg, &AF_cmd, sizeof(AF_cmd));
          pr_info("AF done and release AWB/AE lock... %d\n", AF_result);
            break;
        }
        default:
          pr_info("AF cmd %d not implemented yet\n",AF_cmd.cmd);
          return -1;
      }
      return 0;
    }
    case SENSOR_CUSTOM_IOCTL_SET_EV:
    {
      short ev;

      if (copy_from_user(&ev,(const void __user *)arg, sizeof(short)))
      {
        return -EFAULT;
      }

      printk("SET_EV as %d\n",ev);
      switch(ev)
            {
                case -6:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x0C);
                    break;
                case -5:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x0B);
                    break;
                case -4:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x0A);
                    break;
                case -3:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x09);
                    break;
                case -2:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x08);
                    break;
                case -1:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x07);
                    break;
                case 0:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x06);
                    break;
                case 1:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x05);
                    break;
                case 2:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x04);
                    break;
                case 3:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x03);
                    break;
                case 4:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x02);
                    break;
                case 5:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x01);
                    break;
                case 6:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x00);
                    break;
                default:
                    err = sensor_write_reg(info->i2c_client, 0x7103, 0x06);
                    break;
            }
            if (err)
                return err;
      return 0;
    }

	case SENSOR_CUSTOM_IOCTL_GET_EV:
	{
		int EV;
		u16 tmp;
		printk("SENSOR_CUSTOM_IOCTL_GET_EV\n");
		/* [0x72b4] will return a number from 12 to 0.
		 * It stands for "EV-2 ~ EV+2".
		 */

		sensor_read_reg(info->i2c_client, 0x72b4, &tmp);

		EV = 6 - tmp;
		printk("GET_EV: [0x72b4]:0x%X;\n", EV);

		if (copy_to_user((const void __user *)arg, &EV, sizeof(EV)))
		{
			return -EFAULT;
		}

		return 0;
	}
	case SENSOR_CUSTOM_IOCTL_GET_ISO:
	{
		short iso;
		u16 ISO_lowbyte = 0;
		u16 ISO_highbyte = 0;
		sensor_read_reg(info->i2c_client, 0x72b7, &ISO_lowbyte);
		sensor_read_reg(info->i2c_client, 0x72b8, &ISO_highbyte);

		iso = (ISO_highbyte << 8) | ISO_lowbyte;

		printk("GET_ISO as value:%d\n", iso);
		if (copy_to_user((const void __user *)arg, &iso, sizeof(short)))
			return -EFAULT;

		return 0;
	}
	case SENSOR_CUSTOM_IOCTL_GET_ET:
	{
		printk("SENSOR_CUSTOM_IOCTL_GET_ET \n");

		custom_et_value_package ET;
		u16 et_numerator = 0;
		u16 et_denominator_byte1 = 1;
		u16 et_denominator_byte2 = 0;
		u16 et_denominator_byte3 = 0;

		sensor_read_reg(info->i2c_client, 0x72b0, &et_numerator);
		sensor_read_reg(info->i2c_client, 0x72b1, &et_denominator_byte1);
		sensor_read_reg(info->i2c_client, 0x72b2, &et_denominator_byte2);
		sensor_read_reg(info->i2c_client, 0x72b3, &et_denominator_byte3);

		printk("GET_ET: [0x72b0]:0x%X; [0x72b1]:0x%X\n", et_numerator, et_denominator_byte1);
		printk("GET_ET: [0x72b2]:0x%X; [0x72b3]:0x%X\n", et_denominator_byte2, et_denominator_byte3);

		ET.exposure = et_numerator;
		ET.vts =  (et_denominator_byte3 << 16)|(et_denominator_byte2 << 8)|et_denominator_byte1;

		if (err) {
			printk("GET_ET: err= %d\n", err);
			return err;
		}

		if (copy_to_user((const void __user *)arg, &ET, sizeof(ET)))
		{
			return -EFAULT;
		}
		if (err)
			return err;

		return 0;
	}
    case SENSOR_CUSTOM_IOCTL_SET_TOUCH_AF:
    {
        custom_touch_af_cmd_package touch_af;
        u32 af_w, af_h, af_x, af_y;
        if (copy_from_user(&touch_af,(const void __user *)arg, sizeof(custom_touch_af_cmd_package))){
            return -EFAULT;
        }
/*
        if (!touch_focus_enable) {
		printk("%s: SENSOR_CUSTOM_IOCTL_SET_TOUCH_AF blocked\n", __func__);
		break;
        }
*/
        if(touch_af.zoom){
            touch_mode = TOUCH_STATUS_ON;
            af_w = touch_af.win_w;
            af_h = touch_af.win_h;
            //printk("SENSOR_CUSTOM_IOCTL_SET_TOUCH_AF: af_w:0x%x af_h:0x%x af_x:0x%x af_y:0x%x\n", touch_af.win_w, touch_af.win_h, touch_af.win_x, touch_af.win_y);
            af_x = touch_af.win_x;
            af_y = touch_af.win_y;
            printk("SENSOR_CUSTOM_IOCTL_SET_TOUCH_AF: af_w:0x%x af_h:0x%x af_x:0x%x af_y:0x%x\n", af_w, af_h, af_x, af_y);
            //AE window
            //err = sensor_write_reg(info->i2c_client, 0x7188, 0x01);//ROI on
            //err = sensor_write_reg(info->i2c_client, 0x7148, af_w>>8);
            //err = sensor_write_reg(info->i2c_client, 0x7149, af_w&0xff);
            //err = sensor_write_reg(info->i2c_client, 0x714A, af_x>>8);
            //err = sensor_write_reg(info->i2c_client, 0x714B, af_x&0xff);
            //err = sensor_write_reg(info->i2c_client, 0x714C, af_y>>8);
            //err = sensor_write_reg(info->i2c_client, 0x714D, af_y&0xff);
            //AF window
            err = sensor_write_reg(info->i2c_client, 0x7188, 0x01);//ROI on
            err = sensor_write_reg(info->i2c_client, 0x7140, af_w>>8);
            err = sensor_write_reg(info->i2c_client, 0x7141, af_w&0xff);
            err = sensor_write_reg(info->i2c_client, 0x7142, af_x>>8);
            err = sensor_write_reg(info->i2c_client, 0x7143, af_x&0xff);
            err = sensor_write_reg(info->i2c_client, 0x7144, af_y>>8);
            err = sensor_write_reg(info->i2c_client, 0x7145, af_y&0xff);
            //touch_focus_enable=0;
        }
        else{
            if(touch_mode != TOUCH_STATUS_OFF){
                touch_mode = TOUCH_STATUS_OFF;
                printk("SENSOR_CUSTOM_IOCTL_SET_TOUCH_AF: Cancel touch af\n");
                err = sensor_write_reg(info->i2c_client, 0x710E, 0x00);//Seems default AE window size
                err = sensor_write_reg(info->i2c_client, 0x714F, 0x00);//release focus
                /*
                if(!caf_mode){
                    fjm6mo_read_register(info->i2c_client, 0x0A, 0x03, 0x01, &buffer);
                    if(buffer == 0){
                        //Stop auto focus
                        fjm6mo_write_register(info->i2c_client, 1, 0x0A, 0x02, 0x0);
                        err = isp_interrupt(INT_STATUS_AF);
                        if(err)
                            pr_err("Touch af stop interrupt error");
                    }
                    fjm6mo_write_register(info->i2c_client, 1, 0x0A, 0x02, 0x3);
                }
                */
            }
        }
        break;
    }
    case SENSOR_CUSTOM_IOCTL_SET_ISO:
        {
            u8 iso;
            if (copy_from_user(&iso,(const void __user *)arg,
                    sizeof(iso))) {
                return -EFAULT;
            }
            printk("SET_ISO as %d\n", iso);
            switch(iso)
            {
                u16 val;
                case YUV_ISO_AUTO:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x00);
                    break;
                case YUV_ISO_50:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x01);
                    break;
                case YUV_ISO_100:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x02);
                    break;
                case YUV_ISO_200:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x03);
                    break;
                case YUV_ISO_400:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x04);
                    break;
                case YUV_ISO_800:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x05);
                    break;
                case YUV_ISO_1600:
                    err = sensor_write_reg(info->i2c_client, 0x7110, 0x06);
                    break;
                default:
                    break;
            }
            break;
        }
    case SENSOR_CUSTOM_IOCTL_SET_FLICKERING:
        {
            u8 flickering;
            if (copy_from_user(&flickering,(const void __user *)arg,
                    sizeof(flickering))) {
                return -EFAULT;
            }
            printk("SET_FLICKERING as %d\n", flickering);
            switch(flickering)
            {
                u16 val;
                //case YUV_ANTIBANGING_OFF:
                    //err = sensor_write_reg(info->i2c_client, 0x7101, );
                    //break;
                //case YUV_ANTIBANGING_AUTO:
                    //err = fjm6mo_write_register(info->i2c_client, 1, 0x03, 0x06, 0x0);
                    //break;
                case YUV_ANTIBANGING_50HZ:
                    err = sensor_write_reg(info->i2c_client, 0x7101, 0x01);
                    break;
                case YUV_ANTIBANGING_60HZ:
                    err = sensor_write_reg(info->i2c_client, 0x7101, 0x02);
                    break;
                default:
                    break;
            }
            break;
        }
    case SENSOR_CUSTOM_IOCTL_SET_CONTINUOUS_AF:
    {
        u8 continuous_af;
        if (copy_from_user(&continuous_af,(const void __user *)arg,
                sizeof(continuous_af))) {
            return -EFAULT;
        }
        printk("SET_CONTINUOUS_AF as %d\n", continuous_af);
        if(continuous_af==1){
            caf_mode = true;
            err = sensor_write_reg(info->i2c_client, 0x7105, 0x03);//CAF
        }
        else if (continuous_af==0 && focus_control==2){
            caf_mode = false;
            err = sensor_write_reg(info->i2c_client, 0x7105, 0x00);//auto
            if(err)
              pr_err("CAF stop error\n");
        }
        else if (continuous_af==0 && focus_control==0) {
            caf_mode = false;
            err = sensor_write_reg(info->i2c_client, 0x7105, 0x02);//infinity
            if(err)
              pr_err("Infinity focus error\n");
        }
        if (err)
          return err;
        return 0;
    }
    case SENSOR_CUSTOM_IOCTL_SET_AE_LOCK:
    {
            u32 ae_lock;
            if (copy_from_user(&ae_lock,(const void __user *)arg,
                    sizeof(ae_lock))) {
                return -EFAULT;
            }
            //ae_mode = ae_lock;
            printk("SET_AE_LOCK as 0x%x\n", ae_lock);
            if (ae_lock==1) {
              sensor_write_reg(info->i2c_client, 0x71E4, 0x04);//AE command
              sensor_write_reg(info->i2c_client, 0x71E5, 0x01);//AE lock
              sensor_write_reg(info->i2c_client, 0x71E8, 0x01);
            }
            else if (ae_lock==0) {
              sensor_write_reg(info->i2c_client, 0x71E4, 0x04);//AE command
              sensor_write_reg(info->i2c_client, 0x71E5, 0x02);//AE unlock
              sensor_write_reg(info->i2c_client, 0x71E8, 0x01);
            }
            break;
    }
    case SENSOR_CUSTOM_IOCTL_SET_AWB_LOCK:
    {
            u32 awb_lock;
            if (copy_from_user(&awb_lock,(const void __user *)arg,
                    sizeof(awb_lock))) {
                return -EFAULT;
            }
            //awb_mode = awb_lock;
            printk("SET_AWB_LOCK as 0x%x\n", awb_lock);
            if (awb_lock==1) {
              sensor_write_reg(info->i2c_client, 0x71E4, 0x05);//AWB command
              sensor_write_reg(info->i2c_client, 0x71E5, 0x01);//AWB lock
              sensor_write_reg(info->i2c_client, 0x71E8, 0x01);
            }
            else if (awb_lock==0) {
              sensor_write_reg(info->i2c_client, 0x71E4, 0x05);//AWB command
              sensor_write_reg(info->i2c_client, 0x71E5, 0x02);//AWB unlock
              sensor_write_reg(info->i2c_client, 0x71E8, 0x01);
            }
            break;
    }
    case SENSOR_CUSTOM_IOCTL_SET_AF_CONTROL:
    {
              //int focus_mode;
       if (copy_from_user(&focus_control,(const void __user *)arg,
               sizeof(focus_control))) {
           return -EFAULT;
       }
       return 0;
    }
	case SENSOR_CUSTOM_IOCTL_FW_UPDATE_PROGRAM:
	{
		custom_fw_update_rom_package rom_cmd;
		printk("%s(FW_UPDATE_PROGRAM)++\n", __FUNCTION__);
		if (copy_from_user(&rom_cmd,(const void __user *)arg, sizeof(rom_cmd))) {
			fw_update_status = ICATCH_FW_UPDATE_FAILED;
			return -EFAULT;
		}
		printk("binfile_path=%s; cmd=%d; flash_rom_start_address=%d; program_size= %d\n",
			rom_cmd.binfile_path, rom_cmd.cmd, rom_cmd.flash_rom_start_address, rom_cmd.program_size);

		i7002a_isp_on(1);

		printk("%s: BB_WrSPIFlash()++\n", __FUNCTION__);
		BB_WrSPIFlash(rom_cmd.binfile_path);
		printk("%s: BB_WrSPIFlash()--\n", __FUNCTION__);

		i7002a_isp_on(0);

		if(fw_update_status != ICATCH_FW_UPDATE_SUCCESS){
			printk("i7002a Update FAIL: %d\n", -fw_update_status);
			return -fw_update_status;
		}

		return 0;
	}

/*
    case SENSOR_CUSTOM_IOCTL_SET_AE_METER_WINDOW:
    {

      custom_window_package  win;

      struct sensor_reg_2 AE_meter_window[10] =
      {
        {SENSOR_BYTE_WRITE, 0x5680, 0x00}, //  [3:0] = XStart [11:8]
        {SENSOR_BYTE_WRITE, 0x5681, 0x00}, //  [7:0] = XStart [7:0]
        {SENSOR_BYTE_WRITE, 0x5682, 0x00}, //  [2:0] = YStart [10:8]
        {SENSOR_BYTE_WRITE, 0x5683, 0x00}, //  [7:0] = YStart [7:0]
        {SENSOR_BYTE_WRITE, 0x5684, 0x10}, //  [3:0] = XEnd [11:8]
        {SENSOR_BYTE_WRITE, 0x5685, 0xA0}, //  [7:0] = XEnd [7:0]
        {SENSOR_BYTE_WRITE, 0x5686, 0x0C}, //  [2:0] = YEnd [10:8]
        {SENSOR_BYTE_WRITE, 0x5687, 0x78}, //  [7:0] = YEnd [7:0]
        {SENSOR_MASK_BYTE_WRITE, 0x501D, 0x00, 0x10}, //  [4] = Enable Manual
        {SENSOR_TABLE_END, 0x0000}
      };

      printk("SENSOR_CUSTOM_IOCTL_SET_AE_METER_WINDOW\n");
      if (copy_from_user(&win,(const void __user *)arg, sizeof(custom_window_package)))
      {
        return -EFAULT;
      }

      printk("SET_AE_METER_WINDOW as (%d,%d)-(%d,%d)\n",win.XStart,win.YStart,win.XEnd,win.YEnd);

      if (win.XStart == 0 && win.YStart == 0 && win.XEnd==0 && win.YEnd==0)
      {
      }
      else
      {
        AE_meter_window[0].val= (win.XStart & 0xF00) >> 8;
        AE_meter_window[1].val= (win.XStart & 0xFF);
        AE_meter_window[2].val= (win.YStart & 0x700) >> 8;
        AE_meter_window[3].val= (win.YStart & 0xFF);
        AE_meter_window[4].val= (win.XEnd & 0xF00) >> 8;
        AE_meter_window[5].val= (win.XEnd & 0xFF);
        AE_meter_window[6].val= (win.YEnd & 0x700) >> 8;
        AE_meter_window[7].val= (win.YEnd & 0xFF);
        AE_meter_window[8].val= 0x10;
      }
      err=sensor_write_table_2(info->i2c_client, AE_meter_window);

      if (err)
        return err;

      return 0;
    }
//0715Bill
    case SENSOR_CUSTOM_IOCTL_SET_AF_WINDOW_POS:
    {

      custom_af_pos_package  af_pos;

      u16 CMD_ACK = 1;
      u16 CMD_MAIN = 1;
      u16 i = 1;
      struct sensor_reg AF_window_pos[11] =
      {
        {0x3024, 0x28}, //  X 0~80 (0x00~0x50)
        {SENSOR_WAIT_MS, 0x0A},
        {0x3025, 0x1e}, //  Y 0~60 (0x00~0x3c)
        {SENSOR_WAIT_MS, 0x0A},
        {0x3026, 0x08}, //  W
        {SENSOR_WAIT_MS, 0x0A},
        {0x3027, 0x08}, //  H
        {SENSOR_WAIT_MS, 0x0A},
        {0x3023, 0x01}, //  CMD_ACK
        {0x3022, 0x90}, //  CMD_MAIN
        {SENSOR_TABLE_END, 0x0000}
      };
      struct sensor_reg Launch_Custom_AF[3] =
      {
        {0x3023, 0x01}, //  CMD_ACK
        {0x3022, 0x9f}, //  CMD_MAIN
        {SENSOR_TABLE_END, 0x0000},
      };

      printk("SENSOR_CUSTOM_IOCTL_SET_AF_WINDOW_POS\n");
      if (copy_from_user(&af_pos,(const void __user *)arg, sizeof(custom_af_pos_package)))
      {
        return -EFAULT;
      }

      printk("SET_AF_WINDOW_POS as x:(%d), y:(%d)\n", af_pos.focusX, af_pos.focusY);
      if (af_pos.focusX == -1 && af_pos.focusY == -1)
      {
      }
      else
      {
        af_pos.focusX = af_pos.focusX*2/25;//1000:80
        af_pos.focusY = af_pos.focusY*3/50;// 1000:60
        if (af_pos.focusX < 0x08)
		af_pos.focusX = 0x08;
        if (af_pos.focusX > 0x48)
		af_pos.focusX = 0x48;
        if (af_pos.focusY < 0x08)
		af_pos.focusY = 0x08;
        if (af_pos.focusY > 0x34)
		af_pos.focusY = 0x34;
        AF_window_pos[0].val= (af_pos.focusX & 0xff);
        AF_window_pos[2].val= (af_pos.focusY & 0xff);
        //AF_window_pos[2].val= (win.YStart & 0x700) >> 8;
        //AF_window_pos[3].val= (win.YStart & 0xFF);
        //AF_window_pos[4].val= (win.XEnd & 0xF00) >> 8;
        //AF_window_pos[5].val= (win.XEnd & 0xFF);
        //AF_window_pos[6].val= (win.YEnd & 0x700) >> 8;
        printk("Transform: SET_AF_WINDOW_POS as x:(%d:%d), y:(%d:%d)\n ", af_pos.focusX, AF_window_pos[0].val, af_pos.focusY, AF_window_pos[2].val);
      }
      err=sensor_write_table(info->i2c_client, AF_window_pos);
      for (i=0; i<20; i++) { //wait for ACK = 0
            sensor_read_reg(info->i2c_client, 0x3023, &CMD_ACK);
            sensor_read_reg(info->i2c_client, 0x3022, &CMD_MAIN);
            if (!CMD_ACK) {
              //NvOdmImagerI2cRead(&pContext->I2c, 0x3022, &MAIN);
              //NvOsDebugPrintf("MAIN is %d\n", MAIN);
              //pr_info("ACK is %d\n", MAIN_ACK);
              //sensor_read_reg(info->i2c_client, 0x3029, &FW_Status);
              printk("CMD_ACK is %x\n", CMD_ACK);
	printk("CMD_MAIN is %x\n", CMD_MAIN);
              break;
            }
            msleep(10);
            printk("CMD_ACK is 0x%x CMD_MAIN is 0x%x\n", CMD_ACK, CMD_MAIN);
      }
      err=sensor_write_table(info->i2c_client, Launch_Custom_AF);
      for (i=0; i<20; i++) { //wait for ACK = 0
            sensor_read_reg(info->i2c_client, 0x3023, &CMD_ACK);
            sensor_read_reg(info->i2c_client, 0x3022, &CMD_MAIN);
            if (!CMD_ACK) {
              //NvOdmImagerI2cRead(&pContext->I2c, 0x3022, &MAIN);
              //NvOsDebugPrintf("MAIN is %d\n", MAIN);
              //pr_info("ACK is %d\n", MAIN_ACK);
              //sensor_read_reg(info->i2c_client, 0x3029, &FW_Status);
              printk("CMD_ACK is %x\n", CMD_ACK);
	printk("CMD_MAIN is %x\n", CMD_MAIN);
              break;
            }
            msleep(10);
            printk("CMD_ACK is 0x%x CMD_MAIN is 0x%x\n", CMD_ACK, CMD_MAIN);
      }

      if (err)
        return err;

      return 0;
    }
*/
    default:
      return -EINVAL;
    }
    return 0;
}

static int sensor_open(struct inode *inode, struct file *file)
{
	int ret;

	pr_info("yuv %s\n",__func__);
	file->private_data = info;
	if (info->pdata && info->pdata->power_on)
		ret = info->pdata->power_on();
	if (ret == 0) {
		sensor_opened = true;
		first_open = true;
	}
	else
		sensor_opened = false;
	msleep(20);
	return 0;
}

int iCatch7002a_sensor_release(struct inode *inode, struct file *file)
{
	printk("%s()++\n", __FUNCTION__);
	if (sensor_opened == true) {
		if (info->pdata && info->pdata->power_off) {
			info->pdata->power_off();
			sensor_opened = false;
		}
	} else
		printk("%s No action. Power is already off.\n", __FUNCTION__);
	file->private_data = NULL;
#ifdef CAM_TWO_MODE
  g_initialized_1280_960=0;
  g_initialized_1080p=0;
#endif
	printk("%s()--\n", __FUNCTION__);
	return 0;
}


static const struct file_operations sensor_fileops = {
	.owner = THIS_MODULE,
	.open = sensor_open,
	.unlocked_ioctl = sensor_ioctl,
	.release = iCatch7002a_sensor_release,
};

static struct miscdevice sensor_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = ICATCH7002A_SENSOR_NAME,
	.fops = &sensor_fileops,
};

static ssize_t i7002a_switch_name(struct switch_dev *sdev, char *buf)
{
	printk("%s: version_num_in_isp=0x%X\n", __FUNCTION__, version_num_in_isp);

	if(tegra3_get_project_id() == TEGRA3_PROJECT_TF300T)
		return sprintf(buf, "TF300T-%06X\n", version_num_in_isp);
	else if (tegra3_get_project_id() == TEGRA3_PROJECT_TF300TG)
		return sprintf(buf, "TF300TG-%06X\n", version_num_in_isp);
	else if (tegra3_get_project_id() == TEGRA3_PROJECT_TF300TL)
		return sprintf(buf, "TF300TL-%06X\n", version_num_in_isp);
	else
		return sprintf(buf, "Unknown-%06X\n", version_num_in_isp);
}

static ssize_t i7002a_switch_state(struct switch_dev *sdev, char *buf)
{
    return sprintf(buf, "%d\n", fw_update_status);
}

static int sensor_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int err=0;

	pr_info("yuv %s, compiled at %s %s\n",__func__,__DATE__,__TIME__);

	info = kzalloc(sizeof(struct sensor_info), GFP_KERNEL);

	if (!info) {
		pr_err("yuv_sensor : Unable to allocate memory!\n");
		return -ENOMEM;
	}

#ifndef _CAM_SENSOR_DETECT_
  err = misc_register(&sensor_device);
	if (err) {
		pr_err("yuv_sensor : Unable to register misc device!\n");
		kfree(info);
		return err;
	}
#endif
	info->pdata = client->dev.platform_data;
	info->i2c_client = client;
#ifdef _CAM_SENSOR_DETECT_
	info->dev = client->dev;
#endif

	i2c_set_clientdata(client, info);

	/* Query fw version number in ISP. */
	i7002a_isp_on(1);
	front_chip_id = i7002a_get_sensor_id(1);
	if (front_chip_id == SENSOR_ID_MI1040) {
		u16 tmp;
		sensor_write_table(info->i2c_client, query_mi1040_output_format_seq);
		sensor_read_reg(info->i2c_client, 0x9111, &tmp);
		mi1040_output_format = tmp;
		printk("mi1040 output format= %d\n", mi1040_output_format);
	}
	version_num_in_isp = get_fw_version_in_isp();


	i7002a_sdev.name = I7002A_SDEV_NAME;
	i7002a_sdev.print_name = i7002a_switch_name;
	i7002a_sdev.print_state = i7002a_switch_state;
	if(switch_dev_register(&i7002a_sdev) < 0){
		pr_err("switch_dev_register for camera failed!\n");
	}
	switch_set_state(&i7002a_sdev, 0);

	pr_info("i7002a check version number: 0x%x\n", version_num_in_isp);
	pr_info("i7002a front_chip_id: 0x%X\n", front_chip_id);

	i7002a_isp_on(0);

	return 0;
}

static int sensor_remove(struct i2c_client *client)
{
	struct sensor_info *info;

	pr_info("yuv %s\n",__func__);
	info = i2c_get_clientdata(client);
	misc_deregister(&sensor_device);
	kfree(info);
	return 0;
}

static const struct i2c_device_id sensor_id[] = {
	{ ICATCH7002A_SENSOR_NAME, 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_i2c_driver = {
	.driver = {
		.name = ICATCH7002A_SENSOR_NAME,
		.owner = THIS_MODULE,
	},
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};

#ifdef _CAM_SENSOR_DETECT_
int tegra_camera_set_caminfo(int num, int on);

int __init iCatch7002a_late_init(void)
{
	int ret=-EINVAL;
	u16 temp;
	struct clk *csi_clk=NULL;
	struct clk *csus_clk=NULL;
	struct clk *sensor_clk=NULL;
	struct regulator *Tegra_camera_regulator_csi=NULL;

	pr_err("%s: entry point\n", __func__);

	if (!info || !info->pdata || !info->pdata->power_on)
		goto fail;

	info->pdata->power_on();

	csi_clk = clk_get(NULL, "csi");
	if (IS_ERR_OR_NULL(csi_clk)) {
		pr_err("%s: Couldn't get csi clock\n", __func__);
		csi_clk=NULL;
		goto fail;
	}

	csus_clk = clk_get(NULL, "csus");
	if (IS_ERR_OR_NULL(csus_clk)) {
		pr_err("Couldn't get csus clock\n");
		csus_clk=NULL;
		goto fail;
	}
	sensor_clk = clk_get(NULL, "vi_sensor");
	if (IS_ERR_OR_NULL(sensor_clk)) {
		pr_err("Couldn't get csus clock\n");
		sensor_clk=NULL;
		goto fail;
	}

	msleep(10);
	clk_enable(csus_clk);
	clk_enable(sensor_clk);

	ret = sensor_read_reg(info->i2c_client, 0x300A, &temp);
	if (ret)
  {
	ret = sensor_read_reg(info->i2c_client, 0x300A, &temp);
  }

  if (ret)
    printk("failed to detect iCatch7002a ISP!\n");
  else
  {
    printk("read ID as 0x%x",temp);
    misc_register(&sensor_device);
    tegra_camera_set_caminfo(0,1);
  }
	clk_disable(csi_clk);
	clk_disable(csus_clk);
	clk_disable(sensor_clk);

	ret = 0;
fail:
	if(csus_clk)
		clk_put(csus_clk);
	if(csi_clk)
		clk_put(csi_clk);
	if(sensor_clk)
		clk_put(sensor_clk);
	if(Tegra_camera_regulator_csi)
		regulator_put(Tegra_camera_regulator_csi);

	info->pdata->power_off();

	return ret;
}

late_initcall(iCatch7002a_late_init);
#endif
static int __init sensor_init(void)
{
	if ((tegra3_get_project_id() == TEGRA3_PROJECT_TF300T) ||
			(tegra3_get_project_id() == TEGRA3_PROJECT_TF300TG) ||
			(tegra3_get_project_id() == TEGRA3_PROJECT_TF300TL)) {
		pr_info("i7002a %s\n",__func__);
		return i2c_add_driver(&sensor_i2c_driver);
	}
	return 0;
}

static void __exit sensor_exit(void)
{
	if ((tegra3_get_project_id() == TEGRA3_PROJECT_TF300T) ||
			(tegra3_get_project_id() == TEGRA3_PROJECT_TF300TG) ||
			(tegra3_get_project_id() == TEGRA3_PROJECT_TF300TL)) {
		pr_info("i7002a %s\n",__func__);
		i2c_del_driver(&sensor_i2c_driver);
	}
}

module_init(sensor_init);
module_exit(sensor_exit);



#define CONFIG_I2C_READ_WRITE
#ifdef CONFIG_I2C_READ_WRITE
#include <linux/debugfs.h>
#include <linux/uaccess.h>
//#include <stdio.h>
#define DBG_TXT_BUF_SIZE 256
static char debugTxtBuf[DBG_TXT_BUF_SIZE];
//static u32 i2c_set_value;
static u32 i2c_get_value;

static ssize_t i2c_set_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t i2c_get_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_iCatch7002a_chip_power_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

#if 1
static ssize_t dbg_i7002a_fw_in_isp_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_i7002a_fw_in_isp_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	int tot = 0;
	char debug_buf[1024];
	int dlen = sizeof(debug_buf);
	char *bp = debug_buf;

	printk("%s: buf=%p, count=%d, ppos=%p; *ppos= %d\n", __FUNCTION__, buf, count, ppos, *ppos);

	if (*ppos)
		return 0;	/* the end */

	/* [Project id]-[FrontSensor]-[FW Version]*/
	if (front_chip_id == SENSOR_ID_OV2720) {
		len = snprintf(bp, dlen, "%02X-%02X-%06X\n", tegra3_get_project_id(), 1, version_num_in_isp);
		tot += len; bp += len; dlen -= len;
	} else if (front_chip_id == SENSOR_ID_MI1040){
		/* mi1040 chip_id= 0x2481 */
		len = snprintf(bp, dlen, "%02X-%02X-%06X\n", tegra3_get_project_id(), 2, version_num_in_isp);
		tot += len; bp += len; dlen -= len;
	} else {
		len = snprintf(bp, dlen, "%02X-%02X-%06X\n", tegra3_get_project_id(), 0, version_num_in_isp);
		tot += len; bp += len; dlen -= len;
	}

	if (copy_to_user(buf, debug_buf, tot))
		return -EFAULT;
	if (tot < 0)
		return 0;
	*ppos += tot;	/* increase offset */
	return tot;
}
#endif

static ssize_t dbg_i7002a_page_dump_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_i7002a_page_dump_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	int tot = 0;
	char debug_buf[1024];
	int dlen = sizeof(debug_buf);
	char *bp = debug_buf;
	int i =0;
	u8 mypage[0x100];

	printk("%s: buf=%p, count=%d, ppos=%p; *ppos= %d\n", __FUNCTION__, buf, count, ppos, *ppos);

	if (*ppos)
		return 0;	/* the end */

	i7002a_isp_on(1);

	len = snprintf(bp, dlen, "page_index=%d (0x%X)\n", dbg_i7002a_page_index, dbg_i7002a_page_index);
	tot += len; bp += len; dlen -= len;

	get_one_page_from_i7002a(dbg_i7002a_page_index, mypage);
	for(i=0; i < 0x100; i++) {
		if(i%16 == 0) {
			len = snprintf(bp, dlen, "[%03X] ", i);
			tot += len; bp += len; dlen -= len;
		}
		len = snprintf(bp, dlen, "%02X ", mypage[i]);
		tot += len; bp += len; dlen -= len;

		if(i%16 == 15) {
			len = snprintf(bp, dlen, "\n");
			tot += len; bp += len; dlen -= len;
		}
	}

	i7002a_isp_on(0);

	if (copy_to_user(buf, debug_buf, tot))
		return -EFAULT;
	if (tot < 0)
		return 0;
	*ppos += tot;	/* increase offset */
	return tot;
}

static ssize_t dbg_fw_update_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_fw_update_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	int tot = 0;
	char debug_buf[512];
	int dlen = sizeof(debug_buf);
	char *bp = debug_buf;

	printk("%s: buf=%p, count=%d, ppos=%p; *ppos= %d\n", __FUNCTION__, buf, count, ppos, *ppos);

	if (*ppos)
		return 0;	/* the end */

	switch(fw_update_status) {
	case ICATCH_FW_NO_CMD:
		len = snprintf(bp, dlen, "Never issue fw update cmd yet.\n");
		tot += len; bp += len; dlen -= len;
		break;

	case ICATCH_FW_IS_BURNING:
		if ((page_count >= 0) && (page_count <= total_page_count)) {
			int time_left = 0;
			if (flash_type == ICATCH_FLASH_TYPE_ST)
				time_left = page_count * 8 / 100;
			else
				time_left = page_count / 4;

			len = snprintf(bp, dlen, "FW update progress: %d/%d; Timeleft= %d secs\n", total_page_count - page_count + 1, total_page_count, time_left);
			tot += len; bp += len; dlen -= len;
		} else {
			len = snprintf(bp, dlen, "page_count=%d; total=%d\n", page_count, total_page_count);
			tot += len; bp += len; dlen -= len;
		}
		break;

	case ICATCH_FW_UPDATE_SUCCESS:
		len = snprintf(bp, dlen, "FW Update Complete!\n");
		tot += len; bp += len; dlen -= len;
		break;

	case ICATCH_FW_UPDATE_FAILED:
		len = snprintf(bp, dlen, "FW Update FAIL!\n");
		tot += len; bp += len; dlen -= len;
		break;

	default:
		len = snprintf(bp, dlen, "FW Update Status Unknown: %d\n", fw_update_status);
		tot += len; bp += len; dlen -= len;
	}

	if (copy_to_user(buf, debug_buf, tot))
		return -EFAULT;
	if (tot < 0)
		return 0;
	*ppos += tot;	/* increase offset */
	return tot;
}

static int dbg_fw_update_write(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	char debug_buf[256];
	int cnt;
	char bin_path[80];

	printk("%s: buf=%p, count=%d, ppos=%p\n", __FUNCTION__, buf, count, ppos);
	if (count > sizeof(debug_buf))
		return -EFAULT;
	if (copy_from_user(debug_buf, buf, count))
		return -EFAULT;

	debug_buf[count] = '\0';	/* end of string */
	cnt = sscanf(debug_buf, "%s", bin_path);

	/* Turn on the power & clock. */
	i7002a_isp_on(1);

	/* burning */
	printk("%s: BB_WrSPIFlash()++\n", __FUNCTION__);
	BB_WrSPIFlash(bin_path);
	printk("%s: BB_WrSPIFlash()--\n", __FUNCTION__);

	/* Turn off the clock & power. */
	i7002a_isp_on(0);

	return count;
}

static int i2c_set_write(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
  int len;
  int arg[2];
  //int gpio, set;

  //char gpioname[8];

//  printk("%s: buf=%p, count=%d, ppos=%p\n", __FUNCTION__, buf, count, ppos);
  arg[0]=0;

	if (*ppos)
		return 0;	/* the end */

//+ parsing......
  len=(count > DBG_TXT_BUF_SIZE-1)?(DBG_TXT_BUF_SIZE-1):(count);
  if (copy_from_user(debugTxtBuf,buf,len))
		return -EFAULT;

  debugTxtBuf[len]=0; //add string end

  sscanf(debugTxtBuf, "%x %x", &arg[0], &arg[1]);
  printk("argument is arg1=0x%x arg2=0x%x\n",arg[0], arg[1]);


  *ppos=len;
  sensor_write_reg(info->i2c_client, arg[0], arg[1]);

	return len;	/* the end */
}
/*
static ssize_t i2c_config_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{

}
*/

static int i2c_get_write(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
  int len;
  int arg = 0;
  //int gpio, set;

  //char gpioname[8];

//  printk("%s: buf=%p, count=%d, ppos=%p\n", __FUNCTION__, buf, count, ppos);


	if (*ppos)
		return 0;	/* the end */

//+ parsing......
  len=(count > DBG_TXT_BUF_SIZE-1)?(DBG_TXT_BUF_SIZE-1):(count);
  if (copy_from_user(debugTxtBuf,buf,len))
		return -EFAULT;

  debugTxtBuf[len]=0; //add string end

  sscanf(debugTxtBuf, "%x", &arg);
  printk("argument is arg=0x%x\n",arg);


  *ppos=len;
  sensor_read_reg(info->i2c_client, arg, &i2c_get_value);

	return len;	/* the end */
}

static ssize_t i2c_get_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	char *bp = debugTxtBuf;

       if (*ppos)
		return 0;	/* the end */
	len = snprintf(bp, DBG_TXT_BUF_SIZE, "the value is 0x%x\n", i2c_get_value);

	if (copy_to_user(buf, debugTxtBuf, len))
		return -EFAULT;
       *ppos += len;
	return len;

}

static ssize_t dbg_iCatch7002a_vga_status_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_iCatch7002a_vga_status_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	int tot = 0;
	char debug_buf[1024];
	int dlen = sizeof(debug_buf);
	char *bp = debug_buf;

	u16 chip_id, tmp = 0x0;

	printk("%s: buf=%p, count=%d, ppos=%p; *ppos= %d\n", __FUNCTION__, buf, count, ppos, *ppos);

	if (*ppos)
		return 0;	/* the end */

	if (sensor_opened == false) {
		if (info->pdata && info->pdata->power_on) {
			info->pdata->power_on();
			tegra_camera_mclk_on_off(1);
			msleep(100);
		} else {
			len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_on.\n");
			tot += len; bp += len; dlen -= len;
		}
	}

	/*Start - Power on sensor & enable clock - Front I2C (OV2720)*/
	sensor_write_reg(info->i2c_client, 0x0084, 0x14); /* To sensor clock divider */
	sensor_write_reg(info->i2c_client, 0x0034, 0xFF); /* Turn on all clock */
	sensor_write_reg(info->i2c_client, 0x9030, 0x3f);
	sensor_write_reg(info->i2c_client, 0x9031, 0x04);
	sensor_write_reg(info->i2c_client, 0x9034, 0xf3);
	sensor_write_reg(info->i2c_client, 0x9035, 0x04);

	sensor_write_reg(info->i2c_client, 0x9032, 0x02);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9032, 0x00);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9033, 0x00);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9033, 0x04);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9034, 0xf2);
	/*End - Power on sensor & enable clock */

	sensor_write_reg(info->i2c_client, 0x9008, 0x00); /* Need to check with vincent */
	sensor_write_reg(info->i2c_client, 0x9009, 0x00);
	sensor_write_reg(info->i2c_client, 0x900A, 0x00);
	sensor_write_reg(info->i2c_client, 0x900B, 0x00);

	/*Start - I2C Read*/
	sensor_write_reg(info->i2c_client, 0x9138, 0x30); /* Sub address enable */
	sensor_write_reg(info->i2c_client, 0x9140, 0x6C); /* Slave address      */
	sensor_write_reg(info->i2c_client, 0x9100, 0x03); /* Read mode          */
	sensor_write_reg(info->i2c_client, 0x9110, 0x30); /* Register addr MSB  */
	sensor_write_reg(info->i2c_client, 0x9112, 0x0a); /* Register addr LSB  */
	sensor_write_reg(info->i2c_client, 0x9104, 0x01); /* Trigger I2C read   */

	msleep(10);
	sensor_read_reg(info->i2c_client, 0x9111, &tmp);

	//printk("0x%x\n", tmp);
	chip_id = (tmp << 8) & 0xFF00;

	sensor_write_reg(info->i2c_client, 0x9110, 0x30); /* Register addr MSB  */
	sensor_write_reg(info->i2c_client, 0x9112, 0x0b); /* Register addr LSB  */
	sensor_write_reg(info->i2c_client, 0x9104, 0x01); /* Trigger I2C read   */

	msleep(10);
	sensor_read_reg(info->i2c_client, 0x9111, &tmp);
	//printk("0x%x\n", tmp);
	chip_id = chip_id  | (tmp & 0xFF);

	if (chip_id == SENSOR_ID_OV2720) {
		len = snprintf(bp, dlen, "1\n");
		tot += len; bp += len; dlen -= len;
	} else {
#if 0
		len = snprintf(bp, dlen, "back chip_id= 0x%x\n", chip_id);
		tot += len; bp += len; dlen -= len;
#endif
		/* Check if mi1040 is available. */
		sensor_write_table(info->i2c_client, query_mi1040_id_msb_seq);
		sensor_read_reg(info->i2c_client, 0x9111, &tmp);

		chip_id = (tmp << 8) & 0xFF00;

		sensor_write_table(info->i2c_client, query_mi1040_id_lsb_seq);
		sensor_read_reg(info->i2c_client, 0x9111, &tmp);
		chip_id = chip_id  | (tmp & 0xFF);

		printk("0x%x\n", chip_id);

		if (chip_id == SENSOR_ID_MI1040) {
			/* mi1040 chip_id= 0x2481 */
			len = snprintf(bp, dlen, "1\n");
			tot += len; bp += len; dlen -= len;
		} else {
			len = snprintf(bp, dlen, "0\n");
			tot += len; bp += len; dlen -= len;
		}
	}

	if (sensor_opened == false) {
		if (info->pdata && info->pdata->power_off) {
			tegra_camera_mclk_on_off(0);
			info->pdata->power_off();
		} else {
			len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_off.\n");
			tot += len; bp += len; dlen -= len;
		}
	}

	if (copy_to_user(buf, debug_buf, tot))
		return -EFAULT;
	if (tot < 0)
		return 0;
	*ppos += tot;	/* increase offset */
	return tot;
}


static ssize_t dbg_iCatch7002a_camera_status_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t dbg_iCatch7002a_camera_status_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	int len = 0;
	int tot = 0;
	char debug_buf[256];
	int dlen = sizeof(debug_buf);
	char *bp = debug_buf;

	u16 chip_id, tmp = 0x0;

	printk("%s: buf=%p, count=%d, ppos=%p; *ppos= %d\n", __FUNCTION__, buf, count, ppos, *ppos);

	if (*ppos)
		return 0;	/* the end */

	if (sensor_opened == false) {
		if (info->pdata && info->pdata->power_on) {
			info->pdata->power_on();
			tegra_camera_mclk_on_off(1);
			msleep(100);
		} else {
			len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_on.\n");
			tot += len; bp += len; dlen -= len;
		}
	}
	/* SONY IMX175 */
	sensor_write_reg(info->i2c_client, 0x0084, 0x14); /* To sensor clock divider */
	sensor_write_reg(info->i2c_client, 0x0034, 0xFF); /* Turn on all clock */
	sensor_write_reg(info->i2c_client, 0x9030, 0x3f);
	sensor_write_reg(info->i2c_client, 0x9031, 0x04);
	sensor_write_reg(info->i2c_client, 0x9034, 0xf2);
	sensor_write_reg(info->i2c_client, 0x9035, 0x04);
	sensor_write_reg(info->i2c_client, 0x9032, 0x00);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9032, 0x20);
	msleep(10);
	sensor_write_reg(info->i2c_client, 0x9032, 0x30);
	msleep(10);
	/*End - Power on sensor & enable clock */
	sensor_write_reg(info->i2c_client, 0x9008, 0x00); /* Need to check with vincent */
	sensor_write_reg(info->i2c_client, 0x9009, 0x00);
	sensor_write_reg(info->i2c_client, 0x900A, 0x00);
	sensor_write_reg(info->i2c_client, 0x900B, 0x00);

	/*Start - I2C Read*/
	sensor_write_reg(info->i2c_client, 0x9238, 0x30); /* Sub address enable */
	sensor_write_reg(info->i2c_client, 0x9240, 0x20); /* Slave address      */
	sensor_write_reg(info->i2c_client, 0x9200, 0x03); /* Read mode          */
	sensor_write_reg(info->i2c_client, 0x9210, 0x00); /* Register addr MSB  */
	sensor_write_reg(info->i2c_client, 0x9212, 0x00); /* Register addr LSB  */
	sensor_write_reg(info->i2c_client, 0x9204, 0x01); /* Trigger I2C read   */

	msleep(10);
	sensor_read_reg(info->i2c_client, 0x9211, &tmp);
	// printk("0x%x\n", tmp);
	chip_id = (tmp << 8) & 0xFF00;

	sensor_write_reg(info->i2c_client, 0x9210, 0x00); /* Register addr MSB  */
	sensor_write_reg(info->i2c_client, 0x9212, 0x01); /* Register addr LSB  */
	sensor_write_reg(info->i2c_client, 0x9204, 0x01); /* Trigger I2C read   */

	msleep(10);
	sensor_read_reg(info->i2c_client, 0x9211, &tmp);
	// printk("0x%x\n", tmp);
	chip_id = chip_id  | (tmp & 0xFF);

	if (chip_id == SENSOR_ID_IMX175) {
		len = snprintf(bp, dlen, "1\n");
		tot += len; bp += len; dlen -= len;
	} else {
#if 0
		len = snprintf(bp, dlen, "back chip_id= 0x%x\n", chip_id);
		tot += len; bp += len; dlen -= len;
#endif
		len = snprintf(bp, dlen, "0\n");
		tot += len; bp += len; dlen -= len;
	}

	if (sensor_opened == false) {
		if (info->pdata && info->pdata->power_off) {
			tegra_camera_mclk_on_off(0);
			info->pdata->power_off();
		} else {
			len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_off.\n");
			tot += len; bp += len; dlen -= len;
		}
	}

	if (copy_to_user(buf, debug_buf, tot))
		return -EFAULT;
	if (tot < 0)
		return 0;
	*ppos += tot;	/* increase offset */
	return tot;
}

static int dbg_iCatch7002a_chip_power_write(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
  int len;
  int arg;
  //int gpio, set;

  //char gpioname[8];

//  printk("%s: buf=%p, count=%d, ppos=%p\n", __FUNCTION__, buf, count, ppos);
  arg=0;

	if (*ppos)
		return 0;	/* the end */

//+ parsing......
  len=(count > DBG_TXT_BUF_SIZE-1)?(DBG_TXT_BUF_SIZE-1):(count);
  if (copy_from_user(debugTxtBuf,buf,len))
		return -EFAULT;

  debugTxtBuf[len]=0; //add string end

  sscanf(debugTxtBuf, "%x", &arg);
  printk("argument is arg=0x%x\n",arg);


  *ppos=len;
  //sensor_write_reg(info->i2c_client, arg[0], arg[1]);
  if (arg==0)  //power off
  {
	if (info->pdata && info->pdata->power_off) {
			info->pdata->power_off();
	} else {
		//len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_off.\n");
		//tot += len; bp += len; dlen -= len;
	}
  }
  if (arg==1) //power on
  {
	tegra_camera_mclk_on_off(1);
	msleep(10);
  	if (info->pdata && info->pdata->power_on)
			info->pdata->power_on();
	else {
		//len = snprintf(bp, dlen, "iCatch7002a info isn't enough for power_on.\n");
		//tot += len; bp += len; dlen -= len;
		}
		//msleep(10);
		//tegra_camera_mclk_on_off(1);
  }

	return len;	/* the end */
}

static const struct file_operations dbg_i7002a_fw_in_isp_fops = {
	.open		= dbg_i7002a_fw_in_isp_open,
	.read		= dbg_i7002a_fw_in_isp_read,
};

static const struct file_operations dbg_i7002a_page_dump_fops = {
	.open		= dbg_i7002a_page_dump_open,
	.read		= dbg_i7002a_page_dump_read,
};

static const struct file_operations dbg_fw_update_fops = {
	.open		= dbg_fw_update_open,
	.read		= dbg_fw_update_read,
	.write = dbg_fw_update_write,
};

static const struct file_operations i2c_set_fops = {
	.open		= i2c_set_open,
	//.read		= i2c_config_read,
	//.llseek		= seq_lseek,
	//.release	= single_release,
	.write = i2c_set_write,
};

static const struct file_operations i2c_get_fops = {
	.open		= i2c_get_open,
	.read		= i2c_get_read,
	//.llseek		= seq_lseek,
	//.release	= single_release,
	.write = i2c_get_write,
};

static const struct file_operations dbg_iCatch7002a_vga_status_fops = {
	.open		= dbg_iCatch7002a_vga_status_open,
	.read		= dbg_iCatch7002a_vga_status_read,
};

static const struct file_operations dbg_iCatch7002a_camera_status_fops = {
	.open		= dbg_iCatch7002a_camera_status_open,
	.read		= dbg_iCatch7002a_camera_status_read,
};

static const struct file_operations iCatch7002a_power_fops = {
	.open		= dbg_iCatch7002a_chip_power_open,
	//.read		= i2c_get_read,
	//.llseek		= seq_lseek,
	//.release	= single_release,
	.write = dbg_iCatch7002a_chip_power_write,
};static int __init tegra_i2c_debuginit(void)
{
       struct dentry *dent = debugfs_create_dir("i7002a", NULL);

	(void) debugfs_create_file("fw_in_isp", S_IRUGO | S_IWUSR,
					dent, NULL, &dbg_i7002a_fw_in_isp_fops);

	(void) debugfs_create_file("page_dump", S_IRUGO | S_IWUSR,
					dent, NULL, &dbg_i7002a_page_dump_fops);

	(void) debugfs_create_file("fw_update", S_IRUGO | S_IWUSR,
					dent, NULL, &dbg_fw_update_fops);

	(void) debugfs_create_file("i2c_set", S_IRUGO | S_IWUSR,
					dent, NULL, &i2c_set_fops);
	(void) debugfs_create_file("i2c_get", S_IRUGO | S_IWUSR,
					dent, NULL, &i2c_get_fops);
	(void) debugfs_create_file("camera_status", S_IRUGO, dent, NULL, &dbg_iCatch7002a_camera_status_fops);
	(void) debugfs_create_file("vga_status", S_IRUGO, dent, NULL, &dbg_iCatch7002a_vga_status_fops);
	(void) debugfs_create_file("iCatch_chip_power", S_IRUGO | S_IWUSR, dent, NULL, &iCatch7002a_power_fops);
#ifdef ICATCH7002A_DELAY_TEST
              if (debugfs_create_u32("iCatch7002a_delay", S_IRUGO | S_IWUSR, dent, &iCatch7002a_init_delay)
		== NULL) {
                printk(KERN_ERR "%s(%d): debugfs_create_u32: debug fail\n",
		__FILE__, __LINE__);
                return -1;
              }
              if (debugfs_create_u32("touch_focus_enable", S_IRUGO | S_IWUSR, dent, &touch_focus_enable)
		== NULL) {
                printk(KERN_ERR "%s(%d): debugfs_create_u32: debug fail\n",
		__FILE__, __LINE__);
                return -1;
              }
#endif
             debugfs_create_u32("page_index",S_IRUGO | S_IWUSR, dent, &dbg_i7002a_page_index);
#ifdef CAM_TWO_MODE
             debugfs_create_u32("div",S_IRUGO | S_IWUSR, dent, &g_div);
	return 0;
#endif
}

late_initcall(tegra_i2c_debuginit);
#endif

