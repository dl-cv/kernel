# IMX296 cam1 最小可编译可加载修复计划

## 需求背景

当前仓库中已经存在 `drivers/media/i2c/imx296.c` 和 `rk3588-lubancat-5io-cam1-imx296-overlay.dts`，但默认鲁班猫 RK3588 配置未启用 `CONFIG_VIDEO_IMX296`，`uEnvLubanCat5IO.txt` 也未提供 `cam1-imx296` 的注释入口。进一步板端日志又显示 `rkcif_update_sensor_info` / `Not active sensor`，说明原 overlay 通过动态创建 `imx296@1a` 并重写 `csi2_dcphy1/port@0` 的方式，存在 media graph 组织风险。完成 graph 修复后，板端继续出现 `rockchip-csi2-dphy0: No pixel rate control in subdev`，结合 Rockchip DPHY 驱动代码可确认其实际依赖 `V4L2_CID_LINK_FREQ`，而当前 `imx296.c` 尚未注册该控件。补齐 `LINK_FREQ` 后，板端又进一步暴露 `supply ovdd not found, using dummy regulator`，根因是 `dcphy1_imx296` 节点将 IO 电源写成了 `dovdd-supply`，与驱动要求的 `ovdd-supply` 不匹配。修正 `ovdd-supply` 后，板端在默认 autosuspend 路径下还出现 `8-bit write to 0x3005 failed: -6`；而在 `power/control=on` 后该错误消失，说明 `runtime PM` 触发的 `power_on` 时序过于激进，需要保守化处理。

## 目标与边界

### 目标

- 让鲁班猫 RK3588 默认 `defconfig` 包含 `CONFIG_VIDEO_IMX296`。
- 将 `cam1` 的 IMX296 模板节点和 1-lane graph endpoint 固化到 `rk3588-lubancat-5io-csi.dtsi`。
- 修正 `cam1-imx296 overlay` 的组织方式，只启用模板节点与公共链路，不再动态创建 sensor 节点和重写 `port@0`。
- 为 `imx296.c` 增加固定 `V4L2_CID_LINK_FREQ` 控件，满足 Rockchip CSI DPHY 的 lane 速率查询。
- 修正 `dcphy1_imx296` 的 IO 电源属性名，从 `dovdd-supply` 改为 `ovdd-supply`。
- 评估并对比 `imx296_power_on()` 原始/保守两套上电时序；当前代码树已回退为原始短延时时序，并结合最小寄存器读回日志继续区分是 `runtime PM` 边界问题，还是 sensor/MIPI 输出层问题。
- 在 `uEnvLubanCat5IO.txt` 中补充 `cam1-imx296` 的 `dtoverlay` 注释入口。
- 完成最小编译验证，确认驱动对象和 overlay 至少可以构建。

### 边界

- 不修改其它板型和其它相机槽位的默认行为。
- 不做整包烧录，不直接修改板端环境变量。
- 不引入与 IMX296 无关的驱动、DTS 或命名风格调整。
- 不在本轮处理 `XMASTER=High` 的真正 slave/外触发模式适配。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| `drivers/media/i2c/imx296.c` | 已存在 | 驱动源码已接入 |
| `drivers/media/i2c/Kconfig` / `Makefile` | 已存在 `VIDEO_IMX296` 接入 | Kconfig/Makefile 已闭环 |
| `arch/arm64/configs/lubancat_linux_rk3588_defconfig` | 未见 `CONFIG_VIDEO_IMX296` | 默认配置缺项 |
| `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` | 无 `dcphy1_imx296` 模板和对应 endpoint | base graph 缺少 IMX296 固定描述 |
| `rk3588-lubancat-5io-cam1-imx296-overlay.dts` | 已存在，但会动态新建 `imx296@1a` 并重写 `csi2_dcphy1/port@0` | overlay 结构需要改为模板启用模式 |
| `drivers/media/i2c/imx296.c` | 已注册 `PIXEL_RATE`，未注册 `LINK_FREQ` | DPHY 起流时无法获取 lane 速率 |
| `dcphy1_imx296` 供电属性 | 使用 `dovdd-supply` | 驱动会找不到 `ovdd-supply` 并退回 dummy regulator |
| `imx296_power_on()` 时序 | 电源/复位/时钟间隔过短 | `runtime PM` 恢复后 stream-on 可能出现首笔 I2C 写 `-ENXIO` |
| `uEnvLubanCat5IO.txt` | 无 `cam1-imx296` 注释入口 | 启用入口缺失 |

## 方案设计

1. 在 `arch/arm64/configs/lubancat_linux_rk3588_defconfig` 的 Sony/GalaxyCore 传感器配置区补 `CONFIG_VIDEO_IMX296=y`。
2. 在 `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` 中补齐 `dcphy1_imx296` 模板节点及 `dcphy1_in_imx296` endpoint，让 IMX296 与其它 cam1 传感器保持同一组织方式。
3. 调整 `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dts`：
   - `fragment@0` 仅开启 `i2c1`。
   - 新增针对 `&dcphy1_imx296` 的 fragment，将模板节点置为 `status = "okay"`。
   - 保留公共链路节点 `status = "okay"`。
   - 显式 targeting `&dcphy1_imx415`，将其置为 `status = "disabled"`，避免与 IMX296 同槽位误同时启用。
4. 在 `drivers/media/i2c/imx296.c` 中新增固定 `V4L2_CID_LINK_FREQ` 控件，向 Rockchip CSI DPHY 暴露单 lane、10-bit 的固定 MIPI 速率。
5. 将 `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` 中 `dcphy1_imx296` 的 `dovdd-supply` 更正为 `ovdd-supply`，与驱动的 `ovdd` regulator 名保持一致。
6. 在 `drivers/media/i2c/imx296.c` 中保守化 `power_on` 时序：每次上电前重新断言 reset，并增加电源稳定、XCLR 释放、时钟稳定到 I2C 访问前的等待时间。
7. 在 `arch/arm64/boot/dts/rockchip/uEnv/uEnvLubanCat5IO.txt` 的 `cam1` 小节增加：
   - `#dtoverlay=/dtb/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dtbo`

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `arch/arm64/configs/lubancat_linux_rk3588_defconfig` | 内核配置 | 默认启用 IMX296 驱动 | 内核构建流程 | `drivers/media/i2c/Kconfig` | 直接影响默认编译结果 |
| `drivers/media/i2c/imx296.c` | 驱动源码 | 提供 IMX296 V4L2 控件、起流逻辑与上电时序 | 媒体子系统 / Rockchip CSI DPHY | V4L2 ctrl framework / I2C / clk | 本次补 `V4L2_CID_LINK_FREQ`，增加最小寄存器读回日志用于起流定位；当前代码树已回退到原始短延时 `power_on` 时序 |
| `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` | 基础设备树 | 提供 cam1 传感器模板节点、CSI graph 和 IMX296 供电属性 | 基础板级 DTS | `rk3588-lubancat-5io.dts` | 本次新增 `dcphy1_imx296`/`dcphy1_in_imx296`，并修正 `ovdd-supply` |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dts` | 设备树 overlay | 使能 cam1 的 IMX296 节点与链路 | U-Boot overlay 加载流程 | `rk3588-lubancat-5io.dts` / `rk3588-lubancat-5io-csi.dtsi` | 仅针对 5IO cam1 |
| `arch/arm64/boot/dts/rockchip/uEnv/uEnvLubanCat5IO.txt` | 板级引导配置 | 提供启用 IMX296 overlay 的入口 | 板端 U-Boot | 生成后的 `dtbo` 文件 | 默认保留注释，不强制启用 |

## 关键函数 / 节点

| 函数/方法 | 所在文件 | 作用 | 输入 | 输出 | 调用方 | 被调对象 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `imx296_probe()` | `drivers/media/i2c/imx296.c` | 解析 DT 属性并注册 IMX296 子设备 | I2C client / OF 节点 | probe 结果 | I2C core | regulator/clk/v4l2 子系统 | 依赖 `CONFIG_VIDEO_IMX296` |
| `imx296_ctrls_init()` | `drivers/media/i2c/imx296.c` | 初始化曝光、增益、`LINK_FREQ`、`PIXEL_RATE` 等 V4L2 控件 | `struct imx296 *` | 初始化结果 | `imx296_probe()` | V4L2 ctrl framework | 本次补 `V4L2_CID_LINK_FREQ` |
| `imx296_power_on()` | `drivers/media/i2c/imx296.c` | 完成 regulator、reset、时钟的上电时序 | `struct imx296 *` | 上电结果 | runtime PM / probe | regulator/clk/gpio | 已完成原始/保守时序对比，当前树已回退到原始短延时时序 |
| `imx296_log_stream_state_locked()` | `drivers/media/i2c/imx296.c` | 在 `stream on` 前后读回 `CTRL00/CTRL0A/CTRL0B/SYNCSEL/LOWLAGTRG/PGCTRL` 等关键寄存器 | `struct imx296 *` / 标签字符串 | 无 | `imx296_stream_on()` | `imx296_read()` | 用于确认 sensor 是否退出 standby、是否进入 free-run/fast-trigger、测试图寄存器是否真正生效 |
| `dcphy1_imx415` | `rk3588-lubancat-5io-csi.dtsi` | cam1 默认 0x1a 传感器模板节点 | overlay 状态覆盖 | DTS 节点状态 | cam1 overlay | I2C1 / CSI2 DCPHY1 | 本次需由 IMX296 overlay 显式压制 |
| `dcphy1_imx296` / `dcphy1_in_imx296` | `rk3588-lubancat-5io-csi.dtsi` | cam1 IMX296 的固定 sensor 节点和 graph endpoint | overlay 状态覆盖 / endpoint 连接 | DT graph | IMX296 overlay / Rockchip CSI2 notifier | `i2c1` / `csi2_dcphy1` | 本次新增 |
| `rk3588-lubancat-5io-cam1-imx296-overlay.dts` 中 `fragment@0~12` | overlay 文件 | 开启 i2c1、启用 IMX296 模板并打开 cam1 公共链路 | DT overlay | 合并后的 DTB | U-Boot overlay 机制 | base DT labels | 不再动态创建 `imx296@1a` |

## 依赖项

| 依赖项 | 类型 | 用途 | 所在位置 | 使用入口 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `aarch64-linux-gnu-` | 交叉工具链 | arm64 内核最小编译验证 | 宿主机 | `make ARCH=arm64 CROSS_COMPILE=...` | 当前宿主机为 Linux |
| `dtc` / `fdtoverlay` | DT 工具 | overlay 语法和叠加验证 | 宿主机 | `dtc` / `fdtoverlay` | 已确认存在 |
| `ext_cam_37m_clk` | 固定时钟 | IMX296 输入时钟 | `rk3588-lubancat-cam.dtsi` | IMX296 模板节点 | 37125000 Hz |
| `cam_dovdd` | 电源 | IMX296 IO 电源 | `rk3588-lubancat-cam.dtsi` | `dcphy1_imx296` 的 `ovdd-supply` | 本次修正属性名以匹配驱动 |
| `rockchip,camera-module-sync-mode = "internal_master"` | DT 属性 | 让当前驱动按已支持的 master 路线工作 | `rk3588-lubancat-5io-csi.dtsi` | `imx296_probe()` | 当前不覆盖真正 slave/外触发场景 |
| `V4L2_CID_LINK_FREQ` | V4L2 控件 | 向 Rockchip CSI DPHY 暴露 MIPI lane 速率 | `drivers/media/i2c/imx296.c` | `phy-rockchip-csi2-dphy.c` | 本次补固定单项菜单值 |
| `power/control=on` | runtime PM 调试手段 | 保持 sensor 常上电以验证时序问题 | 板端 sysfs | `/sys/bus/i2c/devices/1-001a/power/control` | 用于定位 `0x3005` 写失败是否与 autosuspend 相关 |

## 风险点

- 当前源码树已有构建产物，若直接走独立输出目录会触发清理要求；验证时需要避免破坏用户现有构建环境。
- `cam1-imx296 overlay` 即使改成模板启用模式并能叠加，也不能替代板端真实硬件探测，仍需后续板端 `dmesg/media-ctl` 复核。
- 若用户同时启用 `cam1-imx415` 与 `cam1-imx296` overlay，最终仍取决于 overlay 加载顺序；本次只做最小防护。
- 当前 `imx296.c` 仍以 `internal_master` 为主路径，若后续恢复 `XMASTER=High` 的 slave/外触发方案，还需单独适配驱动与 DT。
- 本次仅补齐 `LINK_FREQ` 控件和宿主机过编；板端仍需重新部署新内核后，确认 `mipi1-csi2` 不再在 `stream ON` 后立即 `stream off`。
- 若板端实际硬件连线或 PMIC 配置与 `cam_dovdd` 不符，修正 `ovdd-supply` 属性名后仍可能继续黑屏，需要进一步回到板级供电和时钟测量。
- 即使 `runtime PM` 下的 I2C 写错误缓解，仍不能保证 sensor 已经稳定吐帧；若测试图仍抓不到数据，还需要继续分层排查节点选择、预览格式和硬件信号完整性。

## 验证方案

1. 宿主机最小验证：
   - `defconfig` 中存在 `CONFIG_VIDEO_IMX296=y`
   - `dtc` 能编 `rk3588-lubancat-5io-cam1-imx296-overlay.dts`
   - `dtc` 能编带 `dcphy1_imx296` 模板的 `rk3588-lubancat-5io.dts`
   - 交叉编译能生成 `drivers/media/i2c/imx296.o`
   - `imx296.c` 已暴露 `V4L2_CID_LINK_FREQ` 固定菜单项
   - 合并后的 `dcphy1-imx296@1a` 使用 `ovdd-supply`
   - `imx296.c` 当前原始短延时 `power_on` 时序仍可编译为对象文件
   - `fdtoverlay` 后可看到 `dcphy1-imx296@1a = okay`、`dcphy1-imx415@1a = disabled`
2. 板端待验证：
   - `dmesg | grep -i imx296`
   - `dmesg | grep -i "ovdd\\|dummy regulator"`
   - `dmesg | grep -i "0x3005\\|ENXIO"`
   - `dmesg | grep -i "pixel rate control\\|stream ON\\|stream off"`
   - `dmesg | grep -i "stream-state\\["`
   - `media-ctl -p`
   - `v4l2-ctl --list-devices`

## 执行结果

| 项目 | 结果 | 说明 |
| --- | --- | --- |
| `defconfig` 补项 | 已完成 | 已新增 `CONFIG_VIDEO_IMX296=y` |
| `5io-csi.dtsi` 模板补齐 | 已完成 | 已新增 `dcphy1_imx296` 节点和 `dcphy1_in_imx296` 1-lane endpoint |
| `cam1-imx296 overlay` 调整 | 已完成 | 改为启用 `&dcphy1_imx296` 模板并显式压制 `&dcphy1_imx415` |
| `uEnv` 注释入口 | 已完成 | 已新增 `cam1-imx296` 注释行 |
| 驱动最小过编修复 | 已完成 | `imx296.c` 补了缺失头文件和 `imx296_mbus_code()` 前向声明 |
| `LINK_FREQ` 控件补齐 | 已完成 | `imx296.c` 已新增固定 `V4L2_CID_LINK_FREQ`，用于满足 Rockchip CSI DPHY 速率查询 |
| `ovdd-supply` 属性修正 | 已完成 | `dcphy1_imx296` 已从 `dovdd-supply` 改为 `ovdd-supply`，避免驱动退回 dummy regulator |
| `power_on` 时序对比 | 已完成 | 已完成原始/保守对比；当前代码树按继续定位需要回退到原始短延时时序，并保留寄存器读回日志 |
| 宿主机最小验证 | 已完成 | `imx296.o`、overlay `dtbo`、base `dtb` 编译及 overlay 叠加验证通过；合并后 `dcphy1-imx296@1a=okay`、`dcphy1-imx415@1a=disabled`、`dcphy1_in_imx296` 为 1-lane，且 `ovdd-supply` 已就位 |
| 板端验证 | 未执行 | 本次未重新烧录当前回退原始时序且已加入寄存器读回日志的新内核并复测 |

## TODO 清单

- [x] 补 `CONFIG_VIDEO_IMX296=y` 到 `lubancat_linux_rk3588_defconfig`
- [x] 在 `rk3588-lubancat-5io-csi.dtsi` 中补 `dcphy1_imx296` 与 `dcphy1_in_imx296`
- [x] 将 `cam1-imx296 overlay` 改成模板启用模式
- [x] 在 `imx296.c` 中补固定 `V4L2_CID_LINK_FREQ`
- [x] 将 `dcphy1_imx296` 的 `dovdd-supply` 更正为 `ovdd-supply`
- [x] 完成 `imx296_power_on()` 原始/保守时序 A/B 对比，并按继续定位需求回退到原始短延时流程
- [x] 为 `uEnvLubanCat5IO.txt` 增加 `cam1-imx296` 注释入口
- [x] 按新 graph 结构完成最小编译验证
