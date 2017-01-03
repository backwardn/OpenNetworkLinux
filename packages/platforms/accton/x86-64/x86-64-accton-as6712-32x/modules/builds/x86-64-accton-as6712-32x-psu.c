/*
 * An hwmon driver for accton as6712_32x Power Module
 *
 * Copyright (C) 2014 Accton Technology Corporation.
 *
 * Based on ad7414.c
 * Copyright 2006 Stefan Roese <sr at denx.de>, DENX Software Engineering
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/delay.h>

static ssize_t show_status(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t show_model_name(struct device *dev, struct device_attribute *da, char *buf);
static int as6712_32x_psu_read_block(struct i2c_client *client, u8 command, u8 *data,int data_len);
extern int as6712_32x_i2c_cpld_read(unsigned short cpld_addr, u8 reg);

/* Addresses scanned 
 */
static const unsigned short normal_i2c[] = { 0x50, 0x53, I2C_CLIENT_END };

/* Each client has this additional data 
 */
struct as6712_32x_psu_data {
    struct device      *hwmon_dev;
    struct mutex        update_lock;
    char                valid;           /* !=0 if registers are valid */
    unsigned long       last_updated;    /* In jiffies */
    u8  index;           /* PSU index */
    u8  status;          /* Status(present/power_good) register read from CPLD */
    char model_name[14]; /* Model name, read from eeprom */
};

static struct as6712_32x_psu_data *as6712_32x_psu_update_device(struct device *dev);             

enum as6712_32x_psu_sysfs_attributes {
    PSU_PRESENT,
    PSU_MODEL_NAME,
    PSU_POWER_GOOD
};

/* sysfs attributes for hwmon 
 */
static SENSOR_DEVICE_ATTR(psu_present,    S_IRUGO, show_status,    NULL, PSU_PRESENT);
static SENSOR_DEVICE_ATTR(psu_model_name, S_IRUGO, show_model_name,NULL, PSU_MODEL_NAME);
static SENSOR_DEVICE_ATTR(psu_power_good, S_IRUGO, show_status,    NULL, PSU_POWER_GOOD);

static struct attribute *as6712_32x_psu_attributes[] = {
    &sensor_dev_attr_psu_present.dev_attr.attr,
    &sensor_dev_attr_psu_model_name.dev_attr.attr,
    &sensor_dev_attr_psu_power_good.dev_attr.attr,
    NULL
};

static ssize_t show_status(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct as6712_32x_psu_data *data = as6712_32x_psu_update_device(dev);
    u8 status = 0;

    if (attr->index == PSU_PRESENT) {
        status = !(data->status >> ((data->index-1)*4) & 0x1);
    }
    else { /* PSU_POWER_GOOD */
        status = data->status >> ((data->index-1)*4 + 1) & 0x1;
    }

    return sprintf(buf, "%d\n", status);
}

static ssize_t show_model_name(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct as6712_32x_psu_data *data = as6712_32x_psu_update_device(dev);
    
    return sprintf(buf, "%s\n", data->model_name);
}

static const struct attribute_group as6712_32x_psu_group = {
    .attrs = as6712_32x_psu_attributes,
};

static int as6712_32x_psu_probe(struct i2c_client *client,
            const struct i2c_device_id *dev_id)
{
    struct as6712_32x_psu_data *data;
    int status;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK)) {
        status = -EIO;
        goto exit;
    }

    data = kzalloc(sizeof(struct as6712_32x_psu_data), GFP_KERNEL);
    if (!data) {
        status = -ENOMEM;
        goto exit;
    }

    i2c_set_clientdata(client, data);
    data->valid = 0;
    mutex_init(&data->update_lock);

    dev_info(&client->dev, "chip found\n");

    /* Register sysfs hooks */
    status = sysfs_create_group(&client->dev.kobj, &as6712_32x_psu_group);
    if (status) {
        goto exit_free;
    }

    data->hwmon_dev = hwmon_device_register(&client->dev);
    if (IS_ERR(data->hwmon_dev)) {
        status = PTR_ERR(data->hwmon_dev);
        goto exit_remove;
    }

    /* Update PSU index */
    if (client->addr == 0x50 || client->addr == 0x38) {
        data->index = 1;
    }
    else if (client->addr == 0x53 || client->addr == 0x3b) {
        data->index = 2;
    }

    dev_info(&client->dev, "%s: psu '%s'\n",
         dev_name(data->hwmon_dev), client->name);
    
    return 0;

exit_remove:
    sysfs_remove_group(&client->dev.kobj, &as6712_32x_psu_group);
exit_free:
    kfree(data);
exit:
    
    return status;
}

static int as6712_32x_psu_remove(struct i2c_client *client)
{
    struct as6712_32x_psu_data *data = i2c_get_clientdata(client);

    hwmon_device_unregister(data->hwmon_dev);
    sysfs_remove_group(&client->dev.kobj, &as6712_32x_psu_group);
    kfree(data);
    
    return 0;
}

static const struct i2c_device_id as6712_32x_psu_id[] = {
    { "as6712_32x_psu", 0 },
    {}
};
MODULE_DEVICE_TABLE(i2c, as6712_32x_psu_id);

static struct i2c_driver as6712_32x_psu_driver = {
    .class        = I2C_CLASS_HWMON,
    .driver = {
        .name     = "as6712_32x_psu",
    },
    .probe        = as6712_32x_psu_probe,
    .remove       = as6712_32x_psu_remove,
    .id_table     = as6712_32x_psu_id,
    .address_list = normal_i2c,
};

static int as6712_32x_psu_read_block(struct i2c_client *client, u8 command, u8 *data,
              int data_len)
{
    int result = 0;
    int retry_count = 5;
	
	while (retry_count) {
	    retry_count--;
	
	    result = i2c_smbus_read_i2c_block_data(client, command, data_len, data);
		
		if (unlikely(result < 0)) {
		    msleep(10);
	        continue;
		}
		
        if (unlikely(result != data_len)) {
            result = -EIO;
			msleep(10);
            continue;
        }
		
		result = 0;
		break;
	}
	
    return result;
}

static struct as6712_32x_psu_data *as6712_32x_psu_update_device(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct as6712_32x_psu_data *data = i2c_get_clientdata(client);
    
    mutex_lock(&data->update_lock);

    if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
        || !data->valid) {
        int status;
        int present = 0;

        dev_dbg(&client->dev, "Starting as6712_32x update\n");

        /* Read psu status */
        status = as6712_32x_i2c_cpld_read(0x60, 0x2);
        
        if (status < 0) {
            dev_dbg(&client->dev, "cpld reg 0x60 err %d\n", status);
        }
        else {
            data->status = status;
        }
		
        /* Read model name */
        memset(data->model_name, 0, sizeof(data->model_name));
        present = !(data->status >> ((data->index-1)*4) & 0x1);

        if (present) {
            u8 command;
            int model_name_len = 0;

            if (client->addr == 0x38 || client->addr == 0x3b) {
                /* cpr_4011_4mxx AC power */
                command = 0x26;
                model_name_len = 13;
            }
            else { /* 0x50 & 0x53 */
                /* um400d01x DC power */
                command = 0x50;
                model_name_len = 13;
            }

            status = as6712_32x_psu_read_block(client,command,data->model_name,
                                               model_name_len);

            if (status < 0) {
                data->model_name[0] = '\0';
                dev_dbg(&client->dev, "unable to read model name from (0x%x)\n", client->addr);
            }
            else {
                data->model_name[model_name_len] = '\0';
            }
        }
        
        data->last_updated = jiffies;
        data->valid = 1;
    }

    mutex_unlock(&data->update_lock);

    return data;
}

static int __init as6712_32x_psu_init(void)
{
    extern int platform_accton_as6712_32x(void);
    if(!platform_accton_as6712_32x()) { 
      return -ENODEV;
    }

    return i2c_add_driver(&as6712_32x_psu_driver);
}

static void __exit as6712_32x_psu_exit(void)
{
    i2c_del_driver(&as6712_32x_psu_driver);
}

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("as6712_32x_psu driver");
MODULE_LICENSE("GPL");

module_init(as6712_32x_psu_init);
module_exit(as6712_32x_psu_exit);
