# PicoCalc MFD Keyboard Driver

Linux input driver for the PicoCalc keyboard, part of the PicoCalc Multi-Function Device (MFD) subsystem.

## Features

- Full QWERTY keyboard support with function keys
- Modifier key handling (Shift, Ctrl, Alt)
- Mouse mode toggle for cursor control via keyboard
- LED-based mouse mode status indication
- Userspace-controllable mouse mode via LED interface

## Mouse Mode

The driver supports a special **mouse mode** that converts keyboard input into mouse events:

### Activation

- **Hardware Toggle**: Press both shift keys simultaneously to toggle mouse mode ON/OFF
- **Software Control**: Write to the LED_MISC sysfs interface (see below)

### Mouse Mode Behavior

When active, the following keys are remapped:

| Key | Mouse Function |
|-----|----------------|
| Arrow Up/Down/Left/Right | Mouse movement (REL_X/REL_Y) |
| Enter | Left click (BTN_LEFT) |
| Space | Right click (BTN_RIGHT) |

### Userspace Control

Mouse mode can be controlled via both sysfs and input events:

**Via sysfs attribute (recommended):**
```bash
# Enable mouse mode
echo 1 > /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode

# Disable mouse mode
echo 0 > /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode

# Check current status
cat /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode
```

**Via input switch events:**
```c
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

int fd = open("/dev/input/event0", O_RDWR);

// Enable mouse mode
struct input_event ev = {
    .type = EV_SW,
    .code = SW_TABLET_MODE,
    .value = 1
};
write(fd, &ev, sizeof(ev));
```

## Key Mapping

The driver uses a scancode-to-keycode mapping defined in `picocalc_kbd_code.h`. Key features:

- Standard QWERTY layout
- Function keys F1-F10 (F6-F10 accessed via Shift+F1-F5)
- Navigation keys (arrows, Home, End, PageUp/PageDown)
- All standard modifiers and special keys

## Shifted Keys

Many keys have shifted variants handled by the keyboard firmware:

- **F1-F5** + Shift → **F6-F10**
- **1-0** + Shift → **! @ # $ % ^ & * ( )**
- **Symbol keys** + Shift → Alternate symbols (e.g., `-` → `_`, `=` → `+`)
- **Arrow Up/Down** + Shift → **PageUp/PageDown**
- **Enter** + Shift → **Insert**

## Alt Combinations

- **Alt + I** → **Insert**

## Building

This driver is part of the picocalc-drivers package and is built automatically by the Calculinux build system.

Manual build (for development):
```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

## Loading

The driver is automatically loaded by the MFD parent driver (`picocalc_mfd`). It registers as a platform driver:

```bash
# Check if loaded
lsmod | grep picocalc_mfd_kbd

# View logs
dmesg | grep picocalc
```

## Device Tree

Defined as a child node of the picocalc-mfd device:

```dts
picocalc-mfd-kbd {
    compatible = "picocalc-mfd-kbd";
};
```

## Troubleshooting

### Keys Not Working

1. Check driver is loaded: `lsmod | grep picocalc_mfd_kbd`
2. Check input device exists: `ls /dev/input/event*`
3. Test with evtest: `evtest /dev/input/event0` (adjust event number)
4. Check dmesg for errors: `dmesg | grep picocalc`

### Mouse Mode Not Activating

1. Check both shifts pressed simultaneously
2. Verify state: `cat /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode`
3. Try manual activation: `echo 1 > /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode`
4. Check dmesg for toggle events
5. Verify switch capability: `evtest /dev/input/event0` and look for SW_TABLET_MODE

### Mouse Mode Stuck On

```bash
# Force disable via sysfs
echo 0 > /sys/bus/platform/devices/picocalc-mfd-kbd/mouse_mode
```

## License

GPL-2.0-only

## Author

hiro <hiro@hiro.com>
