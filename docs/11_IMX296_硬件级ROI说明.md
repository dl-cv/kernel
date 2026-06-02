# IMX296 硬件级 ROI 说明

## 文档范围与互链

| 项目 | 说明 |
| --- | --- |
| 本文主责 | IMX296 硬件级 ROI（Region of Interest）的寄存器行为、驱动实现、`v4l2-ctl` 与 `media-ctl` 调用方法、ROI 与 binning/VMAX 的联动关系、调试与验证入口 |
| 不写进本文 | IMX296 板级适配、DTS/overlay、media graph、触发链路、曝光增益换算（见 `docs/06_IMX296_cam1_适配记录.md`、`docs/07_IMX296_v4l2曝光与增益说明.md`） |
| 互链 | `docs/06_IMX296_cam1_适配记录.md`（板级链路）、`docs/07_IMX296_v4l2曝光与增益说明.md`（曝光与增益控件） |

## 功能概述

本文汇总当前代码树中 `IMX296` 硬件级 ROI 的实现方式。IMX296 支持通过 `FID0_ROI` 寄存器组在传感器内部裁剪出感兴趣的矩形区域，输出分辨率随之缩小，同时驱动会根据 ROI 尺寸自动联动 binning 标志、`VMAX` 与 `MIPIC_AREA3W`，确保 MIPI 输出与后端 ISP/CIF 的帧描述一致。

## 背景与目标

在默认全分辨率（1456×1088）下，IMX296 的数据率和 ISP 处理负荷均处于最高值。部分应用场景只需要中心或局部区域成像，此时若能在 sensor 端完成硬件裁剪，可减少 MIPI 带宽占用并降低后端处理压力。IMX296  datasheet 提供的 `FID0_ROI` 机制允许通过 5 个寄存器（使能、起点、尺寸）配置硬件 ROI，驱动已在 `imx296_setup()` 中将其与 V4L2 `SELECTION`/`FORMAT` 框架对接，使上层可通过标准 `v4l2-ctl` 或 `media-ctl` 完成配置。

本次目标：

- 明确硬件 ROI 的寄存器映射、约束条件与对齐规则。
- 说明驱动中 ROI 与 binning、`VMAX`、`MIPIC_AREA3W` 的联动逻辑。
- 提供板端通过 `v4l2-ctl` 设置 ROI 的标准命令与验证方法。
- 提供 sysfs 只读节点快速查看当前 ROI 状态。
- 记录 ROI 模式下曝光上限的动态变化与注意事项。

## 文件清单

| 文件路径 | 类型 | 作用 | 调用方 | 依赖项 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `drivers/media/i2c/imx296.c` | V4L2 sensor 驱动 | 实现 ROI 寄存器下发、与 V4L2 `SELECTION`/`FORMAT` 对接、联动 binning/VMAX | V4L2 subdev framework / 板端工具 | I2C / regmap / V4L2 | 本文核心实现入口 |
| `docs/06_IMX296_cam1_适配记录.md` | 专题文档 | 记录板级链路、触发、默认模式 | 开发 / 调试 | DTS / 驱动实现 | 与本文互补，偏板级与触发 |
| `docs/07_IMX296_v4l2曝光与增益说明.md` | 专题文档 | 曝光与增益控件、换算与调试 | 开发 / 调试 | V4L2 ctrl framework | ROI 会改变 `frame_lines` 与曝光上限，需参考本文 |

## 关键函数

| 函数/方法 | 所在文件 | 作用 | 入参/出参 | 上下游依赖 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `imx296_setup()` | `drivers/media/i2c/imx296.c` | 在 `stream on` 时下发生成序列，包含 ROI 寄存器、binning、VMAX、MIPIC_AREA3W | 入参：`struct imx296 *`；出参：写寄存器结果 | `imx296_s_stream()` -> I2C 写寄存器 | ROI 使能/禁用、尺寸、位置均在此函数写入硬件 |
| `imx296_set_selection()` | `drivers/media/i2c/imx296.c` | V4L2 `SELECTION` 标准入口，设置 `V4L2_SEL_TGT_CROP` | 入参：`v4l2_subdev_selection`；出参：裁剪后的实际矩形 | V4L2 subdev pad ops -> `v4l2-ctl --set-selection` | 4 像素对齐并 clamp 到合法范围 |
| `imx296_set_format()` | `drivers/media/i2c/imx296.c` | V4L2 `FORMAT` 标准入口，同步更新 format 与 crop 尺寸 | 入参：`v4l2_subdev_format`；出参：实际 format | V4L2 subdev pad ops -> `v4l2-ctl --set-fmt` | 通过修改尺寸间接影响 ROI |
| `imx296_get_selection()` | `drivers/media/i2c/imx296.c` | 读取当前 crop / bounds / native size | 入参：`v4l2_subdev_selection`；出参：矩形值 | V4L2 subdev pad ops -> `v4l2-ctl --get-selection` | 支持 `CROP`、`CROP_DEFAULT`、`CROP_BOUNDS`、`NATIVE_SIZE` |
| `imx296_enum_frame_size()` | `drivers/media/i2c/imx296.c` | 枚举 sensor 支持的帧尺寸范围 | 入参：`v4l2_subdev_frame_size_enum`；出参：min/max | V4L2 subdev pad ops | 当前最小 80×4，最大 1456×1088 |
| `imx296_log_stream_state_locked()` | `drivers/media/i2c/imx296.c` | `stream on` 前后读回关键寄存器，输出 ROI 实际状态 | 入参：`struct imx296 *` / 日志标签；出参：无 | `imx296_stream_on()` -> `imx296_read()` | 包含 `roi_en`、`roi_pos`、`roi_size` 字段 |

## ROI 寄存器与约束

### 寄存器映射

| 寄存器 | 地址 | 位宽 | 作用 | 备注 |
| --- | --- | --- | --- | --- |
| `FID0_ROI` | `0x3300` | 8-bit | ROI 使能控制 | `bit0=ROIH1ON`（水平 ROI 使能），`bit1=ROIV1ON`（垂直 ROI 使能） |
| `FID0_ROIPH1` | `0x3310` | 16-bit | ROI 水平起始位置（left） | 从像素阵列左边缘起算 |
| `FID0_ROIPV1` | `0x3312` | 16-bit | ROI 垂直起始位置（top） | 从像素阵列上边缘起算 |
| `FID0_ROIWH1` | `0x3314` | 16-bit | ROI 水平宽度 | 最小 `80`，最大 `1456` |
| `FID0_ROIWV1` | `0x3316` | 16-bit | ROI 垂直高度 | 最小 `4`，最大 `1088` |
| `MIPIC_AREA3W` | `0x4182` | 16-bit | MIPI 输出垂直尺寸 | ROI 模式下需同步为 `crop->height`，全幅时恢复为 `1088` |
| `CTRL0D` | `0x300d` | 8-bit | Binning / 窗口模式控制 | `bit5=HADD_ON_BINNING`（水平 binning），`bit0~1=WINMODE_FD_BINNING`（垂直 binning） |

### 尺寸约束

| 参数 | 最小值 | 最大值 | 对齐要求 | 说明 |
| --- | --- | --- | --- | --- |
| 宽度（width） | `80` | `1456` | `4` 像素对齐 | 驱动内部用 `ALIGN(w, 4)` 处理 |
| 高度（height） | `4` | `1088` | `4` 像素对齐 | 驱动内部用 `ALIGN(h, 4)` 处理 |
| 水平起始（left） | `0` | `1456 - 80` | `4` 像素对齐 | 受剩余宽度约束 |
| 垂直起始（top） | `0` | `1088 - 4` | `4` 像素对齐 | 受剩余高度约束 |

### ROI 与 Binning 联动

驱动在 `imx296_setup()` 中根据 `crop` 与 `format` 的对比结果自动决定 binning 标志：

- 若 `crop->width != format->width`，则置位 `HADD_ON_BINNING`（水平 binning）。
- 若 `crop->height != format->height`，则置位 `WINMODE_FD_BINNING`（垂直 binning）。

当前代码中 `crop` 与 `format` 的尺寸在 `set_selection`/`set_format` 中保持同步，因此默认情况下 ROI 尺寸与输出尺寸一致，binning 标志通常为 `0`。若后续应用显式通过 `set_format` 请求比 ROI 更小的输出，binning 会自动启用。

### ROI 与 VMAX 联动

ROI 模式下，`VMAX` 不再使用全幅高度，而是写作：

```c
imx296_write(sensor, IMX296_VMAX, format->height + sensor->vblank->val, &ret);
```

即 `VMAX = ROI 高度 + vblank`。这意味着：

- 帧率会随 ROI 高度减小而提高（在相同 `vblank` 下）。
- 最大曝光时间（行数）也会随 ROI 高度减小而降低，需结合 `vblank` 调整。

## 依赖关系

| 依赖项 | 类型 | 用途 | 所在位置 | 备注 |
| --- | --- | --- | --- | --- |
| `V4L2_SEL_TGT_CROP` | V4L2 选择目标 | 用户态配置 ROI 矩形 | `drivers/media/i2c/imx296.c` | 标准 subdev selection 接口 |
| `V4L2_SUBDEV_FORMAT_TRY/ACTIVE` | V4L2 format 作用域 | 区分预览格式与实际生效格式 | `drivers/media/i2c/imx296.c` | `ACTIVE` 格式变更会触发 `setup_hblank` 与曝光范围更新 |
| `IMX296_FID0_ROIWH1_MIN=80` | 驱动常量 | ROI 最小宽度约束 | `drivers/media/i2c/imx296.c` | 硬件限制 |
| `IMX296_FID0_ROIWV1_MIN=4` | 驱动常量 | ROI 最小高度约束 | `drivers/media/i2c/imx296.c` | 硬件限制 |
| `IMX296_PIXEL_ARRAY_WIDTH=1456` | 驱动常量 | 全幅宽度 | `drivers/media/i2c/imx296.c` | 像素阵列物理尺寸 |
| `IMX296_PIXEL_ARRAY_HEIGHT=1088` | 驱动常量 | 全幅高度 | `drivers/media/i2c/imx296.c` | 像素阵列物理尺寸 |
| `roi_left` / `roi_top` / `roi_width` / `roi_height` | sysfs 节点 | 只读查看当前 ROI 状态 | `/sys/bus/i2c/devices/1-001a/` | 由 `sensor->crop` 直接导出 |
| `roi_enable` | sysfs 节点 | 只读查看 ROI 是否启用 | `/sys/bus/i2c/devices/1-001a/` | 判断条件：`width!=1456 || height!=1088` |

## 推荐调用链（设置 ROI）

### 方式一：通过 `v4l2-ctl` 设置 selection（推荐）

```sh
# 1. 确认 subdev 节点
v4l2-ctl --list-devices

# 2. 查看当前 crop（可选）
v4l2-ctl -d /dev/v4l-subdevX --get-selection=crop

# 3. 设置 ROI：例如从 (100, 80) 开始，裁剪出 800x600 区域
v4l2-ctl -d /dev/v4l-subdevX --set-selection=target=crop,left=100,top=80,width=800,height=600

# 4. 查看生效后的实际 crop（驱动会自动 4 像素对齐）
v4l2-ctl -d /dev/v4l-subdevX --get-selection=crop
```

### 方式二：通过 `v4l2-ctl` 设置 format（间接修改 ROI 尺寸）

```sh
# 设置 format 会同步更新 crop 的 width/height
v4l2-ctl -d /dev/v4l-subdevX --set-fmt=width=800,height=600

# 查看当前 format 与 crop
v4l2-ctl -d /dev/v4l-subdevX --get-fmt
v4l2-ctl -d /dev/v4l-subdevX --get-selection=crop
```

### 方式三：通过 sysfs 查看当前 ROI 状态（只读）

```sh
cat /sys/bus/i2c/devices/1-001a/roi_left
cat /sys/bus/i2c/devices/1-001a/roi_top
cat /sys/bus/i2c/devices/1-001a/roi_width
cat /sys/bus/i2c/devices/1-001a/roi_height
cat /sys/bus/i2c/devices/1-001a/roi_enable
```

### 恢复全幅

```sh
# 将 crop 恢复为全分辨率
v4l2-ctl -d /dev/v4l-subdevX --set-selection=target=crop,left=0,top=0,width=1456,height=1088
```

## 调试命令

| 调试命令 | 执行位置 | 用途 | 示例 |
| --- | --- | --- | --- |
| 查看 ROI 寄存器状态 | 板端 | 确认硬件是否真正写入 ROI | `dmesg \| grep -i "stream-state\["` |
| 查看当前 crop | 板端 | 确认 V4L2 层当前 ROI 矩形 | `v4l2-ctl -d /dev/v4l-subdevX --get-selection=crop` |
| 查看当前 format | 板端 | 确认输出分辨率与 ROI 是否一致 | `v4l2-ctl -d /dev/v4l-subdevX --get-fmt` |
| 查看 sysfs ROI 状态 | 板端 | 快速确认当前 ROI 数值 | `cat /sys/bus/i2c/devices/1-001a/roi_{left,top,width,height,enable}` |
| 查看帧率变化 | 板端 | ROI 高度减小后帧率会提升 | `v4l2-ctl -d /dev/v4l-subdevX --get-frameinterval` |
| 查看曝光范围 | 板端 | ROI 会改变最大曝光上限 | `v4l2-ctl -d /dev/v4l-subdevX --list-ctrls-ext \| grep exposure` |

## 常见问题

| 问题 | 常见原因 | 排查方式 | 备注 |
| --- | --- | --- | --- |
| 设置 `left/top` 后被改小 | 输入值未 4 像素对齐，驱动自动向下对齐 | 用 `--get-selection=crop` 回读实际值 | 属于正常 clamp 行为 |
| 设置 `width/height` 后被改大 | 输入值超出剩余阵列范围，驱动自动收缩 | 检查 `left + width <= 1456` 与 `top + height <= 1088` | 属于正常 clamp 行为 |
| ROI 缩小后长曝光上不去 | `VMAX` 随 ROI 高度减小而减小，最大曝光行数降低 | 增大 `vblank` 补偿，或切回全幅 | 参考 `docs/07_IMX296_v4l2曝光与增益说明.md` |
| `roi_enable` 为 `0` 但预期已启用 | 设置尺寸恰好等于全幅 `1456x1088`，驱动视为未裁剪 | 确认 `width` 或 `height` 是否小于全幅 | 驱动以 `width!=1456 \|\| height!=1088` 为启用条件 |
| 设置 ROI 后图像偏移不对 | `left/top` 被自动对齐到 4 像素边界，导致起始位置与预期有偏差 | 输入时直接按 4 的倍数设置 | 建议应用层主动对齐 |
| ROI 模式下帧率异常 | `VMAX` 已更新，但后端 ISP/CIF 的帧间隔未同步 | 确认应用侧是否按 `g_frame_interval` 重新配置 | ROI 高度越小，帧率越高 |

## 验证结果

### 已验证项

- `drivers/media/i2c/imx296.c` 中 `imx296_set_selection()` 已实现 `V4L2_SEL_TGT_CROP` 的 4 像素对齐、clamp 与防溢出。
- `drivers/media/i2c/imx296.c` 中 `imx296_setup()` 已根据 `crop` 与全幅对比自动写入 `FID0_ROI` 使能/禁用、位置、尺寸寄存器。
- `drivers/media/i2c/imx296.c` 中 `imx296_setup()` 已联动写入 `MIPIC_AREA3W`（ROI 高度）与 `CTRL0D`（binning 标志）。
- `drivers/media/i2c/imx296.c` 中 ROI 模式下 `VMAX` 已改为 `format->height + vblank`。
- `drivers/media/i2c/imx296.c` 已新增 `roi_left`、`roi_top`、`roi_width`、`roi_height`、`roi_enable` 五个只读 sysfs 节点。
- `drivers/media/i2c/imx296.c` 中 `imx296_log_stream_state_locked()` 已读回并打印 `FID0_ROI` 寄存器组状态，包含 `roi_en`、`roi_pos`、`roi_size`。
- `drivers/media/i2c/imx296.c` 中 `imx296_enum_frame_size()` 已报告最小尺寸 `80x4`。

### 未验证项

- 未在板端用示波器确认 ROI 模式下 MIPI 数据行的有效像素数是否真正减少。
- 未在板端验证极端 ROI（如 `80x4` 最小尺寸）的 ISP 链路稳定性。
- 未验证 ROI + binning 同时启用时的图像质量与尺度映射。
- 未在 `master_fast_trigger` 模式下单独验证 ROI 的触发帧输出行为。

## 风险点

- ROI 模式下 `VMAX` 减小会导致最大曝光时间下降，长曝光场景需提前评估。
- 当前 `set_format` 与 `set_selection` 在驱动内部会互相影响尺寸，应用层若同时调用两者，需以最终回读值为准。
- 若应用请求的 ROI 尺寸不是 4 的倍数，驱动会自动向下对齐，可能导致图像边缘出现未预期的黑边或裁剪偏移。
- ROI 启用后 MIPI 输出分辨率改变，后端 ISP/CIF 的 buffer 尺寸与帧间隔需同步更新，否则可能出现 `frame dma end` 不匹配。
- 当前 sysfs ROI 节点为只读，不支持直接通过 `echo` 写入修改；必须通过 V4L2 `SELECTION` 或 `FORMAT` 接口设置。

## 变更记录

| 日期 | 修改人/来源 | 修改原因 | 影响范围 | 对应功能/文件/模块 |
| --- | --- | --- | --- | --- |
| 2026-05-15 | Agent | 新增 IMX296 硬件级 ROI 支持，包括 `FID0_ROI` 寄存器组配置、与 V4L2 `SELECTION`/`FORMAT` 对接、联动 binning/VMAX/MIPIC_AREA3W | IMX296 sensor 驱动 / ROI 裁剪 / V4L2 subdev | `drivers/media/i2c/imx296.c` |
| 2026-05-15 | Agent | 新增 ROI 只读 sysfs 节点（`roi_left/top/width/height/enable`），方便板端快速查看当前 ROI 状态 | IMX296 板端调试入口 | `drivers/media/i2c/imx296.c` |
| 2026-05-15 | Agent | 在 `stream-state` 日志中补充 ROI 寄存器读回，支持软硬件联合定位 | IMX296 起流调试 / ROI 状态观测 | `drivers/media/i2c/imx296.c` |
| 2026-05-15 | Agent | 新建本文档，沉淀 ROI 寄存器映射、约束、调用链与常见问题 | 文档 / IMX296 ROI 专题 | `docs/11_IMX296_硬件级ROI说明.md` |
