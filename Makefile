# SPDX-License-Identifier: GPL-2.0

# Generic drivers (any SBC) and Luckfox Lyra-specific drivers.
# Keep names in sync with repo layout.
SUBDIRS := drivers/picocalc_kbd drivers/picocalc_lcd_fb drivers/picocalc_lcd_drm \
		   drivers/picocalc_snd-pwm drivers/picocalc_snd-softpwm \
		   drivers/picocalc_mfd drivers/picocalc_mfd_bms drivers/picocalc_mfd_bkl \
		   drivers/picocalc_mfd_kbd drivers/picocalc_mfd_led \
		   luckfox-lyra/drivers/picocalc_rk3506_rproc luckfox-lyra/drivers/picocalc_snd-m0

obj-$(CONFIG_PICOCALC_KBD)     += drivers/picocalc_kbd/
obj-$(CONFIG_PICOCALC_LCD_FB)  += drivers/picocalc_lcd_fb/
obj-$(CONFIG_PICOCALC_LCD_DRM) += drivers/picocalc_lcd_drm/
obj-$(CONFIG_PICOCALC_SND_PWM)     += drivers/picocalc_snd-pwm/
obj-$(CONFIG_PICOCALC_SND_SOFT_PWM)     += drivers/picocalc_snd-softpwm/
obj-$(CONFIG_PICOCALC_MFD)     += drivers/picocalc_mfd/
obj-$(CONFIG_PICOCALC_MFD_BMS)     += drivers/picocalc_mfd_bms/
obj-$(CONFIG_PICOCALC_MFD_BKL)     += drivers/picocalc_mfd_bkl/
obj-$(CONFIG_PICOCALC_MFD_KBD)     += drivers/picocalc_mfd_kbd/
obj-$(CONFIG_PICOCALC_MFD_LED)     += drivers/picocalc_mfd_led/
obj-$(CONFIG_PICOCALC_RK3506_RPROC) += luckfox-lyra/drivers/picocalc_rk3506_rproc/
obj-$(CONFIG_PICOCALC_SND_M0)   += luckfox-lyra/drivers/picocalc_snd-m0/

# Kernel build targets
# Allow overriding KERNEL_SRC from the environment/recipe (e.g. KSRC)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	for d in $(SUBDIRS); do \
		if [ -d $$d ]; then \
			$(MAKE) -C $(KERNEL_SRC) M=$(PWD)/$$d modules; \
		fi; \
	done

clean:
	for d in $(SUBDIRS); do \
		if [ -d $$d ]; then \
			$(MAKE) -C $(KERNEL_SRC) M=$(PWD)/$$d clean; \
		fi; \
	done

install:
	# Install modules into KERNEL_SRC tree (honor INSTALL_MOD_PATH if set)
	for d in $(SUBDIRS); do \
		if [ -d $$d ]; then \
			$(MAKE) -C $(KERNEL_SRC) M=$(PWD)/$$d modules_install; \
		fi; \
	done

# --- Device tree overlay symbol whitelist ---
# Scans all *-overlay.dts files (generic + luckfox-lyra), extracts the base-DTB
# phandle labels they reference, and writes them to overlay-symbols.txt.
# The Yocto kernel recipe reads this file to inject only the needed __symbols__ into the DTB.

OVERLAY_SYMBOLS_FILE := overlays/overlay-symbols.txt
OVERLAY_DIRS := overlays luckfox-lyra/overlays

overlay-symbols: scripts/extract-overlay-symbols.sh
	@echo "Extracting overlay symbols..."
	@./scripts/extract-overlay-symbols.sh $(OVERLAY_DIRS) > $(OVERLAY_SYMBOLS_FILE).tmp
	@if ! cmp -s $(OVERLAY_SYMBOLS_FILE).tmp $(OVERLAY_SYMBOLS_FILE) 2>/dev/null; then \
		mv $(OVERLAY_SYMBOLS_FILE).tmp $(OVERLAY_SYMBOLS_FILE); \
		echo "Updated $(OVERLAY_SYMBOLS_FILE)"; \
	else \
		rm -f $(OVERLAY_SYMBOLS_FILE).tmp; \
		echo "$(OVERLAY_SYMBOLS_FILE) is up to date"; \
	fi

.PHONY: all clean install overlay-symbols
