# IMX296 trigger-dev 默认目标切换计划

## 需求背景

当前 `trigger-dev` 的默认硬触发链路仍指向 `OS08A20` 的 sysfs 入口（`/sys/bus/i2c/devices/1-0036/trigger`），而当前板级 `IMX296` 方案已经具备独立的 `trigger` 入口（`/sys/bus/i2c/devices/1-001a/trigger`）并通过 `GPIO1_D6(PWM14_M2)` 输出低电平脉冲。

本次目标是将外部硬触发输入（`GPIO0_C6`）统一切换到 `IMX296` 触发入口，避免与 `OS08A20` 默认链路描述冲突，同时保持最小改动，不触碰 `OS08A20` 驱动和其 overlay 本身。

## 目标与边界

### 目标

- 将 `rk3588-lubancat-5io-trigger-dev-overlay.dts` 的 `trigger-path` 从 `1-0036` 切换到 `1-001a`。
- 保持硬触发输入口仍为 `GPIO0_C6`，下降沿触发逻辑不变。
- 更新 `trigger-dev` 相关文档的默认命令、链路图和调试步骤，统一到 `IMX296`。
- 在 `IMX296` 专题文档中补充本次切换关系和变更记录。

### 边界

- 不修改 `drivers/media/i2c/os08a20.c` 中的 FSIN/单帧逻辑。
- 不修改 `rk3588-lubancat-5io-cam1-os08a20-3840x2160-30fps-overlay.dts` 的硬触发配置。
- 不变更 `trigger-dev` IRQ/workqueue 核心时序逻辑，仅做必要文义清理。
- 不执行板端刷写，仅做宿主机最小静态编译验证。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| `trigger-dev overlay` 默认目标 | 指向 `/sys/bus/i2c/devices/1-0036/trigger` | 仍绑定 OS08A20，需切换 |
| `IMX296` 触发入口 | 已有 `/sys/bus/i2c/devices/1-001a/trigger` | 可直接复用 |
| 输入触发 GPIO | `GPIO0_C6` 下降沿 | 保持不变 |
| 触发输出引脚 | `IMX296` 由 `GPIO1_D6(PWM14_M2)` 输出脉冲 | 与本次目标一致 |
| 资源冲突边界 | `GPIO1_D6` 与 `OS08A20 fsin-gpios`/`IMX415 pwm14` 冲突 | 文档中需显式说明 |

## 方案设计

1. **DTS 切换**：仅修改 `rk3588-lubancat-5io-trigger-dev-overlay.dts` 中 `trigger-path` 与注释，把目标从 OS08A20 改为 IMX296。
2. **驱动文义清理**：在 `drivers/input/misc/trigger-dev.c` 顶部注释中去掉“camera node must NOT have fsin-gpios”这类偏 OS08A20 的强绑定描述，改成通用 sysfs 触发说明。
3. **文档原地更新**：更新 `TRIGGER_DEV.md`、`Documentation/trigger-dev.md` 的默认路径、触发链路和调试命令为 IMX296。
4. **专题文档补充**：在 `docs/06_IMX296_cam1_适配记录.md` 增加 `trigger-dev` 默认链路切换说明和变更记录。
5. **最小验证**：重新编译 `rk3588-lubancat-5io-trigger-dev-overlay.dts`；如 `trigger-dev.c` 有代码改动，再编译 `drivers/input/misc/trigger-dev.o`。

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dts` | DT overlay | 定义 trigger-dev 输入 GPIO 和触发目标路径 | U-Boot overlay 加载流程 | `gpio0` / trigger-dev 平台驱动 | 本次核心配置切换 |
| `drivers/input/misc/trigger-dev.c` | 驱动源码 | trigger-dev 通用 GPIO 触发逻辑 | 内核输入子系统 / 板端触发链路 | GPIO / IRQ / VFS | 仅做注释文义清理 |
| `TRIGGER_DEV.md` | 项目文档 | 项目级 trigger-dev 使用说明 | 开发/调试人员 | DTS 与驱动实现 | 默认示例改为 IMX296 |
| `Documentation/trigger-dev.md` | 驱动文档 | 内核树内 trigger-dev 文档 | 开发/调试人员 | 驱动实现与 overlay | 默认命令改为 IMX296 |
| `docs/06_IMX296_cam1_适配记录.md` | 专题文档 | IMX296 触发链路和边界记录 | 后续维护者 | DTS/驱动/验证命令 | 增补本次切换说明 |
| `docs/00_文档总目录.md` | 文档入口 | 增加本次计划文档索引 | 全局 | `docs/` | 必须同步更新 |

## 关键函数 / 节点

| 函数/节点 | 所在文件 | 作用 | 输入 | 输出 | 调用方 | 被调对象 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `trigger_dev_parse_dt()` | `drivers/input/misc/trigger-dev.c` | 解析 `trigger-path` / `trigger-value` | DT 属性 | `trigger_path` / payload | `trigger_dev_probe()` | `device_property_read_*` | 逻辑不改 |
| `trigger_dev_write_once()` | `drivers/input/misc/trigger-dev.c` | 向目标 sysfs 写入一次触发值 | path + payload | 写入结果 | `trigger_dev_trigger_work()` | `filp_open()` / `kernel_write()` | 语义等价 `echo` |
| `trigger-path` | `rk3588-lubancat-5io-trigger-dev-overlay.dts` | 指定默认触发目标 | 设备路径字符串 | 触发目标定位 | trigger-dev 驱动 | IMX296 `trigger` sysfs | 本次从 `1-0036` 切 `1-001a` |
| `trigger_store()` | `drivers/media/i2c/imx296.c` | 处理 `echo 1 > .../trigger` | 用户写入值 | 单次脉冲输出 | userspace/trigger-dev | `imx296_trigger_once_locked()` | 已存在，无需改动 |

## 依赖项

| 依赖项 | 类型 | 用途 | 所在位置 | 使用入口 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `/sys/bus/i2c/devices/1-001a/trigger` | sysfs 节点 | IMX296 单次触发入口 | `imx296.c` | `trigger-dev` 的 `trigger-path` | 本次默认目标 |
| `GPIO0_C6` | 输入 GPIO | 外部硬触发输入口 | trigger-dev overlay | `input-gpios` + IRQ | 保持不变 |
| `GPIO1_D6(PWM14_M2)` | 输出引脚 | IMX296 触发脉冲输出 | IMX296 overlay + `imx296.c` | `echo 1 > .../trigger` | 本次不改其实现 |
| `master_fast_trigger` | 运行模式 | IMX296 单帧触发语义所需模式 | `imx296.c` sysfs | `run_mode` 节点 | 文档需提示 |

## 风险点

- 如果板端未加载 `cam1-imx296` overlay 或 `imx296` 未 probe，`trigger-dev` 写 `1-001a/trigger` 会失败（路径不存在）。
- 即便脉冲成功发出，若 `run_mode` 不在 `master_fast_trigger`，业务层可能看不到预期单帧行为。
- `GPIO1_D6` 与 `OS08A20` / `IMX415` 方案仍有复用冲突，不能并行启用。

## 验证方案

1. **宿主机静态验证**
   - `rk3588-lubancat-5io-trigger-dev-overlay.dts` 可编译为 `dtbo`；
   - 若 `trigger-dev.c` 有实质改动，`drivers/input/misc/trigger-dev.o` 可编译通过；
   - 文档中的默认触发路径统一为 `1-001a/trigger`。
2. **板端功能验证（待执行）**
   - 确认 `imx296` 节点存在：`ls /sys/bus/i2c/devices/1-001a/`
   - 设置模式：`echo master_fast_trigger > /sys/bus/i2c/devices/1-001a/run_mode`
   - 外部硬触发输入后观察：`dmesg | grep -i "trigger"`

## 当前验证结果

### 已验证项

- `rk3588-lubancat-5io-trigger-dev-overlay.dts` 已完成 `trigger-path` 切换，并可编译为 `dtbo`。
- `drivers/input/misc/trigger-dev.c` 注释已完成通用化清理，`trigger-dev.o` 可编译通过。
- `TRIGGER_DEV.md` 与 `Documentation/trigger-dev.md` 已统一为 IMX296 默认链路描述。
- `docs/06_IMX296_cam1_适配记录.md` 与 `docs/00_文档总目录.md` 已同步补充本次切换记录。

### 未验证项

- 板端外部硬触发到单帧采图的完整链路验证。
- 示波器复核 `GPIO1_D6` 电平波形。
- 板端 `GPIO0_C6` 连续触发压力下的稳定性与漏触发统计。

## TODO 清单

- [x] 新增本次切换 plan 文档
- [x] 修改 trigger-dev overlay 默认触发目标到 `IMX296`
- [x] 更新 trigger-dev 驱动注释与相关文档
- [x] 更新 IMX296 专题文档并补变更记录
- [x] 执行最小静态编译验证并回填结果
