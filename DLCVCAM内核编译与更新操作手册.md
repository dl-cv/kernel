# DLCVCAM RK3576 内核编译与更新操作手册

> 适用对象：DLCVCAM (RK3576)  
> 内核版本：6.1.99-rk3576  
> 编译主机：x86_64 Linux (交叉编译)  
> 目标板子：`ssh root@192.168.1.204`

---

## 1. 编译环境准备

确认交叉编译器可用：

```bash
which aarch64-linux-gnu-gcc
# 输出示例：/usr/bin/aarch64-linux-gnu-gcc
```

---

## 2. 编译内核

### 2.1 日期构建编号规则

DLCVCAM 发布内核使用仓库根目录的 `DLCVCAM_BUILD_VERSION` 作为构建编号，
格式固定为：

```text
YYYYMMDDNN
```

其中 `YYYYMMDD` 是发布日期，`NN` 是当天两位序号，从 `01` 开始。例如：

```text
2026072201
```

合入 `master` 前必须把该文件更新为实际合入日期；同一天发布多个内核时依次使用
`01`、`02`、`03`。不要通过修改 `VERSION/PATCHLEVEL/SUBLEVEL` 记录发布日期，
这样可以保持 `uname -r` 和 `/lib/modules/6.1.99-rk3576` 路径稳定。

开发机查看仓库默认构建编号：

```bash
cat DLCVCAM_BUILD_VERSION
make -s dlcvcam-build-version
```

板卡刷入对应内核后查看：

```bash
uname -v
cat /proc/version
```

输出示例：

```text
#2026072201 SMP Wed Jul 22 10:46:10 CST 2026
```

正常发布构建不需要再手工传入 `KBUILD_BUILD_VERSION`。CI 或临时构建仍可显式传入
该变量覆盖仓库值，但交付镜像必须使用 `DLCVCAM_BUILD_VERSION` 中已提交的编号。
建议镜像文件名同时记录构建编号和 Git 短提交，例如：

```text
boot-rk3576-6.1.99-2026072201-e8c10e0f.img
```

### 2.2 初次配置（仅需执行一次）

```bash
cd /home/ypw/kernel

# 加载基础配置
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- lubancat_linux_rk3576_defconfig

# 合并 DLCVCAM 裁剪配置
./scripts/kconfig/merge_config.sh -m .config \
    arch/arm64/configs/dlcvcam_rk3576_kernel_cut.config

# 应用合并后的配置
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
```

### 2.3 增量编译（日常修改后）

```bash
cd /home/ypw/kernel

# 增量编译内核、设备树、模块
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules -j8

# 安装模块到目标根文件系统（如需模块支持，如 WiFi/BT 等）
# 替换 /path/to/rootfs 为板子实际根文件系统挂载路径
# sudo make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_install INSTALL_MOD_PATH=/path/to/rootfs

# 生成 lz4 压缩内核
lz4 -f arch/arm64/boot/Image arch/arm64/boot/Image.lz4

# 打包 boot.img（FIT 格式，含 kernel + dtb + resource）
BOOT_ITS=boot.its ./scripts/mkimg --dtb dlcvcam-rk3576.dtb
```

> 建议用 `-j8`。如果内存较小（如 8GB 以下）出现 `cc1` 被杀死的情况，请改用 `-j4` 或 `-j2`。

编译完成后会生成：
- `boot.img`（~23MB，FIT 镜像）
- `resource.img`（Logo 等资源）

验证：

```bash
mkimage -l boot.img
```

同时确认本次编译使用的日期构建编号：

```bash
make -s ARCH=arm64 O=/path/to/kernel-out dlcvcam-build-version
# 2026072201
```

---

## 3. 烧录到板子

### 方式一：RKDevTool（Windows，官方推荐）

1. 下载 RKDevTool_v3.32 并解压（可从 [LCKFB wiki](https://wiki.lckfb.com/zh-hans/tspi-3-rk3576/img-download/distributed-image.html) 获取）
2. 下载分区表 `TaishanPi-3M-RK3576_Linux.cfg`，导入 RKDevTool
3. 勾选 **boot** 分区，选择生成的 `boot.img`
4. 板子进入 **Loader 模式**：
   - 按住 **REC** 按钮不放
   - 按一下 **RST** 复位键
   - 等待 2 秒后松开 **REC**
5. 点击 RKDevTool 的"执行"按钮烧录

### 方式二：rkdeveloptool（Linux 命令行）

安装工具：

```bash
sudo apt install libusb-1.0-0-dev
# 从 https://github.com/rockchip-linux/rkdeveloptool 编译安装
git clone https://github.com/rockchip-linux/rkdeveloptool.git
cd rkdeveloptool && autoreconf -i && ./configure && make && sudo make install
```

烧录步骤：

```bash
# 1. 板子进入 Maskrom 模式（短接 Maskrom 测试点 + 上电）
#    或进入 Loader 模式（REC + RST）

# 2. 确认设备连接
sudo rkdeveloptool ld
# 应显示：DevNo=1 Vid=0x2207,Pid=0x350b,Maskrom

# 3. 下载并烧录 boot.img
sudo rkdeveloptool db /path/to/RK3576Loader.bin   # 下载 Loader
sudo rkdeveloptool wl 0x8000 boot.img              # 写入 boot 分区（地址见分区表）
sudo rkdeveloptool rd                               # 重启
```

> 地址 `0x8000` 来自 `TaishanPi-3M-RK3576_Linux.cfg` 分区表中 boot 分区的起始偏移。

---

## 4. 完整编译更新脚本

在编译主机上创建 `build_bootimg.sh`：

```bash
#!/bin/bash
set -e

cd /home/ypw/kernel

echo "[1/4] 增量编译内核..."
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules -j8

echo "[2/4] 生成 Image.lz4..."
lz4 -f arch/arm64/boot/Image arch/arm64/boot/Image.lz4

echo "[3/4] 打包 boot.img..."
BOOT_ITS=boot.its ./scripts/mkimg --dtb dlcvcam-rk3576.dtb

echo "[4/4] 提示：如需更新内核模块（如 WiFi/BT 驱动等），请执行："
echo "  sudo make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_install INSTALL_MOD_PATH=/path/to/rootfs"

echo ""
echo "完成！"
ls -lh boot.img

echo ""
echo "下一步：通过 RKDevTool 或 rkdeveloptool 烧录 boot.img"
```

---

## 附录：常用排查命令

```bash
# 查看 FIT 镜像内容
mkimage -l boot.img

# 查看 eMMC 分区表
fdisk -l /dev/mmcblk0

# 查看当前内核日志中的显示相关日志
dmesg | grep -i "vop\|dp\|hdmi\|drm"

# 查看 USB-C / DP 状态
dmesg | grep -i "typec\|fusb302\|dp_alt"
```
