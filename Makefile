# SPDX-License-Identifier: GPL-2.0

# List of driver subdirectories. Keep names in sync with repo layout.
SUBDIRS := picocalc_kbd picocalc_lcd_fb picocalc_lcd_drm picocalc_snd-pwm picocalc_snd-softpwm \
		   picocalc_mfd picocalc_mfd_bms picocalc_mfd_bkl picocalc_mfd_kbd picocalc_mfd_led

obj-$(CONFIG_PICOCALC_KBD)     += picocalc_kbd/
obj-$(CONFIG_PICOCALC_LCD_FB)  += picocalc_lcd_fb/
obj-$(CONFIG_PICOCALC_LCD_DRM) += picocalc_lcd_drm/
obj-$(CONFIG_PICOCALC_SND_PWM)     += picocalc_snd-pwm/
obj-$(CONFIG_PICOCALC_SND_SOFT_PWM)     += picocalc_snd-softpwm/
obj-$(CONFIG_PICOCALC_MFD)     += picocalc_mfd/
obj-$(CONFIG_PICOCALC_MFD_BMS)     += picocalc_mfd_bms/
obj-$(CONFIG_PICOCALC_MFD_BKL)     += picocalc_mfd_bkl/
obj-$(CONFIG_PICOCALC_MFD_KBD)     += picocalc_mfd_kbd/
obj-$(CONFIG_PICOCALC_MFD_LED)     += picocalc_mfd_led/

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
# Scans all *-overlay.dts files, extracts the base-DTB phandle labels they
# reference, and writes them to overlay-symbols.txt.  The Yocto kernel recipe
# reads this file to inject only the needed __symbols__ into the DTB.

OVERLAY_SYMBOLS_FILE := devicetree-overlays/overlay-symbols.txt

overlay-symbols: $(wildcard devicetree-overlays/*-overlay.dts) scripts/extract-overlay-symbols.sh
	@echo "Extracting overlay symbols..."
	@./scripts/extract-overlay-symbols.sh > $(OVERLAY_SYMBOLS_FILE).tmp
	@if ! cmp -s $(OVERLAY_SYMBOLS_FILE).tmp $(OVERLAY_SYMBOLS_FILE) 2>/dev/null; then \
		mv $(OVERLAY_SYMBOLS_FILE).tmp $(OVERLAY_SYMBOLS_FILE); \
		echo "Updated $(OVERLAY_SYMBOLS_FILE)"; \
	else \
		rm -f $(OVERLAY_SYMBOLS_FILE).tmp; \
		echo "$(OVERLAY_SYMBOLS_FILE) is up to date"; \
	fi

.PHONY: all clean install overlay-symbols
