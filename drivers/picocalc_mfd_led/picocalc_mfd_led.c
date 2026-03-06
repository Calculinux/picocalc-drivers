// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keyboard Backlight LED Driver for PicoCalc MFD
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/fb.h>
#include <linux/notifier.h>

struct picocalc_mfd_led {
    struct regmap *regmap;
    struct led_classdev led_dev;
    unsigned int reg;
    struct notifier_block fb_notifier;
};

static enum led_brightness picocalc_led_get_brightness(struct led_classdev *led_cdev)
{
    struct picocalc_mfd_led *led = container_of(led_cdev,
                                                struct picocalc_mfd_led,
                                                led_dev);
    u8 buf[2];
    int ret;

    ret = regmap_bulk_read(led->regmap, led->reg, buf, 2);
    if (ret < 0)
        return ret;

    /* 
     * Always sync software state with hardware state.
     * This is called by the LED trigger before it saves the brightness,
     * so we capture any hardware button changes here.
     */
    led_cdev->brightness = buf[1];
        
    return buf[1];
}

static int picocalc_led_fb_notifier(struct notifier_block *nb,
                                    unsigned long event, void *data)
{
    struct picocalc_mfd_led *led = container_of(nb, struct picocalc_mfd_led, fb_notifier);
    struct fb_event *fb_event = data;
    int *blank;
    
    /* Only care about FB_EVENT_BLANK, and only when blanking (not unblanking) */
    if (event != FB_EVENT_BLANK)
        return NOTIFY_OK;
    
    blank = fb_event->data;
    if (*blank != FB_BLANK_UNBLANK) {
        /* 
         * About to blank: poll hardware to update cached brightness BEFORE
         * the LED backlight trigger saves it.
         */
        u8 buf[2];
        int ret = regmap_bulk_read(led->regmap, led->reg, buf, 2);
        if (ret == 0) {
            led->led_dev.brightness = buf[1];
        }
    }
    
    return NOTIFY_OK;
}

static void picocalc_led_brightness_set(struct led_classdev *led_cdev,
                                     enum led_brightness brightness)
{
    struct picocalc_mfd_led *led = container_of(led_cdev,
                                                struct picocalc_mfd_led,
                                                led_dev);
    
    regmap_write(led->regmap, led->reg | (1<<7), brightness);
}

static int picocalc_mfd_led_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct picocalc_mfd_led *led;
	struct led_init_data init_data = {};

    led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
    if (!led)
        return -ENOMEM;

    u32 reg_addr;
    if (of_property_read_u32(pdev->dev.of_node, "reg", &reg_addr)) {
        dev_err(&pdev->dev, "Failed to get reg property\n");
        return -EINVAL;
    }
    led->reg = reg_addr;

    led->regmap = dev_get_regmap(dev->parent, NULL);
    if (!led->regmap) {
        dev_err(dev, "Failed to get parent regmap\n");
        return -EINVAL;
    }

    led->led_dev.brightness_set = picocalc_led_brightness_set;
    led->led_dev.brightness_get = picocalc_led_get_brightness;
    
    /* Get initial hardware brightness */
    picocalc_led_get_brightness(&led->led_dev);

    /* Flag that hardware can change the brightness */
    led->led_dev.flags |= LED_BRIGHT_HW_CHANGED;

    /* LED name comes from device tree 'label' property */
    init_data.fwnode = dev_fwnode(dev);

    platform_set_drvdata(pdev, led);

    /* 
     * Register FB notifier to catch blanking events BEFORE the LED backlight
     * trigger processes them. This allows us to update the cached brightness
     * from hardware before the trigger saves it.
     */
    led->fb_notifier.notifier_call = picocalc_led_fb_notifier;
    led->fb_notifier.priority = 1;  /* Higher priority than trigger (0) to run first */
    fb_register_client(&led->fb_notifier);

    int ret = devm_led_classdev_register_ext(dev, &led->led_dev, &init_data);
    if (ret == 0)
        dev_info(dev, "Keyboard backlight LED registered successfully\n");
    
    return ret;
}

static void picocalc_mfd_led_remove(struct platform_device *pdev)
{
    struct picocalc_mfd_led *led = platform_get_drvdata(pdev);
    
    /* Unregister FB notifier */
    fb_unregister_client(&led->fb_notifier);
}

static const struct of_device_id picocalc_mfd_led_of_match[] = {
    { .compatible = "picocalc-mfd-led", },
    {}
};
MODULE_DEVICE_TABLE(of, picocalc_mfd_led_of_match);

static struct platform_driver picocalc_mfd_led_driver = {
    .driver = {
        .name = "picocalc_mfd_led",
        .of_match_table = picocalc_mfd_led_of_match,
    },
    .probe = picocalc_mfd_led_probe,
    .remove_new = picocalc_mfd_led_remove,
};

module_platform_driver(picocalc_mfd_led_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Laur <johnl@blurbco.com>");
MODULE_DESCRIPTION("Keyboard Backlight LED driver for PicoCalc MFD");