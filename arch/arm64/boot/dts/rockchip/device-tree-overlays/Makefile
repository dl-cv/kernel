#*********************************************************************
# SPDX-License-Identifier: (GPL-2.0+ OR MIT)
# 
# Copyright (c) 2023 Rockchip Electronics Co., Ltd.
# Copyright (c) 2026 LCKFB <lckfb@sz-jlc.com>
# *********************************************************************
# Wiki-(Chinese)：https://wiki.lckfb.com/
# Wiki-(English)：https://wiki.lckfb.com/en/
# *********************************************************************

# Makefile for TaishanPi-3 RK3576 Device Tree Overlays

# Tools
DTC ?= dtc
MKIMAGE ?= mkimage
CPP ?= $(CROSS_COMPILE)cpp

# Kernel directory (navigate from device-tree-overlays to kernel root)
# Path: kernel-6.1/arch/arm64/boot/dts/rockchip/device-tree-overlays -> kernel-6.1
KERNEL_DIR ?= $(shell cd ../../../../../.. && pwd)

# Include paths for C preprocessor (same as kernel DTS build)
DTS_INCLUDE := -I$(KERNEL_DIR)/include \
               -I$(KERNEL_DIR)/arch/arm64/boot/dts \
               -I$(KERNEL_DIR)/arch/arm64/boot/dts/rockchip

# CPP flags for DTS preprocessing
DTS_CPPFLAGS := -nostdinc -undef -D__DTS__ -x assembler-with-cpp -E

# Installation directory
DESTDIR ?= /boot

# Source files
UENV_TXT := ubootEnv.txt

# Find all .dts files in overlays/
OVERLAYS_DTS := $(wildcard overlays/*.dts)
OVERLAYS_DTBO := $(OVERLAYS_DTS:.dts=.dtbo)
OVERLAYS_TMP := $(OVERLAYS_DTS:.dts=.dts.tmp)

# Architecture for mkimage
ARCH ?= arm64

.PHONY: all clean install overlays boot help debug

# Default target: build everything
all: overlays

# Help target
help:
	@echo "TaishanPi-3 Device Tree Overlays Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build all overlays"
	@echo "  make overlays     - Build only .dtbo files"
	@echo "  make install      - Install to DESTDIR (default: /boot)"
	@echo "  make clean        - Remove all generated files"
	@echo ""
	@echo "Variables:"
	@echo "  DESTDIR=$(DESTDIR)"
	@echo "  DTC=$(DTC)"
	@echo "  MKIMAGE=$(MKIMAGE)"
	@echo ""
	@echo "Available overlays:"
	@for dts in $(OVERLAYS_DTS); do echo "  - $$(basename $$dts .dts)"; done

# Build all overlays
overlays: $(OVERLAYS_DTBO)
	@echo "Built $(words $(OVERLAYS_DTBO)) overlay(s)"

%.dts.tmp: %.dts
	@echo "  CPP      $<"
	@$(CPP) $(DTS_CPPFLAGS) $(DTS_INCLUDE) -o $@ $<

%.dtbo: %.dts.tmp
	@echo "  DTC      $@"
	@$(DTC) -@ -I dts -O dtb -o $@ $<
	@rm -f $<

# Install to destination
install: all
	@echo "Installing to $(DESTDIR)..."
	@mkdir -p $(DESTDIR)/overlays
	@echo "  INSTALL  $(UENV_TXT) -> $(DESTDIR)/ubootEnv.txt"
	@install -m 644 $(UENV_TXT) $(DESTDIR)/ubootEnv.txt
	@if [ -n "$(OVERLAYS_DTBO)" ]; then \
		echo "  INSTALL  overlays/*.dtbo"; \
		install -m 644 $(OVERLAYS_DTBO) $(DESTDIR)/overlays/; \
	fi
	@echo ""
	@echo "Installation complete!"
	@echo ""
	@echo "Directory structure:"
	@echo "  $(DESTDIR)/"
	@echo "  ├── ubootEnv.txt"
	@echo "  └── overlays/"
	@for dtbo in $(OVERLAYS_DTBO); do echo "      └── $$(basename $$dtbo)"; done
	@echo ""
	@echo "To enable overlays, edit $(DESTDIR)/ubootEnv.txt"
	@echo "and add your desired overlays to the 'overlays=' line."

# Clean build artifacts
clean:
	@echo "  CLEAN    *.dtbo *.scr *.tmp"
	@rm -f $(BOOT_SCR) $(OVERLAYS_DTBO) $(OVERLAYS_TMP)

# Debug: show variables
debug:
	@echo "OVERLAYS_DTS  = $(OVERLAYS_DTS)"
	@echo "OVERLAYS_DTBO = $(OVERLAYS_DTBO)"
	@echo "KERNEL_DIR    = $(KERNEL_DIR)"
	@echo "DTS_INCLUDE   = $(DTS_INCLUDE)"
	@echo "DESTDIR       = $(DESTDIR)"
	@echo "CPP           = $(CPP)"
