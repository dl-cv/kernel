#!/bin/bash
scp ./arch/arm64/boot/Image root@192.168.1.27:/boot/Image-5.10.160
scp ./arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io.dtb root@192.168.1.27:/boot/dtb
scp ./arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dtbo root@192.168.1.27:/boot/dtb/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dtbo
scp arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dtbo root@192.168.1.27:/boot/dtb/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dtbo

