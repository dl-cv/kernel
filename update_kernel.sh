#!/bin/bash
set -e

# 泰山派3M RK3576 内核编译脚本
# 本脚本仅生成 boot.img，烧录需通过 RKDevTool / rkdeveloptool 完成
# 警告：不要直接 dd 写入 /dev/mmcblk0p3，会导致签名验证失败变砖

cd /home/ypw/kernel

echo "[1/4] 编译内核..."
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules -j8

echo "[2/4] 生成 Image.lz4..."
lz4 -f arch/arm64/boot/Image arch/arm64/boot/Image.lz4

echo "[3/4] 打包 boot.img..."
BOOT_ITS=boot.its ./scripts/mkimg --dtb tspi-3m-rk3576.dtb

echo "[4/4] 完成！"
ls -lh boot.img

echo ""
echo "=========================================="
echo "下一步：通过 RKDevTool 或 rkdeveloptool"
echo "进入 Loader/Maskrom 模式烧录 boot.img"
echo "=========================================="
