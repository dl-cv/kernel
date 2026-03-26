#!/bin/bash
scp ./arch/arm64/boot/Image root@192.168.1.199:/boot/Image-5.10.160
scp ./arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io.dtb root@192.168.1.199:/boot/dtb
scp ./arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-os08a20-3840x2160-30fps-overlay.dtbo root@192.168.1.199:/boot/dtb/overlay/rk3588-lubancat-5io-cam1-os08a20-3840x2160-30fps-overlay.dtbo
scp ./arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx415-overlay.dtbo root@192.168.1.199:/boot/dtb/overlay/rk3588-lubancat-5io-cam1-imx415-overlay.dtbo
scp ./arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dtbo root@192.168.1.199:/boot/dtb/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dtbo
