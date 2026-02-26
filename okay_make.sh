#!/bin/bash
make mrproper
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- lubancat_linux_rk3588_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8 Image dtbs modules

