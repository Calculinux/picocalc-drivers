// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight Driver for PicoCalc MFD
 */

#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/ioport.h>

struct picocalc_mfd_bkl {
    struct regmap *regmap;
    struct backlight_device *bldev;
    unsigned int reg;
};

static int picocalc_bkl_update_status(struct backlight_device *bldev)
{
    struct picocalc_mfd_bkl *bkl = bl_get_data(bldev);
    int brightness;
    
    /*
     * Check for blanking via fb_blank or state flags (not props.power which stays at 0).
     */
    if (bldev->props.fb_blank != FB_BLANK_UNBLANK || 
        bldev->props.state & (BL_CORE_FBBLANK | BL_CORE_SUSPENDED)) {
        /*
         * Blanking: poll hardware first to capture any button changes before turning off.
         * This preserves the brightness value so it can be restored on unblank.
         */
        u8 buf[2];
        int ret = regmap_bulk_read(bkl->regmap, bkl->reg, buf, 2);
        if (ret == 0) {
            bldev->props.brightness = buf[1];
        } else {
            dev_err(&bldev->dev, "Failed to read hw brightness before blanking, ret=%d\n", ret);
        }
        brightness = 0;
    } else {
        /*
         * Active/Unblanking: just use props.brightness directly without polling.
         * This restores the brightness that was saved during blanking.
         */
        brightness = bldev->props.brightness;
    }

    return regmap_write(bkl->regmap, bkl->reg | (1<<7), brightness);
}

static int picocalc_bkl_get_brightness(struct backlight_device *bldev)
{
    struct picocalc_mfd_bkl *bkl = bl_get_data(bldev);
    u8 buf[2];
    int ret;

    ret = regmap_bulk_read(bkl->regmap, bkl->reg, buf, 2);
    if (ret < 0) {
        dev_err(&bldev->dev, "Failed to read brightness, ret=%d\n", ret);
        return ret;
    }

    /* Always keep track of actual hardware brightness */
    bldev->props.brightness = buf[1];
    
    /* Return 0 if powered down, actual brightness otherwise */
    return (bldev->props.power == FB_BLANK_POWERDOWN) ? 0 : buf[1];
}

static const struct backlight_ops picocalc_bkl_ops = {
    .options = BL_CORE_SUSPENDRESUME,
    .update_status = picocalc_bkl_update_status,
    .get_brightness = picocalc_bkl_get_brightness,
};

static int picocalc_mfd_bkl_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct picocalc_mfd_bkl *bkl;
    struct backlight_properties props;

    bkl = devm_kzalloc(dev, sizeof(*bkl), GFP_KERNEL);
    if (!bkl)
        return -ENOMEM;

    u32 reg_addr;
    if (of_property_read_u32(pdev->dev.of_node, "reg", &reg_addr)) {
        dev_err(&pdev->dev, "Failed to get reg property\n");
        return -EINVAL;
    }
    bkl->reg = reg_addr;

    bkl->regmap = dev_get_regmap(dev->parent, NULL);
    if (!bkl->regmap) {
        dev_err(dev, "Failed to get parent regmap\n");
        return -EINVAL;
    }

    memset(&props, 0, sizeof(struct backlight_properties));
    props.type = BACKLIGHT_RAW;
    props.max_brightness = 255;
    props.power = FB_BLANK_UNBLANK;  /* Initialize power state before registration */
    
    u32 initial_brightness;
    if (!of_property_read_u32(pdev->dev.of_node, "default-brightness", &initial_brightness)) {
        props.brightness = initial_brightness;
    } else {
        props.brightness = props.max_brightness / 2;
    }

    const char *name = of_node_full_name(pdev->dev.of_node);
    bkl->bldev = devm_backlight_device_register(dev, name,
                                                dev->parent, bkl,
                                                &picocalc_bkl_ops, &props);
    if (IS_ERR(bkl->bldev)) {
        return PTR_ERR(bkl->bldev);
    }
    
    /* Set initial hardware state to match registered properties */
    backlight_update_status(bkl->bldev);
    platform_set_drvdata(pdev, bkl);

    dev_info(dev, "LCD backlight registered successfully\n");
    return 0;
}

static const struct of_device_id picocalc_mfd_bkl_of_match[] = {
    { .compatible = "picocalc-mfd-bkl" },
    {}
};
MODULE_DEVICE_TABLE(of, picocalc_mfd_bkl_of_match);

static struct platform_driver picocalc_mfd_bkl_driver = {
    .driver = {
        .name = "picocalc_mfd_bkl",
        .of_match_table = picocalc_mfd_bkl_of_match,
    },
    .probe = picocalc_mfd_bkl_probe,
};

module_platform_driver(picocalc_mfd_bkl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Laur <johnl@blurbco.com>");
MODULE_DESCRIPTION("LCD Backlight driver for PicoCalc MFD");