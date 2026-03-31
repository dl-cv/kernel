# trigger-dev（GPIO 输入触发转发）说明

`trigger-dev` 用于把外部 GPIO 事件转换为一次触发动作。它支持两类彼此独立的模式：

- **输入模式**：决定如何解释 GPIO 输入
- **输出模式**：决定如何执行最终触发动作

## 当前工程默认行为

当前 `rk3588-lubancat-5io` 默认链路为：

```bash
GPIO0_C6 -> trigger-dev -> echo 1 > /sys/bus/i2c/devices/1-001a/trigger
```

最终由 `IMX296` 在 `GPIO1_D6(PWM14_M2)` 输出一次低电平脉冲。

## 输入模式

### `button`

- 走双沿 IRQ + 防抖 + 电平采样
- 使用 `pressed` 状态机
- 只在“按下”时触发一次
- “松开”只做状态释放，不触发

适合：

- 机械按键
- 继电器触点
- 有明显抖动的低速输入

### `edge`

- 走快速 IRQ 路径
- 每个有效活动沿直接排队一次触发
- 忽略 `debounce-ms`

适合：

- 干净的外部单脉冲
- 对时延敏感的窄脉冲输入

## 输出模式

### `mode = gpio`

直接控制 `trigger-output-gpios` 输出脉冲。

### `mode = sysfs`

向 `trigger-path` 写入 `trigger-value`，等价用户态 `echo`。

当前工程默认是：

```bash
echo 1 > /sys/bus/i2c/devices/1-001a/trigger
```

## 关键 sysfs 节点

设备路径：

`/sys/devices/platform/trigger-dev*/`

- `input_mode`：输入模式，`button` / `edge`
- `mode`：输出模式，`gpio` / `sysfs`
- `pulse_count`：GPIO 输出模式下的脉冲个数
- `pulse_interval_us`：GPIO 输出模式下的脉冲间隔
- `stats`：调试统计，包含 `input_mode`、`debounce_ms`、`irq/ok/fail`

## 推荐命令

```bash
# 查看当前模式
cat /sys/devices/platform/trigger-dev*/input_mode
cat /sys/devices/platform/trigger-dev*/mode

# 切输入模式
echo button > /sys/devices/platform/trigger-dev*/input_mode
echo edge > /sys/devices/platform/trigger-dev*/input_mode

# 查看统计
cat /sys/devices/platform/trigger-dev*/stats

# IMX296 触发验证
echo master_fast_trigger > /sys/bus/i2c/devices/1-001a/run_mode
echo 1 > /sys/bus/i2c/devices/1-001a/trigger
```

## 设备树相关属性

- `input-gpios`
- `interrupts`
- `trigger-input-mode`
- `debounce-ms`
- `trigger-output-gpios`
- `trigger-output-pulse-us`
- `trigger-output-pulse-count`
- `trigger-output-pulse-interval-us`
- `trigger-path`
- `trigger-value`

## 关键实现点

文件：`drivers/input/misc/trigger-dev.c`

- `trigger_dev_irq()`：根据 `input_mode` 选择快速边沿路径或按键安全路径
- `trigger_dev_debounce_work()`：button 模式下做延迟采样
- `trigger_dev_handle_state()`：只在按下沿累计一次触发
- `trigger_dev_trigger_work()`：执行最终 `gpio/sysfs` 动作
- `input_mode_show()` / `input_mode_store()`：运行时切换输入模式
- `mode_show()` / `mode_store()`：运行时切换输出模式

## 当前 5IO 默认设置

`rk3588-lubancat-5io-trigger-dev-overlay.dts` 当前默认：

- `trigger-input-mode = "button"`
- `interrupts = <... IRQ_TYPE_EDGE_BOTH>`
- `debounce-ms = <10>`

这样当前板子接按键时，按下只触发一次，松开不会因回弹再次触发。

## 常见问题

- 按下和松开都触发：通常是机械按键回弹，但驱动处于 `edge` 模式或 `button` 模式下防抖过小。
- `trigger-path` 不存在：目标传感器节点或 overlay 未生效。
- 脉冲已发出但业务不出单帧：优先检查 `IMX296` 是否已切到 `master_fast_trigger`。
