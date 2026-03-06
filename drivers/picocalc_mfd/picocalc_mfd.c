// SPDX-License-Identifier: GPL-2.0-only
/*
 * MFD Core Driver for the PicoCalc Keyboard Peripheral
 *
 * Based on drivers/mfd/simple-mfd-i2c.c
 *
 * This driver probes the I2C device and instantiates child platform devices
 * for the various functions provided by the peripheral (keyboard, power, etc.).
 * It sets up a regmap to provide safe, shared access to the device registers
 * for all child drivers.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/reboot.h>

#include "picocalc_reg.h"

struct picocalc_mfd_data {
	u32 poweroff_reg;
};

/* This regmap config assumes 8-bit register addresses and 8-bit values */
static const struct regmap_config picocalc_mfd_regmap_config = {
    .reg_bits = 8,
    .val_bits = 8,
};

static ssize_t fw_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct regmap *regmap = dev_get_regmap(dev, NULL);
    unsigned int version;
    int ret;

    ret = regmap_read(regmap, REG_ID_VER, &version);
    if (ret < 0)
        return ret;

    return sysfs_emit(buf, "0x%02x\n", version);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t fw_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct regmap *regmap = dev_get_regmap(dev, NULL);
    unsigned int type;
    int ret;

    ret = regmap_read(regmap, REG_ID_TYP, &type);
    if (ret < 0)
        return ret;

    return sysfs_emit(buf, "0x%02x\n", type);
}
static DEVICE_ATTR_RO(fw_type);

static struct attribute *picocalc_mfd_attrs[] = {
    &dev_attr_fw_version.attr,
    &dev_attr_fw_type.attr,
    NULL,
};

static const struct attribute_group picocalc_mfd_attr_group = {
    .attrs = picocalc_mfd_attrs,
};

static int picocalc_mfd_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
    struct regmap *regmap;
    unsigned int fw_type, fw_version;
    int ret;
    struct picocalc_mfd_data *data;

    data = devm_kzalloc(&i2c->dev, sizeof(struct picocalc_mfd_data), GFP_KERNEL);
    if (!data) {
        return -ENOMEM;
    }

    i2c_set_clientdata(i2c, data);

    ret = device_property_read_u32(&i2c->dev, "poweroff", &data->poweroff_reg);
    if (ret) {
        dev_err(&i2c->dev, "Failed to read 'poweroff' property\n");
        return ret;
    }

    /* Initialize the regmap for the I2C device */
    regmap = devm_regmap_init_i2c(i2c, &picocalc_mfd_regmap_config);
    if (IS_ERR(regmap)) {
        dev_err(&i2c->dev, "Failed to initialize regmap\n");
        return PTR_ERR(regmap);
    }

    /* Read firmware type and version */
    ret = regmap_read(regmap, REG_ID_TYP, &fw_type);
    if (ret < 0) {
        dev_err(&i2c->dev, "Failed to read firmware type\n");
        return ret;
    }

    ret = regmap_read(regmap, REG_ID_VER, &fw_version);
    if (ret < 0) {
        dev_err(&i2c->dev, "Failed to read firmware version\n");
        return ret;
    }

    dev_info(&i2c->dev,
             "PicoCalc MFD initialized at I2C address 0x%02x, firmware type 0x%02x, version 0x%02x\n",
             i2c->addr, fw_type, fw_version);

    ret = sysfs_create_group(&i2c->dev.kobj, &picocalc_mfd_attr_group);
    if (ret) {
        dev_err(&i2c->dev, "Failed to create sysfs group\n");
        return ret;
    }

    return devm_of_platform_populate(&i2c->dev);
}

static void picocalc_mfd_remove(struct i2c_client *i2c)
{
    sysfs_remove_group(&i2c->dev.kobj, &picocalc_mfd_attr_group);
}

static void picocalc_mfd_shutdown(struct i2c_client *i2c)
{
    struct picocalc_mfd_data *data = i2c_get_clientdata(i2c);
    struct regmap *regmap = dev_get_regmap(&i2c->dev, NULL);

    if (system_state == SYSTEM_RESTART)
        return;

    if (!regmap) {
        dev_err(&i2c->dev, "Failed to get regmap for shutdown\n");
        return;
    }

    dev_info(&i2c->dev, "Power Off\n");
    regmap_write(regmap, data->poweroff_reg | (1<<7), 1);
}

static const struct of_device_id picocalc_mfd_of_match[] = {
    { .compatible = "picocalc-mfd", },
    {}
};
MODULE_DEVICE_TABLE(of, picocalc_mfd_of_match);

static struct i2c_driver picocalc_mfd_driver = {
    .driver = {
        .name = "picocalc_mfd",
        .of_match_table = picocalc_mfd_of_match,
    },
    .probe = picocalc_mfd_probe,
    .remove = picocalc_mfd_remove,
    .shutdown = picocalc_mfd_shutdown,
};
module_i2c_driver(picocalc_mfd_driver);

MODULE_DESCRIPTION("PicoCalc MFD I2C Driver");
MODULE_AUTHOR("John Laur <johnl@blurbco.com>");
MODULE_LICENSE("GPL");