# trigger-dev 双输入模式计划

## 需求背景

当前 `trigger-dev` 在 `debounce-ms = 0` 时，会把每个有效 IRQ 边沿直接当成一次触发。该路径适合干净的外部窄脉冲，但不适合机械按键输入：按键松开时的回弹可能再次形成下降沿，导致“按下触发一次、松开又触发一次”。

最初用户实际输入源是按键，因此需要在保留原有快速边沿模式的同时，增加一个“按键安全模式”，并支持通过命令行在两种输入模式之间切换。后续板级默认使用场景又切回干净外部脉冲，因此当前 5IO 默认值改回 `edge`，但仍保留 `button` 作为运行时可切换的安全模式。

在 `rk3588-lubancat-5io` 实测中，又发现一个运行时切换边界问题：`GPIO0_C6(GPIO_ACTIVE_LOW)` 从 `button` 切到 `edge` 后，上升沿也会继续触发。根因不在 `trigger-dev.c`，而在 `drivers/gpio/gpio-rockchip.c` 的 GPIO V2 中断类型切换路径没有在离开 `IRQ_TYPE_EDGE_BOTH` 时清掉硬件 `int_bothedge` 位，导致控制器仍按双沿上报中断。

## 目标与边界

### 目标

- 为 `trigger-dev` 增加运行时可切换的输入模式：
  - `button`：按键安全模式
  - `edge`：边沿快速模式
- 保持现有输出动作模式 `mode = gpio/sysfs` 不变。
- 对当前 `rk3588-lubancat-5io-trigger-dev-overlay.dts` 默认切到边沿快速模式。
- 修复 RK GPIO V2 在运行时从双沿切到单沿后仍误报双沿的问题。
- 更新 `trigger-dev` 与 `IMX296` 相关文档，明确新模式、切换命令与风险。

### 边界

- 不修改 `imx296.c` 的触发输出逻辑。
- 不修改 `os08a20.c` 及其 overlay 的 FSIN 逻辑。
- 不改变 `GPIO0_C6 -> trigger-dev -> /sys/bus/i2c/devices/1-001a/trigger` 这条默认输出链路。
- 不做板端烧录，仅做宿主机编译验证。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| 输入 IRQ 路径 | `debounce-ms = 0` 时每个 IRQ 直接排队触发 | 适合脉冲，不适合按键 |
| 按键状态机 | 只在 `trigger_dev_handle_state()` 中生效 | 需要 button 模式显式使用 |
| IRQ 触发类型 | 当前实现偏 `falling-only` | button 模式需要支持按下/松开感知 |
| RK3588 GPIO V2 运行时切换 | 从 `button` 切到 `edge` 后仍可能保留双沿 | 需在底层 `gpio-rockchip` 清 `int_bothedge` |
| 输出动作模式 | 已有 `mode = gpio/sysfs` | 需保持兼容，不能复用同名属性 |

## 方案设计

1. 增加独立输入模式枚举：`button` / `edge`。
2. 新增 sysfs 属性 `input_mode`，用于运行时切换输入模式。
3. `button` 模式：
   - IRQ 使用双沿感知；
   - 走防抖 + 电平采样 + `pressed` 状态机；
   - 仅“按下”产生一次触发，松开只更新状态不触发。
4. `edge` 模式：
   - 保留现有快速路径；
   - 每个有效活动沿直接排队触发，适用于干净脉冲输入。
5. 对 RK GPIO V2 补充运行时切边沿修复：从 `IRQ_TYPE_EDGE_BOTH` 切到 `RISING/FALLING/LEVEL` 时，显式清除 `int_bothedge`，避免双沿残留。
6. 在当前 5IO trigger-dev overlay 中增加输入模式默认配置，并把 IRQ 边沿配置调整为满足 edge 默认模式且仍兼容运行时切回 button。
7. 文档中明确：
   - `mode` 是输出动作模式（gpio/sysfs）
   - `input_mode` 是输入语义模式（button/edge）
   - `GPIO0_C6(GPIO_ACTIVE_LOW)` 在 `edge` 模式下应只响应下降沿

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `drivers/input/misc/trigger-dev.c` | 驱动源码 | 新增双输入模式、`input_mode` sysfs 和 IRQ 模式切换 | 内核输入子系统 / 板端调试 | GPIO / IRQ / VFS | 本次核心修改 |
| `drivers/gpio/gpio-rockchip.c` | GPIO 控制器驱动 | 负责 RK GPIO V2 边沿/电平 IRQ 类型切换 | GPIOLIB / `trigger-dev` 运行时切模式 | GPIO 控制器寄存器 | 本次修复 `int_bothedge` 残留 |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dts` | DT overlay | 指定当前板级默认输入模式与 IRQ 边沿 | U-Boot overlay 加载流程 | trigger-dev 驱动 | 当前板级默认改为 edge |
| `TRIGGER_DEV.md` | 项目文档 | 记录双输入模式、切换命令与默认链路 | 开发/测试 | DTS + 驱动实现 | 需更新 |
| `Documentation/trigger-dev.md` | 驱动文档 | 记录双输入模式工作原理与验证方法 | 开发/测试 | 驱动实现 | 需更新 |
| `docs/06_IMX296_cam1_适配记录.md` | 专题文档 | 记录 trigger-dev 输入模式变化对 IMX296 链路的影响 | 后续维护者 | DTS/驱动/验证命令 | 需补变更记录 |
| `docs/00_文档总目录.md` | 文档入口 | 增加本次计划索引 | 全局 | `docs/` | 需同步更新 |

## 风险点

- button 模式会引入防抖延迟，不适合作为当前默认外部脉冲模式。
- edge 模式仍可能对机械按键回弹敏感，因此文档需要明确使用场景。
- 运行时切换 `input_mode` 时除了同步调整 IRQ 类型，还必须在 RK GPIO V2 上清除 `int_bothedge`，否则模式语义会与硬件行为不一致。

## 验证方案

1. 宿主机静态验证：
   - `drivers/gpio/gpio-rockchip.o` 编译通过；
   - `drivers/input/misc/trigger-dev.o` 编译通过；
   - `rk3588-lubancat-5io-trigger-dev-overlay.dts` 可编译为 `dtbo`；
   - 文档中已出现 `input_mode` 切换说明。
2. 板端验证（待执行）：
   - `cat /sys/devices/platform/trigger-dev*/input_mode`
   - `echo button > /sys/devices/platform/trigger-dev*/input_mode`
   - `echo edge > /sys/devices/platform/trigger-dev*/input_mode`
   - 按键场景下验证：按下触发一次、松开不触发。

## 当前验证结果

### 已验证项

- `drivers/gpio/gpio-rockchip.o` 已可通过最小对象编译。
- `drivers/input/misc/trigger-dev.o` 已可通过最小对象编译。
- `rk3588-lubancat-5io-trigger-dev-overlay.dts` 已可编译为 `dtbo`。
- `trigger-dev.c` 已新增 `input_mode(button/edge)` 与 `stats` 中的模式/防抖观测信息。
- `gpio-rockchip.c` 已在 GPIO V2 从 `IRQ_TYPE_EDGE_BOTH` 切到单沿/电平时显式清除 `int_bothedge`，用于修复 `GPIO0_C6` 运行时切到 `edge` 后仍双沿触发的问题。
- `rk3588-lubancat-5io-trigger-dev-overlay.dts` 已默认切到 `trigger-input-mode = "edge"`、`IRQ_TYPE_EDGE_BOTH`、`debounce-ms = <10>`。
- `TRIGGER_DEV.md`、`Documentation/trigger-dev.md`、`docs/06_IMX296_cam1_适配记录.md` 已补充 `input_mode` 运行时切换说明。

### 未验证项

- 板端 `input_mode=button` 下“按下只触发一次、松开不触发”的真实按键复测。
- 板端 `input_mode=edge` 下对干净外部脉冲的边沿触发复测，尤其是 `GPIO0_C6(GPIO_ACTIVE_LOW)` 是否已只剩下降沿触发。
- 两种输入模式切换后的长期稳定性和极端抖动边界。

## TODO 清单

- [x] 新建本次双输入模式计划文档
- [x] 实现 `input_mode(button/edge)` 及运行时切换
- [x] 修复 RK GPIO V2 从双沿切到单沿时 `int_bothedge` 残留
- [x] 调整 5IO trigger-dev overlay 默认输入模式
- [x] 更新 trigger-dev 与 IMX296 相关文档
- [x] 完成最小编译验证
