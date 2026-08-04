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

### 2.1 初次配置（仅需执行一次）

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

### 2.2 增量编译（日常修改后）

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

## 4. 完整编译更新脚本（推荐）

> **推荐日常使用仓库根目录的 `build_bootimg.sh`**（见下）。  
> 防砖（U-Boot FIT fallback / boot-try、recovery 金镜像、confirm 服务）**不在本仓库实现**：  
> - U-Boot：https://github.com/dl-cv/u-boot/pull/1 （`tools/dlcvcam/` 含 confirm 脚本协议副本）  
> - CamOS Admin：seed recovery、安装 boot-try / WDT、维护脚本 `dlcvcam_sync_boot_to_recovery.sh`

### 4.1 一键编译

```bash
cd /path/to/kernel   # 本仓库

./build_bootimg.sh

# 内存较小时降低并行度
./build_bootimg.sh -j4

# 强制重新 defconfig + 合并裁剪配置
./build_bootimg.sh --force-config

# 删除 .config 后走完整初次配置
./build_bootimg.sh --clean-config

# 不自动改写 DLCVCAM_BUILD_VERSION（沿用文件当前值）
./build_bootimg.sh --no-bump
```

### 4.2 脚本会做什么

1. **构建编号**：按 `DLCVCAM_BUILD_VERSION`（`YYYYMMDDNN`）规则自动 bump（见 §2.1）。  
2. **配置**：无合适 `.config` 时 `lubancat_linux_rk3576_defconfig` + `dlcvcam_rk3576_kernel_cut.config`。  
3. **编译打包**：`Image` / `dtbs` / `modules` → `Image.lz4` → `boot.img`（FIT）。  
4. **交付文件名**（同时保留根目录 `boot.img`）：

```text
boot-rk3576-6.1.99-<YYYYMMDDNN>-<git短sha[.dirty]>.img
```

5. **完整性 sidecar（无签名）**：为 `boot.img` 与交付镜像各生成：

```text
<镜像>.sha256         # 整包 SHA-256
<镜像>.dlcvcam.json   # 整包哈希 + FIT 内嵌分量哈希清单 + 构建元数据
```

FIT 镜像本身在打包时已写入各 image 的内嵌 `sha256`（见 `boot.its`）。  
sidecar 用于上传后、烧录前做**损坏/截断/误传**检测；**不能替代签名**。

### 4.3 编译完成后

```bash
ls -lh boot.img boot-rk3576-*.img boot-rk3576-*.img.sha256 boot-rk3576-*.img.dlcvcam.json
mkimage -l boot.img

# 本地再验一次（打包脚本结束时已自检）
python3 scripts/dlcvcam_verify_bootimg.py verify boot-rk3576-*.img --require-sidecar
```

### 4.4 板卡上传后、烧录前校验（推荐）

把**镜像 + `.sha256`（建议连同 `.dlcvcam.json` 与校验脚本）**一起拷到板子，例如 `/tmp`：

```bash
# 在板卡上（需 python3）
python3 scripts/dlcvcam_verify_bootimg.py verify /tmp/boot-rk3576-....img \
  --require-sidecar
# 退出码 0 才允许烧录；非 0 则重新传输，不要 wl/dd
```

| 检查 | 依据 | 作用 |
|------|------|------|
| 整包 SHA-256 | `<img>.sha256` | 发现下载截断、拷贝损坏、传错文件 |
| FIT 内嵌 hash | boot.img 内 fdt/kernel/resource 的 sha256 节点 | 发现 payload 被改；**不依赖 sidecar** 也能查内容损坏 |

只带了镜像、没有 sidecar 时，仍可只验 FIT 内嵌 hash：

```bash
python3 scripts/dlcvcam_verify_bootimg.py verify /tmp/boot-rk3576-....img
```

对已有镜像补生成 sidecar（开发机）：

```bash
python3 scripts/dlcvcam_verify_bootimg.py gen-sidecar boot-rk3576-....img
```

下一步：校验通过后，用第 3 节的 RKDevTool 或 `rkdeveloptool` 烧录（或经 **CamOS Admin** 内核升级面板）。

### 4.5 防砖与 recovery（外链，本仓不提供脚本）

| 能力 | 仓库 / 位置 |
|------|-------------|
| boot FIT 坏 → 读 recovery | [u-boot#1](https://github.com/dl-cv/u-boot/pull/1) `fit.c` |
| 合法 FIT 起不来 → boot-try 计数 | 同上；misc@48KiB `DCBT` |
| multi-user 后清计数 | u-boot `tools/dlcvcam/` + **Admin** 安装 confirm unit |
| recovery 金镜像 status/sync/restore | **CamOS Admin** `dlcvcam_sync_boot_to_recovery.sh` |
| 硬挂复位 | Admin `10-dlcvcam-watchdog.conf`（`RuntimeWatchdogSec`） |
| 升级前 seed recovery / 只写 boot | Admin 内核升级逻辑 |

**原则（产品路径）：**

- 日常升级**只写 boot**；recovery 为金镜像，稳定后才手工提升。  
- 不使用完整 userspace bootguard 状态机。  
- 板端安装与操作以 Admin / u-boot `tools/dlcvcam/README.md` 为准。

---

## 附录：常用排查命令

```bash
# 查看 FIT 镜像内容
mkimage -l boot.img

# 完整性校验（整包 + FIT 内嵌 hash，无签名）
python3 scripts/dlcvcam_verify_bootimg.py verify boot.img --require-sidecar

# 查看 eMMC 分区表
fdisk -l /dev/mmcblk0

# 查看当前内核日志中的显示相关日志
dmesg | grep -i "vop\|dp\|hdmi\|drm"

# 查看 USB-C / DP 状态
dmesg | grep -i "typec\|fusb302\|dp_alt"
```
