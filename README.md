   # PicoCalc driver sources

   This repository provides the out-of-tree Linux kernel driver sources and a top-level build Makefile for the PicoCalc device and related hardware.

   For Yocto/OpenEmbedded consumption and BitBake recipes, see the `meta-picocalc-drivers` or `meta-calculinux` repositories which contain recipes that can fetch and package these sources for images.

   ## Repository layout

   - **`drivers/`** – Generic drivers applicable to any SBC used with PicoCalc (MFD, keyboard, LCD, sound, etc.).
   - **`overlays/`** – Generic device tree overlays (e.g. 100 kHz I2C, DS3231 RTC).
   - **`luckfox-lyra/`** – Luckfox Lyra–specific drivers, firmware, overlays, and device tree sources.

   What this repo contains
   - A top-level Makefile that can build all drivers against a kernel source (`KERNEL_SRC` / `KSRC`).
   - Subdirectories for individual drivers (MFD core and sub-drivers, legacy keyboard, LCD, sound drivers, etc.).

   Quick standalone build
   1. Ensure you have a matching kernel build directory (for example the kernel source or headers for the target kernel):

   ```bash
   # KERNEL_SRC should point to the kernel build directory (the directory with the top-level Makefile)
   export KERNEL_SRC=/path/to/kernel/build
   # Build everything
   make KERNEL_SRC=${KERNEL_SRC} all

   # Install modules into a staging directory (honors INSTALL_MOD_PATH)
   make KERNEL_SRC=${KERNEL_SRC} INSTALL_MOD_PATH=/tmp/picocalc-mods modules_install
   ```

   Notes:
   - The Makefile delegates to the kernel build system (it runs `make -C ${KERNEL_SRC} M=$(PWD)/<driver> modules`).
   - You can then copy the produced `.ko` files from the install tree to your target rootfs or package them for your distribution.

   Device tree / runtime notes
   - The drivers support both a legacy keyboard driver and a new MFD-based keyboard. These can be installed simultaneously; the device tree 'compatible' strings and driver probe order control which driver binds at runtime.
   - The MFD core driver populates child platform devices for sub-drivers (keyboard, backlight, BMS, LED).
   - Device tree source-of-truth lives in this repo. Luckfox Lyra–specific Linux and U-Boot DTS files are under `luckfox-lyra/`:
      - Linux: `luckfox-lyra/linux-rk3506-luckfox-lyra.dtsi`, `luckfox-lyra/linux-rk3506g-luckfox-lyra.dts`
      - U-Boot: `luckfox-lyra/uboot-rk3506-luckfox.dtsi`, `luckfox-lyra/uboot-rk3506-luckfox.dts`
      The Yocto `picocalc-devicetree` recipe installs these into the sysroot for the Luckfox Lyra platform.

   Contributing
   - Keep each driver in its own subdirectory with a small `Makefile`/Kbuild fragment if needed (see existing subdirs).
   - Prefer the kernel build system's `modules`/`modules_install` rules rather than custom build logic.
   - Update licensing and headers for any new files, and add tests or instructions where appropriate.

   License
   - Kernel parts are GPL-compatible; check each driver subdirectory for the specific licensing header.

   Contact
   - Raise issues or PRs against the Calculinux organization repositories on GitHub.
