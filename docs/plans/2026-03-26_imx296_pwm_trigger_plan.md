# IMX296 PWM 单脉冲触发计划

## 需求背景

当前 `rk3588-lubancat-5io` 上的 `IMX296` 已具备 `FREE_RUN` 与主模式快速触发的运行模式切换入口，但外部触发脉冲原先由板上 `GD32` 输出到摄像头触发脚。现确认 `GPIO1_D6` 物理上已接到摄像头触发输入，希望改为由 `RK3588` 直接输出单个低电平脉冲来触发 `IMX296` 拍照，并保留板端 `echo` 调试入口。

同时需要确认该引脚与现有相机 overlay 是否冲突，并保证：

- 空闲电平为高；
- 触发脉冲为低电平有效；
- 默认低脉宽为 `5us`；
- 脉宽可以查看和修改；
- 通过 `echo` 能直接打出一次触发脉冲。

## 目标与边界

### 目标

- 在 `IMX296` 方案下使用 `GPIO1_D6 -> PWM14_M2` 作为触发脉冲输出。
- 在 `imx296.c` 中增加 PWM 单脉冲触发能力和 sysfs 调试入口。
- 默认脉冲宽度设为 `5us`，支持运行时查看和修改。
- 通过 `echo 1 > .../trigger` 直接输出一次低电平脉冲。
- 更新现有 `IMX296` 文档和文档总目录。

### 边界

- 仅处理 `rk3588-lubancat-5io` 的 `cam1 + IMX296` 方案。
- 不修改 `IMX415`、`OS08A20` 驱动本身的触发逻辑。
- 不在本轮强制启用 `CONFIG_PWM_ROCKCHIP_ONESHOT`。
- 不修改板级原理图，不验证真实示波器波形。
- 不处理多相机同时加载 overlay 的异常组合。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| `GPIO1_D6` 复用 | `rk3588s-pinctrl.dtsi` 中对应 `pwm14m2_pins` | 可作为 `PWM14_M2` 输出 |
| `pwm14` 控制器 | `rk3588s.dtsi` 中默认 `status = "disabled"` | 需由 `IMX296` overlay 显式启用 |
| `IMX415 cam1 overlay` | 已占用 `&pwm14` + `&pwm15` 输出 XVS/XHS | 与本方案冲突，但仅在加载 `IMX415` overlay 时成立 |
| `OS08A20 cam1 overlay` | 已把 `GPIO1_D6` 配成 `fsin-gpios` | 与本方案冲突，但仅在加载 `OS08A20` overlay 时成立 |
| `IMX296 cam1 overlay` | 当前未引用 `GPIO1_D6` / `pwm14` | 现有 `IMX296` 方案本身无占用 |
| `pwm-rockchip` 驱动 | 支持 one-shot 逻辑，但当前 `.config` 未开启 `CONFIG_PWM_ROCKCHIP_ONESHOT` | 当前先采用 `enable -> 延时 -> disable` 的单脉冲输出方式 |
| `imx296.c` | 已有 `run_mode` sysfs | 可继续扩展 `trigger` / `trigger_pulse_us` 入口 |

## 方案设计

1. 在 `IMX296` 设备树节点中增加 `pwm-names = "trigger"` 和 `pwms = <&pwm14 0 ... PWM_POLARITY_INVERTED>`，通过反相 PWM 保证空闲高、脉冲低。
2. 在 `rk3588-lubancat-5io-cam1-imx296-overlay.dts` 中新增 `&pwm14` 片段，将其启用并切到 `&pwm14m2_pins`。
3. 在 `drivers/media/i2c/imx296.c` 中：
   - 申请可选 PWM 资源；
   - 从 DT 读取默认脉宽，缺省为 `5us`；
   - 增加 `trigger_pulse_us` sysfs 节点用于查看/修改脉宽；
   - 增加 `trigger` sysfs 节点用于 `echo 1` 触发一次单脉冲；
   - 在当前内核配置下使用 `pwm_apply_state(enabled=true)` 立即起波形，再通过 `udelay/usleep_range` 等待脉宽结束后 `disable`，避免第二个周期。
4. 保留现有 `run_mode` 入口，不替换已有 `FREE_RUN` / `master_fast_trigger` 模式切换能力。
5. 更新 `docs/06_IMX296_cam1_适配记录.md` 和 `docs/00_文档总目录.md`，记录新的 PWM 触发能力、冲突边界和调试命令。

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `drivers/media/i2c/imx296.c` | 驱动源码 | 增加 PWM 单脉冲触发、脉宽配置和 sysfs 入口 | 内核媒体子系统 / 板端 shell | PWM framework / I2C / V4L2 | 本次核心实现文件 |
| `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` | 基础设备树 | 为 IMX296 节点增加 PWM 触发输出资源描述 | 基础 DTS | `pwm14` / pinctrl | 只改 IMX296 节点 |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dts` | DT overlay | 启用 `pwm14` 并切到 `pwm14m2_pins` | U-Boot overlay 加载流程 | `rk3588s.dtsi` / `rk3588s-pinctrl.dtsi` | 与 IMX296 方案绑定 |
| `docs/06_IMX296_cam1_适配记录.md` | 现有专题文档 | 记录 PWM 触发实现、冲突边界和调试命令 | 后续开发者 / 测试 | 代码与 DTS | 按旧文档原地更新 |
| `docs/00_文档总目录.md` | 文档总入口 | 补充本次 plan 和适配记录的索引说明 | 全局 | `docs/` | 必须同步更新 |

## 关键函数 / 节点

| 函数/方法 | 所在文件 | 作用 | 输入 | 输出 | 调用方 | 被调对象 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `imx296_probe()` | `drivers/media/i2c/imx296.c` | 申请 PWM 资源并初始化默认脉宽 | I2C client / OF node | probe 结果 | I2C core | PWM framework / regulator / clk | 需要新增 PWM 解析逻辑 |
| `imx296_trigger_once_locked()` | `drivers/media/i2c/imx296.c` | 输出一次低电平 PWM 触发脉冲 | `struct imx296 *` | 触发结果 | `trigger_store()` | `pwm_apply_state()` | 本次新增 |
| `trigger_show()` / `trigger_store()` | `drivers/media/i2c/imx296.c` | 提供 `echo 1` 的单脉冲触发入口 | `device` / 字符串 | sysfs 读写结果 | 板端 shell | `imx296_trigger_once_locked()` | 本次新增 |
| `trigger_pulse_us_show()` / `trigger_pulse_us_store()` | `drivers/media/i2c/imx296.c` | 查看/修改触发脉宽（微秒） | `device` / 数值字符串 | sysfs 读写结果 | 板端 shell | 脉宽配置字段 | 本次新增 |
| `&pwm14` + `&pwm14m2_pins` | `rk3588-lubancat-5io-cam1-imx296-overlay.dts` | 启用 `GPIO1_D6` 的 PWM14 M2 复用输出 | overlay | 节点状态 | U-Boot overlay | `rk3588s.dtsi` / `rk3588s-pinctrl.dtsi` | 本次新增 |

## 依赖项

| 依赖项 | 类型 | 用途 | 所在位置 | 使用入口 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `pwm14` | SoC PWM 控制器 | 输出触发脉冲 | `rk3588s.dtsi` | `pwms = <&pwm14 ...>` | 默认禁用，需要 overlay 开启 |
| `pwm14m2_pins` | pinctrl 复用 | 将 `GPIO1_D6` 切换为 `PWM14_M2` | `rk3588s-pinctrl.dtsi` | `pinctrl-0 = <&pwm14m2_pins>` | 物理脚就是 `GPIO1_D6` |
| `PWM_POLARITY_INVERTED` | PWM 极性 | 保证空闲高、脉冲低有效 | DT PWM 参数 | `pwms = <... PWM_POLARITY_INVERTED>` | 满足低电平触发要求 |
| `CONFIG_PWM_ROCKCHIP` | 内核配置 | 提供 Rockchip PWM framework 支持 | `.config` | PWM 驱动 | 当前已开启 |
| `CONFIG_PWM_ROCKCHIP_ONESHOT` | 内核配置 | 提供硬件 one-shot 路径 | `.config` | `pwm-rockchip.c` | 当前未开启，本次不依赖 |

## 风险点

- `GPIO1_D6/PWM14_M2` 与 `IMX415 cam1` overlay 的 `pwm14` 输出、`OS08A20 cam1` overlay 的 `fsin-gpios` 明确冲突，因此这些方案不能同时使用。
- 当前不启用 `CONFIG_PWM_ROCKCHIP_ONESHOT`，单脉冲将走 `enable -> 等待 -> disable` 路径，功能上可行，但“命令到脉冲开始”的确定性不应直接等同于 MCU 定时器 one-pulse 模式。
- `5us` 脉宽很短，虽然低电平宽度由 PWM 硬件生成，但软件 disable 时机仍需保守处理，避免跨到第二个周期。
- 若板级实际触发输入链路上还有额外上拉/二极管/隔离器件，低电平有效宽度可能与软件设置略有偏差，仍需示波器复核。

## 验证方案

1. 宿主机静态验证：
   - `IMX296` overlay 中已出现 `&pwm14` 使能片段；
   - `IMX296` 节点已具备 `pwms` / `pwm-names = "trigger"`；
   - `imx296.c` 能通过最小对象编译；
   - 新增 sysfs 节点名和文档描述保持一致。
2. 板端功能验证：
   - `cat /sys/bus/i2c/devices/1-001a/trigger_pulse_us`
   - `echo 5 > /sys/bus/i2c/devices/1-001a/trigger_pulse_us`
   - `echo 1 > /sys/bus/i2c/devices/1-001a/trigger`
   - `dmesg | grep -i "imx296.*trigger"`
   - 示波器观察 `GPIO1_D6`：空闲高、触发时出现单个低脉冲，宽度约为配置值
3. 业务链路验证：
   - 切到 `master_fast_trigger` 模式后，`echo 1 > .../trigger` 能触发单帧采图；
   - 切回 `free_run` 模式后，单脉冲仍可用于电气验证，但采图语义应以 `master_fast_trigger` 为准。

## 实施结果

- 已在 `drivers/media/i2c/imx296.c` 中新增可选 `trigger` PWM 资源解析、`imx296_trigger_once_locked()` helper，以及 `trigger` / `trigger_pulse_us` sysfs 节点。
- 已在 `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` 的 `dcphy1_imx296` 节点中新增 `pwm-names = "trigger"`、`pwms = <&pwm14 0 10000000 PWM_POLARITY_INVERTED>` 和 `rockchip,trigger-pulse-us = <5>`。
- 已在 `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-cam1-imx296-overlay.dts` 中新增 `&pwm14` 使能片段，并切到 `&pwm14m2_pins`，让 `GPIO1_D6` 输出 PWM。
- 当前单脉冲实现采用 `enable -> 等待脉宽 -> disable` 路径，未依赖 `CONFIG_PWM_ROCKCHIP_ONESHOT`。

## 当前验证结果

### 已验证项

- `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- drivers/media/i2c/imx296.o` 通过。
- `rk3588-lubancat-5io-cam1-imx296-overlay.dts` 可重新编译为 `dtbo`，并可继续叠加到 `rk3588-lubancat-5io.dtb`。
- 新增的 `trigger` / `trigger_pulse_us` / `run_mode` sysfs 节点已统一收敛到同一个 attribute group。

### 未验证项

- 未在板端读取到新增 sysfs 节点并实际执行 `echo 1 > .../trigger`。
- 未在示波器上确认 `GPIO1_D6` 的空闲高、触发低和 `5us` 低脉宽。
- 未在 `master_fast_trigger` 模式下获取到“脉冲触发单帧采图成功”的板端证据。

## TODO 清单

- [x] 为本次 `IMX296 + PWM14(GPIO1_D6)` 触发能力补 plan 文档
- [x] 在 `IMX296` DTS/overlay 中启用 `pwm14m2` 并为 sensor 节点增加 PWM 触发属性
- [x] 在 `imx296.c` 中增加 PWM 单脉冲 helper 和 sysfs 入口
- [x] 更新现有 `IMX296` 适配文档和总目录
- [x] 做最小静态检查并整理板端验证命令
