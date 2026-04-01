# trigger-dev 使用说明

通用 GPIO 输入触发设备，用于把外部输入信号转换成一次相机触发动作。当前驱动同时支持：

- **输入模式**：`input_mode = button/edge`
- **输出模式**：`mode = gpio/sysfs`

这两个模式互相独立，不要混淆。

## 当前项目默认链路（LubanCat-5IO + IMX296）

- **输入 GPIO**：`GPIO0_C6`
- **默认输入模式**：`edge`
- **输出动作**：写 `echo 1 > /sys/bus/i2c/devices/1-001a/trigger`
- **默认 IMX296 模式**：`master_fast_trigger`
- **默认触发低脉宽**：`800us`
- **最终输出引脚**：`GPIO1_D6(PWM14_M2)` 产生一次低电平脉冲

即：

`GPIO0_C6 -> trigger-dev -> /sys/bus/i2c/devices/1-001a/trigger -> IMX296 -> GPIO1_D6(PWM14_M2)`

## 输入模式说明

| 输入模式 | 说明 | 适用场景 |
|------|------|------|
| `button` | 双沿感知 + 防抖 + 按下/松开状态机，只在按下时触发一次 | 机械按键、继电器、易抖动输入 |
| `edge` | 活动沿快速触发，每个有效边沿直接排队一次触发 | 干净的外部窄脉冲输入 |

- `button` 模式下，`debounce-ms` 生效。
- `edge` 模式下，驱动走快速路径，忽略 `debounce-ms`。
- `edge` 模式下的有效沿跟随 `input-gpios` 极性：`GPIO_ACTIVE_HIGH` 对应上升沿，`GPIO_ACTIVE_LOW` 对应下降沿。
- 当前 5IO overlay 默认使用 `edge` 模式，适合干净外部脉冲输入。

## 输出模式说明

| 输出模式 | 说明 | 配置 |
|------|------|------|
| `gpio` | 直接输出 GPIO 脉冲 | `trigger-output-gpios` |
| `sysfs` | 写指定 sysfs 节点，语义等价 `echo` | `trigger-path` |

- 当前工程默认是 `sysfs` 输出模式，目标为 `IMX296` 的 `trigger` 节点。
- 如果同时配置了 `trigger-output-gpios` 和 `trigger-path`，可通过 `mode` 在两者间切换。

## Sysfs 接口

设备路径：`/sys/devices/platform/trigger-dev*/`

| 属性 | 读写 | 说明 |
|------|------|------|
| `input_mode` | RW | 输入模式切换：`button` / `edge` |
| `mode` | RW | 输出模式切换：`gpio` / `sysfs` |
| `pulse_count` | RW | 输出模式为 `gpio` 时的脉冲个数 |
| `pulse_interval_us` | RW | 输出模式为 `gpio` 时的脉冲间隔 |
| `stats` | RO | 调试统计，含 `input_mode` 和 `debounce_ms` |

## 常用命令

```bash
# 查看 trigger-dev 节点
ls -d /sys/bus/platform/devices/trigger-dev*

# 查看当前输入/输出模式
cat /sys/devices/platform/trigger-dev*/input_mode
cat /sys/devices/platform/trigger-dev*/mode

# 输入模式切换
echo button > /sys/devices/platform/trigger-dev*/input_mode
echo edge > /sys/devices/platform/trigger-dev*/input_mode

# 输出模式切换（需同时配置 output-gpios + trigger-path）
echo gpio > /sys/devices/platform/trigger-dev*/mode
echo sysfs > /sys/devices/platform/trigger-dev*/mode

# 查看统计
cat /sys/devices/platform/trigger-dev*/stats

# IMX296 软触发验证
echo master_fast_trigger > /sys/bus/i2c/devices/1-001a/run_mode
echo 1 > /sys/bus/i2c/devices/1-001a/trigger
```

## Device Tree 常用属性

| 属性 | 说明 | 默认 |
|------|------|------|
| `input-gpios` | 外部输入 GPIO | 必填 |
| `interrupts` | 输入 IRQ 边沿配置 | 必填 |
| `trigger-input-mode` | 默认输入模式：`button` / `edge` | `edge` |
| `debounce-ms` | `button` 模式防抖时间 | 0 |
| `trigger-output-gpios` | 输出模式 `gpio` 的目标 GPIO | - |
| `trigger-output-pulse-us` | GPIO 模式单脉冲宽度(us) | 50 |
| `trigger-output-pulse-count` | GPIO 模式脉冲个数 | 1 |
| `trigger-output-pulse-interval-us` | GPIO 模式脉冲间隔(us) | 0 |
| `trigger-path` | 输出模式 `sysfs` 的路径 | - |
| `trigger-value` | `sysfs` 模式写入内容 | `"1"` |

## 当前 5IO 默认配置

`rk3588-lubancat-5io-trigger-dev-overlay.dts` 当前默认设置为：

- `trigger-input-mode = "edge"`
- `interrupts = <... IRQ_TYPE_EDGE_BOTH>`
- `debounce-ms = <10>`

这样当前板级上电后，`GPIO0_C6(GPIO_ACTIVE_LOW)` 会由驱动切到单下降沿触发，适合直接接干净外部脉冲；若后续改接机械按键，再运行时切回 `button`。

当前默认 `input_mode=edge` 时，`GPIO0_C6(GPIO_ACTIVE_LOW)` 只应在下降沿触发。若上升沿也触发，优先检查当前内核是否已经包含 `drivers/gpio/gpio-rockchip.c` 对 GPIO V2 `int_bothedge` 清零的修复。

## 风险与边界

- `button` 模式会引入防抖延迟，不适合作为当前默认外部脉冲模式。
- `edge` 模式对机械按键回弹敏感，可能出现松开时再次触发。
- 若内核缺少 Rockchip GPIO V2 的 `int_bothedge` 清零修复，运行时从 `button` 切到 `edge` 后可能错误表现为双沿都触发。
- `GPIO1_D6(PWM14_M2)` 与 `cam1 IMX415`、`cam1 OS08A20` 的触发相关配置存在复用冲突，不能同时加载相关 overlay。
