// SPDX-License-Identifier: GPL-2.0-only
/*
 * Keyboard Driver for picocalc
 */

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/stddef.h>

#include "picocalc_kbd_code.h"

//#include "config.h"
#include "debug_levels.h"

#define REG_ID_BAT (0x0b)
#define REG_ID_BKL (0x05)
#define REG_ID_FIF (0x09)
#define REG_ID_BK2 (0x0A)

#define PICOCALC_WRITE_MASK (1<<7)

#define KBD_BUS_TYPE        BUS_I2C
#define KBD_VENDOR_ID       0x0001
#define KBD_PRODUCT_ID      0x0001
#define KBD_VERSION_ID      0x0001

#define KBD_FIFO_SIZE               31



static uint64_t mouse_fast_move_thr_time = 150000000ull;
static int8_t mouse_move_step = 1;

// From keyboard firmware source
enum pico_key_state
{
    KEY_STATE_IDLE = 0,
    KEY_STATE_PRESSED = 1,
    KEY_STATE_HOLD = 2,
    KEY_STATE_RELEASED = 3,
    KEY_STATE_LONG_HOLD = 4, //Unused
};
struct key_fifo_item
{
    uint8_t _ : 4;
    enum pico_key_state state : 4;
    uint8_t scancode;
};

#define MOUSE_MOVE_LEFT  (1 << 1)
#define MOUSE_MOVE_RIGHT (1 << 2)
#define MOUSE_MOVE_UP    (1 << 3)
#define MOUSE_MOVE_DOWN  (1 << 4)

struct kbd_ctx
{
    struct work_struct work_struct;
    uint8_t version_number;

    struct i2c_client *i2c_client;
    struct regmap *regmap;
    struct input_dev *input_dev;

    // Map from input HID scancodes to Linux keycodes
    uint8_t *keycode_map;

    // Key state and touch FIFO queue
    uint8_t key_fifo_count;
    struct key_fifo_item key_fifo_data[KBD_FIFO_SIZE];
    uint64_t last_keypress_at;

    int mouse_mode;
    uint8_t mouse_move_dir;
    bool left_shift_pressed;   // Shift status L
    bool right_shift_pressed;  // Shift status R
    bool F6_pressed;
    bool F7_pressed;
    bool F8_pressed;
    bool F9_pressed;
    bool F10_pressed;
    bool Brk_pressed;
    bool Home_pressed;
    bool End_pressed;
    bool PageUp_pressed;
    bool PageDown_pressed;
    bool Ins_pressed;
};

// Mapping of 2nd key scancodes to their corresponding kbd_ctx boolean offsets
struct second_key_maps_item {
    unsigned int scancode;
    size_t offset;
};
// This is a static, compile-time constant array that does not contain any runtime addresses
static const struct second_key_maps_item second_key_maps[] = {
    {0x86, offsetof(struct kbd_ctx, F6_pressed)},
    {0x87, offsetof(struct kbd_ctx, F7_pressed)},
    {0x88, offsetof(struct kbd_ctx, F8_pressed)},
    {0x89, offsetof(struct kbd_ctx, F9_pressed)},
    {0x90, offsetof(struct kbd_ctx, F10_pressed)},
    {0xD0, offsetof(struct kbd_ctx, Brk_pressed)},
    {0xD2, offsetof(struct kbd_ctx, Home_pressed)},
    {0xD5, offsetof(struct kbd_ctx, End_pressed)},
    {0xD6, offsetof(struct kbd_ctx, PageUp_pressed)},
    {0xD7, offsetof(struct kbd_ctx, PageDown_pressed)},
    {0xD8, offsetof(struct kbd_ctx, Ins_pressed)},
};

// Parse 0 to 255 from string
static inline int parse_u8(char const* buf)
{
    int rc, result;

    // Parse string value
    if ((rc = kstrtoint(buf, 10, &result)) || (result < 0) || (result > 0xff)) {
        return -EINVAL;
    }
    return result;
}

// Read a single uint8_t value from I2C register
static inline int kbd_read_i2c_u8(struct kbd_ctx* ctx, uint8_t reg_addr,
    uint8_t* dst)
{
    unsigned int val;
    int ret;

    ret = regmap_read(ctx->regmap, reg_addr, &val);
    if (ret < 0) {
        dev_err(&ctx->i2c_client->dev,
            "%s Could not read from register 0x%02X, error: %d\n",
            __func__, reg_addr, ret);
        return ret;
    }

    *dst = val;
    return 0;
}

// Write a single uint8_t value to I2C register
static inline int kbd_write_i2c_u8(struct kbd_ctx* ctx, uint8_t reg_addr,
    uint8_t src)
{
    int ret;

    ret = regmap_write(ctx->regmap, reg_addr | PICOCALC_WRITE_MASK, src);
    if (ret < 0) {
        dev_err(&ctx->i2c_client->dev,
            "%s Could not write to register 0x%02X, Error: %d\n",
            __func__, reg_addr, ret);
        return ret;
    }

    return 0;
}

// Read a pair of uint8_t values from I2C register
static inline int kbd_read_i2c_2u8(struct kbd_ctx* ctx, uint8_t reg_addr,
    uint8_t* dst)
{
    int ret;

    ret = regmap_bulk_read(ctx->regmap, reg_addr, dst, 2);
    if (ret < 0) {
        dev_err(&ctx->i2c_client->dev,
            "%s Could not read from register 0x%02X, error: %d\n",
            __func__, reg_addr, ret);
        return ret;
    }

    return 0;
}

// Shared global state for global interfaces such as sysfs
struct kbd_ctx *g_ctx;

// Sysfs attribute for mouse mode control
static ssize_t mouse_mode_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct kbd_ctx *ctx = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", ctx->mouse_mode ? 1 : 0);
}

static ssize_t mouse_mode_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct kbd_ctx *ctx = dev_get_drvdata(dev);
    int value;
    
    if (kstrtoint(buf, 10, &value) < 0)
        return -EINVAL;
    
    ctx->mouse_mode = (value != 0);
    
    // Report switch state change via input event
    input_report_switch(ctx->input_dev, SW_TABLET_MODE, ctx->mouse_mode);
    input_sync(ctx->input_dev);
    
    dev_info(dev, "Mouse mode %s via sysfs\n",
             ctx->mouse_mode ? "enabled" : "disabled");
    
    return count;
}

static DEVICE_ATTR_RW(mouse_mode);

static struct attribute *kbd_attrs[] = {
    &dev_attr_mouse_mode.attr,
    NULL,
};

static const struct attribute_group kbd_attr_group = {
    .attrs = kbd_attrs,
};

void input_fw_read_fifo(struct kbd_ctx* ctx)
{
    uint8_t fifo_idx;
    int rc;

    // Read number of FIFO items
    /*
    if (kbd_read_i2c_u8(ctx, REG_KEY, &ctx->key_fifo_count)) {
        return;
    }
    */
    ctx->key_fifo_count = 0;

    // Read and transfer all FIFO items
    for (fifo_idx = 0; fifo_idx < KBD_FIFO_SIZE; fifo_idx++) {

        uint8_t data[2];
        // Read 2 fifo items
        if ((rc = kbd_read_i2c_2u8(ctx, 0x09,
            (uint8_t*)&data))) {

            dev_err(&ctx->i2c_client->dev,
                "%s Could not read REG_FIF, Error: %d\n", __func__, rc);
            return;
        }

        if (data[0] == 0)
        {
            break;
        }

        ctx->key_fifo_data[fifo_idx]._ = 0;
        ctx->key_fifo_data[fifo_idx].state = data[0];
        ctx->key_fifo_data[fifo_idx].scancode= data[1];
        ctx->key_fifo_count++;
        // Advance FIFO position
        dev_info_fe(&ctx->i2c_client->dev,
            "%s %02d: 0x%02x%02x State %d Scancode %d\n",
            __func__, fifo_idx,
            ((uint8_t*)&ctx->key_fifo_data[fifo_idx])[0],
            ((uint8_t*)&ctx->key_fifo_data[fifo_idx])[1],
            ctx->key_fifo_data[fifo_idx].state,
            ctx->key_fifo_data[fifo_idx].scancode);
        /*
        printk("%02d: 0x%02x%02x State %d Scancode %d\n",
            fifo_idx,
            ((uint8_t*)&ctx->key_fifo_data[fifo_idx])[0],
            ((uint8_t*)&ctx->key_fifo_data[fifo_idx])[1],
            ctx->key_fifo_data[fifo_idx].state,
            ctx->key_fifo_data[fifo_idx].scancode);
        */
    }
}

static void key_report_event(struct kbd_ctx* ctx,
    struct key_fifo_item const* ev)
{
    uint8_t keycode;

    // Only handle key pressed, held, or released events
    if ((ev->state != KEY_STATE_PRESSED) && (ev->state != KEY_STATE_RELEASED)
     && (ev->state != KEY_STATE_HOLD)) {
        return;
    }

    // Post key scan event
//     input_event(ctx->input_dev, EV_MSC, MSC_SCAN, ev->scancode);

    // Track the physical pressed state of left and right shift
    bool is_held_or_pressed = (ev->state == KEY_STATE_PRESSED || ev->state == KEY_STATE_HOLD);

    if (ev->scancode == 0xA2) { // L_Shift
        ctx->left_shift_pressed = is_held_or_pressed;
    }
    if (ev->scancode == 0xA3) { // R_shift
        ctx->right_shift_pressed = is_held_or_pressed;
    }


/*
Special logic for Shift:
1. Used to toggle mouse mode,
2. Handle the case where Shift is released first in Shift + combination keys

*/
    if ((ev->state == KEY_STATE_PRESSED) && (ev->scancode == 0xA2 || ev->scancode == 0xA3)) {
        if (ctx->left_shift_pressed && ctx->right_shift_pressed) {
            ctx->mouse_mode = !ctx->mouse_mode;
            // Press both Shifts simultaneously to toggle mouse mode
            // Update switch to reflect mouse mode state
            input_report_switch(ctx->input_dev, SW_TABLET_MODE, ctx->mouse_mode);
            input_sync(ctx->input_dev);
        }
    } else if ((ev->state == KEY_STATE_RELEASED) && (ev->scancode == 0xA2 || ev->scancode == 0xA3)){
        if (!ctx->left_shift_pressed && !ctx->right_shift_pressed) {
            // When any Shift is released, if the tracked combination keys are not released, manually release them
            for (int i = 0; i < ARRAY_SIZE(second_key_maps); i++) {
                bool *is_pressed = (bool *)((char *)ctx + second_key_maps[i].offset);
                if (*is_pressed) {
                    *is_pressed = false;
                    uint8_t keycode = keycodes[second_key_maps[i].scancode];
                    input_report_key(ctx->input_dev, keycode, false);
                }
            }
        }
    }

    // Mouse mode
    if (ctx->mouse_mode){
        switch(ev->scancode){
        /* KEY_RIGHT */
        case 0xb7:
                if (ev->state == KEY_STATE_PRESSED)
                {
                    if (!(ctx->mouse_move_dir & MOUSE_MOVE_RIGHT))
                    ctx->last_keypress_at = ktime_get_boottime_ns();
                    ctx->mouse_move_dir |= MOUSE_MOVE_RIGHT;
                }
                else if (ev->state == KEY_STATE_RELEASED)
                {
                    ctx->mouse_move_dir &= ~MOUSE_MOVE_RIGHT;
                }
                return;
        /* KEY_LEFT */
        case 0xb4:
                if (ev->state == KEY_STATE_PRESSED)
                {
                    if (!(ctx->mouse_move_dir & MOUSE_MOVE_LEFT))
                    ctx->last_keypress_at = ktime_get_boottime_ns();
                    ctx->mouse_move_dir |= MOUSE_MOVE_LEFT;
                }
                else if (ev->state == KEY_STATE_RELEASED)
                {
                ctx->last_keypress_at = ktime_get_boottime_ns();
                    ctx->mouse_move_dir &= ~MOUSE_MOVE_LEFT;
                }
                return;
        /* KEY_DOWN */
        case 0xb6:
                if (ev->state == KEY_STATE_PRESSED)
                {
                    if (!(ctx->mouse_move_dir & MOUSE_MOVE_DOWN))
                    ctx->last_keypress_at = ktime_get_boottime_ns();
                    ctx->mouse_move_dir |= MOUSE_MOVE_DOWN;
                }
                else if (ev->state == KEY_STATE_RELEASED)
                {
                    ctx->mouse_move_dir &= ~MOUSE_MOVE_DOWN;
                }
                return;
        /* KEY_UP */
        case 0xb5:
                if (ev->state == KEY_STATE_PRESSED)
                {
                    if (!(ctx->mouse_move_dir & MOUSE_MOVE_UP))
                    ctx->last_keypress_at = ktime_get_boottime_ns();
                    ctx->mouse_move_dir |= MOUSE_MOVE_UP;
                }
                else if (ev->state == KEY_STATE_RELEASED)
                {
                    ctx->mouse_move_dir &= ~MOUSE_MOVE_UP;
                }
                return;
        /* KEY_RIGHTBRACE */
        case ']':
            input_report_key(ctx->input_dev, BTN_RIGHT, ev->state == KEY_STATE_PRESSED);
                return;
        /* KEY_LEFTBRACE */
        case '[':
            input_report_key(ctx->input_dev, BTN_LEFT, ev->state == KEY_STATE_PRESSED);
                return;
        /* KEY_MINUS = page up*/
        case 0x84:
        case 0x89:
            input_report_key(ctx->input_dev, KEY_PAGEUP, ev->state == KEY_STATE_PRESSED);
                return;
        /* KEY_PLUS = page down*/
        case 0x85:
        case 0x90:
            input_report_key(ctx->input_dev, KEY_PAGEDOWN, ev->state == KEY_STATE_PRESSED);
                return;
        default:
                break;
        }
    }


    // Map input scancode to Linux input keycode
    keycode = keycodes[ev->scancode];

    //keycode = ev->scancode;
    dev_info_fe(&ctx->input_dev->dev,
        "%s state %d, scancode %d mapped to keycode %d\n",
        __func__, ev->state, ev->scancode, keycode);
    //printk("state %d, scancode %d mapped to keycode %d\n",
    //  ev->state, ev->scancode, keycode);

    // Scancode mapped to ignored keycode
    if (keycode == 0) {
        return;

    // Scancode converted to keycode not in map
    } else if (keycode == KEY_UNKNOWN) {
        dev_warn(&ctx->input_dev->dev,
            "%s Could not get Keycode for Scancode: [0x%02X]\n",
            __func__, ev->scancode);
        return;
    }

    // Update last keypress time
    g_ctx->last_keypress_at = ktime_get_boottime_ns();

/*
    if (keycode == KEY_STOP) {

        // Pressing power button sends Tmux prefix (Control + code 171 in keymap)
        if (ev->state == KEY_STATE_PRESSED) {
            input_report_key(ctx->input_dev, KEY_LEFTCTRL, TRUE);
            input_report_key(ctx->input_dev, 171, TRUE);
            input_report_key(ctx->input_dev, 171, FALSE);
            input_report_key(ctx->input_dev, KEY_LEFTCTRL, FALSE);

        // Short hold power buttion opens Tmux menu (Control + code 174 in keymap)
        } else if (ev->state == KEY_STATE_HOLD) {
            input_report_key(ctx->input_dev, KEY_LEFTCTRL, TRUE);
            input_report_key(ctx->input_dev, 174, TRUE);
            input_report_key(ctx->input_dev, 174, FALSE);
            input_report_key(ctx->input_dev, KEY_LEFTCTRL, FALSE);
        }
        return;
    }
    */

    // Subsystem key handling
    /*
    if (input_fw_consumes_keycode(ctx, &keycode, keycode, ev->state)
     || input_touch_consumes_keycode(ctx, &keycode, keycode, ev->state)
     || input_modifiers_consumes_keycode(ctx, &keycode, keycode, ev->state)
     || input_meta_consumes_keycode(ctx, &keycode, keycode, ev->state)) {
        return;
    }
    */

    // Ignore hold keys at this point
    if (ev->state == KEY_STATE_HOLD) {
        return;
    }

/*
Handle the issue where when pressing esc - brk combination keys to trigger 2nd_key(brk), the shift is not released, causing the actual trigger of shift+2nd_key;
And if shift is released first, the firmware reports 1st_key "release" causing key confusion;
*/
    //Shift + Enter will be reported as 0xd1 Insert, so in the keymap, 0xd1 is also mapped to Enter, first handle the case of pressing Insert alone,
    if (ev->scancode == 0xd1 && !ctx->left_shift_pressed && !ctx->right_shift_pressed) {
        input_report_key(ctx->input_dev, KEY_INSERT, ev->state == KEY_STATE_PRESSED);
        return;
    }
    // Track the press state of 2nd keys, iterate through the processing list
    for (int i = 0; i < ARRAY_SIZE(second_key_maps); i++) {
        if (ev->scancode == second_key_maps[i].scancode) {

            bool *is_pressed = (bool *)((char *)ctx + second_key_maps[i].offset);

            *is_pressed = (ev->state == KEY_STATE_PRESSED);

            if(ev->state  == KEY_STATE_PRESSED){
                input_report_key(ctx->input_dev, KEY_LEFTSHIFT, FALSE);
                input_report_key(ctx->input_dev, KEY_RIGHTSHIFT, FALSE);
            }

            input_report_key(ctx->input_dev, keycode, ev->state == KEY_STATE_PRESSED);
            return;
        }
    }

    // Report key to input system
    input_report_key(ctx->input_dev, keycode, ev->state == KEY_STATE_PRESSED);

    // Reset sticky modifiers
//     input_modifiers_reset(ctx);
}

static void input_workqueue_handler(struct work_struct *work_struct_ptr)
{
    struct kbd_ctx *ctx;
    uint8_t fifo_idx;

    // Get keyboard context from work struct
    ctx = container_of(work_struct_ptr, struct kbd_ctx, work_struct);

    input_fw_read_fifo(ctx);
    // Process FIFO items
    for (fifo_idx = 0; fifo_idx < ctx->key_fifo_count; fifo_idx++) {
        key_report_event(ctx, &ctx->key_fifo_data[fifo_idx]);
    }

    if (ctx->mouse_mode)
        {
            uint64_t press_time = ktime_get_boottime_ns() - ctx->last_keypress_at;
            if (press_time <= mouse_fast_move_thr_time)
            {
                mouse_move_step = 1;
            }
            else if (press_time <= 3 * mouse_fast_move_thr_time)
            {
                mouse_move_step = 2;
            }
            else
            {
                mouse_move_step = 4;
            }

            if (ctx->mouse_move_dir & MOUSE_MOVE_LEFT)
            {
                input_report_rel(ctx->input_dev, REL_X, -mouse_move_step);
            }
            if (ctx->mouse_move_dir & MOUSE_MOVE_RIGHT)
            {
                input_report_rel(ctx->input_dev, REL_X, mouse_move_step);
            }
            if (ctx->mouse_move_dir & MOUSE_MOVE_DOWN)
            {
                input_report_rel(ctx->input_dev, REL_Y, mouse_move_step);
            }
            if (ctx->mouse_move_dir & MOUSE_MOVE_UP)
            {
                input_report_rel(ctx->input_dev, REL_Y, -mouse_move_step);
            }
        }

    // Reset pending FIFO count
    ctx->key_fifo_count = 0;

    // Synchronize input system and clear client interrupt flag
    input_sync(ctx->input_dev);
    /*
    if (kbd_write_i2c_u8(ctx, REG_INT, 0)) {
        return;
    }
    */
}
static void kbd_timer_function(struct timer_list *data);
DEFINE_TIMER(g_kbd_timer,kbd_timer_function);

static void kbd_timer_function(struct timer_list *data)
{
    data = NULL;
    schedule_work(&g_ctx->work_struct);
    mod_timer(&g_kbd_timer, jiffies + HZ / 128);
}

// Input event handler - allows userspace to control mouse mode via switch events
static int kbd_event(struct input_dev *dev, unsigned int type,
                     unsigned int code, int value)
{
    struct kbd_ctx *ctx = input_get_drvdata(dev);

    if (type != EV_SW)
        return -EINVAL;

    if (code == SW_TABLET_MODE) {
        // Set mouse mode based on switch value (0=off, 1=on)
        ctx->mouse_mode = (value != 0);
        // Report switch state change
        input_report_switch(ctx->input_dev, SW_TABLET_MODE, ctx->mouse_mode);
        input_sync(ctx->input_dev);
        dev_info(&ctx->i2c_client->dev,
            "Mouse mode %s by userspace\n",
            ctx->mouse_mode ? "enabled" : "disabled");
        return 0;
    }

    return -EINVAL;
}

int input_probe(struct i2c_client* i2c_client, struct regmap* regmap)
{
    int rc, i;

    // Allocate keyboard context (managed by device lifetime)
    g_ctx = devm_kzalloc(&i2c_client->dev, sizeof(*g_ctx), GFP_KERNEL);
    if (!g_ctx) {
        return -ENOMEM;
    }

    // Allocate and copy keycode array
    g_ctx->keycode_map = devm_kmemdup(&i2c_client->dev, keycodes, NUM_KEYCODES,
        GFP_KERNEL);
    if (!g_ctx->keycode_map) {
        return -ENOMEM;
    }

    // Initialize keyboard context
    g_ctx->i2c_client = i2c_client;
    g_ctx->regmap = regmap;
    g_ctx->last_keypress_at = ktime_get_boottime_ns();

    // Allocate input device
    if ((g_ctx->input_dev = devm_input_allocate_device(&i2c_client->dev)) == NULL) {
        dev_err(&i2c_client->dev,
            "%s Could not devm_input_allocate_device BBQX0KBD.\n", __func__);
        return -ENOMEM;
    }

    // Initialize input device
    g_ctx->input_dev->name = i2c_client->name;
    g_ctx->input_dev->id.bustype = KBD_BUS_TYPE;
    g_ctx->input_dev->id.vendor  = KBD_VENDOR_ID;
    g_ctx->input_dev->id.product = KBD_PRODUCT_ID;
    g_ctx->input_dev->id.version = KBD_VERSION_ID;

    // Initialize input device keycodes
    g_ctx->input_dev->keycode = keycodes; //g_ctx->keycode_map;
    g_ctx->input_dev->keycodesize = sizeof(keycodes[0]);
    g_ctx->input_dev->keycodemax = ARRAY_SIZE(keycodes);

    // Set input device keycode bits
    for (i = 0; i < NUM_KEYCODES; i++) {
        __set_bit(keycodes[i], g_ctx->input_dev->keybit);
    }
    __clear_bit(KEY_RESERVED, g_ctx->input_dev->keybit);
    __set_bit(EV_REP, g_ctx->input_dev->evbit);
    __set_bit(EV_KEY, g_ctx->input_dev->evbit);

    // Set input device capabilities
    input_set_capability(g_ctx->input_dev, EV_MSC, MSC_SCAN);
    input_set_capability(g_ctx->input_dev, EV_REL, REL_X);
    input_set_capability(g_ctx->input_dev, EV_REL, REL_Y);
/*
    input_set_capability(g_ctx->input_dev, EV_ABS, ABS_X);
    input_set_capability(g_ctx->input_dev, EV_ABS, ABS_Y);
    input_set_abs_params(g_ctx->input_dev, ABS_X, 0, 320, 4, 8);
    input_set_abs_params(g_ctx->input_dev, ABS_Y, 0, 320, 4, 8);
*/
    input_set_capability(g_ctx->input_dev, EV_KEY, BTN_LEFT);
    input_set_capability(g_ctx->input_dev, EV_KEY, BTN_RIGHT);

    // Enable switch for mouse mode indication (use tablet mode semantically)
    input_set_capability(g_ctx->input_dev, EV_SW, SW_TABLET_MODE);
    __set_bit(EV_SW, g_ctx->input_dev->evbit);
    __set_bit(SW_TABLET_MODE, g_ctx->input_dev->swbit);

    // Register input event handler for userspace control
    g_ctx->input_dev->event = kbd_event;
    input_set_drvdata(g_ctx->input_dev, g_ctx);

    // Request IRQ handler for I2C client and initialize workqueue
    /*
        g_ctx->left_shift_pressed = false;  // Initialize the state of both shifts
        g_ctx->right_shift_pressed = false; // Initialize the state of both shifts
        g_ctx->F6_pressed = false;
        g_ctx->F7_pressed = false;
        g_ctx->F8_pressed = false;
        g_ctx->F9_pressed = false;
        g_ctx->F10_pressed = false;
        g_ctx->Brk_pressed = false;
        g_ctx->Home_pressed = false;
        g_ctx->End_pressed = false;
        g_ctx->PageUp_pressed = false;
        g_ctx->PageDown_pressed = false;
        g_ctx->Ins_pressed = false;
        //
    if ((rc = devm_request_threaded_irq(&i2c_client->dev,
        i2c_client->irq, NULL, input_irq_handler, IRQF_SHARED | IRQF_ONESHOT,
        i2c_client->name, g_ctx))) {

        dev_err(&i2c_client->dev,
            "Could not claim IRQ %d; error %d\n", i2c_client->irq, rc);
        return rc;
    }
    */
    g_ctx->mouse_mode = FALSE;
    g_ctx->mouse_move_dir = 0;

    INIT_WORK(&g_ctx->work_struct, input_workqueue_handler);
    g_kbd_timer.expires = jiffies + HZ / 128;
    add_timer(&g_kbd_timer);

    // Register input device with input subsystem
    dev_info(&i2c_client->dev,
        "%s registering input device", __func__);
    if ((rc = input_register_device(g_ctx->input_dev))) {
        dev_err(&i2c_client->dev,
            "Failed to register input device, error: %d\n", rc);
        return rc;
    }

    return 0;
}

void input_shutdown(struct i2c_client* i2c_client)
{
    // Remove context from global state
    // (It is freed by the device-specific memory mananger)
    del_timer(&g_kbd_timer);
    g_ctx = NULL;
}

static int picocalc_mfd_kbd_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct i2c_client *i2c = to_i2c_client(dev->parent);
    struct regmap *regmap = dev_get_regmap(dev->parent, NULL);
    int rc;

    if (!regmap) {
        dev_err(dev, "Failed to get parent regmap\n");
        return -EINVAL;
    }

    // Initialize key handler system
    if ((rc = input_probe(i2c, regmap))) {
        return rc;
    }

    platform_set_drvdata(pdev, g_ctx);

    // Create sysfs attribute for mouse mode
    rc = sysfs_create_group(&pdev->dev.kobj, &kbd_attr_group);
    if (rc) {
        dev_warn(dev, "Failed to create sysfs attributes: %d\n", rc);
        // Non-fatal, continue anyway
    }

    dev_info(dev, "Keyboard input driver registered successfully\n");
    return 0;
}

static int picocalc_mfd_kbd_remove(struct platform_device *pdev)
{
    struct kbd_ctx *ctx = platform_get_drvdata(pdev);

    dev_info_fe(&pdev->dev, "%s Removing picocalc-kbd.\n", __func__);

    // Remove sysfs attributes
    sysfs_remove_group(&pdev->dev.kobj, &kbd_attr_group);

    input_shutdown(ctx->i2c_client);

    return 0;
}

static const struct of_device_id picocalc_mfd_kbd_of_match[] = {
    { .compatible = "picocalc-mfd-kbd", },
    {}
};
MODULE_DEVICE_TABLE(of, picocalc_mfd_kbd_of_match);

static struct platform_driver picocalc_mfd_kbd_driver = {
    .driver = {
        .name = "picocalc_mfd_kbd",
        .of_match_table = picocalc_mfd_kbd_of_match,
    },
    .probe    = picocalc_mfd_kbd_probe,
    .remove   = picocalc_mfd_kbd_remove,
};

module_platform_driver(picocalc_mfd_kbd_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hiro <hiro@hiro.com>");
MODULE_DESCRIPTION("keyboard driver for picocalc");
MODULE_VERSION("0.02");
