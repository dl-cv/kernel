# IMX296 曝光与增益：v4l2-ctl 调用与寄存器原理说明

## 1. 文档目标

本文面向以下两类读者：

- **应用开发者**：需要快速调用 `v4l2-ctl`（或封装同等能力）来读写 IMX296 的曝光和增益。
- **驱动/算法开发者**：需要理解 `V4L2 控件值 -> 传感器寄存器` 的换算关系与边界。

本文内容基于当前代码树中的 IMX296 驱动实现：

- `drivers/media/i2c/imx296.c`

---

## 2. AI 快速索引（Machine Readable）

```yaml
sensor: imx296
driver: drivers/media/i2c/imx296.c

controls:
  exposure:
    v4l2_id: V4L2_CID_EXPOSURE
    unit: us
    dynamic_range: true
    note: "范围受 frame_lines(height+vblank) 影响"
    register: SHS1(0x308d, 24-bit)

  analogue_gain:
    v4l2_id: V4L2_CID_ANALOGUE_GAIN
    unit: "raw code（非 dB）"
    min: 0
    max: 240
    step: 1
    register: GAIN(0x3204, 16-bit)

timing_constants:
  HMAX_DEFAULT: 1100
  OP_CLK_HZ: 74250000
  EXPOSURE_OFFSET_NS: 14260
  MIN_MEMORY_WAIT_LINES: 4

default_mode_on_5io_dts:
  trigger-mode: 1
  mode_name: master_fast_trigger
  impact: "exposure/vblank 控件在该模式下会被置为 inactive，建议先切 free_run 再调曝光"
```

---

## 3. 控件可调范围结论（当前代码）

## 3.1 增益（`analogue_gain`）

- 控件：`V4L2_CID_ANALOGUE_GAIN`
- 范围：`0..240`
- 步进：`1`
- 默认：`0`
- 映射：**直接写寄存器** `GAIN(0x3204)`，无额外换算。

即：`analogue_gain = N` -> `GAIN = N`（最终会被 clamp 到 `0..240`）。

## 3.2 曝光（`exposure`）

- 控件：`V4L2_CID_EXPOSURE`
- 单位：`us`（微秒）
- 范围：**动态范围**（不是固定值），由当前 `height + vblank` 决定。

驱动在初始化时先给了一个宽泛占位范围（`1..2000000`），之后会根据当前模式/时序动态更新为真实范围。

---

## 4. 关键前提：运行模式影响曝光控件

在该项目的 5IO 默认设备树中，IMX296 默认是：

- `trigger-mode = <1>` -> `master_fast_trigger`

此模式下，驱动会将 `exposure`/`vblank` 控件设为 inactive（不可用或不生效）。  
因此要稳定调曝光，建议先切到连续出图模式：

```bash
echo free_run | sudo tee /sys/bus/i2c/devices/1-001a/run_mode
```

查看当前模式：

```bash
cat /sys/bus/i2c/devices/1-001a/run_mode
```

---

## 5. 如何查询“当前真实可调范围”

先定位 IMX296 对应的 subdev 节点（`/dev/v4l-subdevX`）：

```bash
v4l2-ctl --list-devices
```

然后查询控件：

```bash
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls-ext
```

读取当前值：

```bash
v4l2-ctl -d /dev/v4l-subdevX --get-ctrl=exposure,analogue_gain,vblank
```

> 建议使用 `--list-ctrls-ext`，它会显示当前 min/max/step/default，更直观。

---

## 6. 默认时序下的曝光范围（可用于快速预估）

按当前驱动常量计算：

- `line_ns = round(HMAX_DEFAULT * 1e9 / OP_CLK_HZ)`
- `HMAX_DEFAULT = 1100`
- `OP_CLK_HZ = 74250000`
- 得 `line_ns = 14815 ns`

默认全分辨率高度 + 默认垂直 blank：

- `height = 1088`
- `vblank(default) = 30`
- `frame_lines = 1118`

得到默认下曝光范围约为：

- 最小：`29 us`
- 最大：`16518 us`（约 `16.5 ms`）
- 默认：`16370 us`

> 如果增大 `vblank`，最大曝光会继续增加；极限情况下可到秒级（但帧率会显著下降）。

---

## 7. 单位与换算公式（核心原理）

## 7.1 曝光：`exposure(us) -> SHS1`

### 步骤 A：先算每行时间

```text
line_ns = round(HMAX_DEFAULT * 1e9 / OP_CLK_HZ)
```

### 步骤 B：把 us 转成曝光行数

```text
request_ns = exposure_us * 1000

if request_ns <= EXPOSURE_OFFSET_NS:
    exposure_lines = 1
else:
    exposure_lines = round((request_ns - EXPOSURE_OFFSET_NS) / line_ns)

max_lines = max(1, frame_lines - MIN_MEMORY_WAIT_LINES)
exposure_lines = clamp(exposure_lines, 1, max_lines)
```

其中：

- `EXPOSURE_OFFSET_NS = 14260`
- `MIN_MEMORY_WAIT_LINES = 4`
- `frame_lines = height + vblank`

### 步骤 C：写入 SHS1

```text
SHS1 = frame_lines - exposure_lines
```

驱动将 `SHS1` 写入寄存器：

- `SHS1` -> `0x308d`（24-bit）

---

## 7.2 增益：`analogue_gain -> GAIN`

增益换算非常直接：

```text
gain_code = clamp(analogue_gain, 0, 240)
GAIN = gain_code
```

驱动写入：

- `GAIN` -> `0x3204`（16-bit）

注意：

- 这里是**原始码值**，不是该驱动里已经标定好的 dB 值。
- 若业务侧需要“dB 显示”，请在应用层建立单独映射表（结合 Sony 官方规格）。

---

## 8. 应用侧推荐调用流程

建议按下面顺序调用，避免“参数写了但看起来不生效”：

1. 确认 IMX296 节点存在并拿到 `/dev/v4l-subdevX`。
2. 切到 `free_run`（若当前是 `master_fast_trigger`）。
3. 读取 `--list-ctrls-ext` 拿到当前真实范围。
4. 应用层先 clamp 参数到 min/max。
5. 先设置 `vblank`（如需长曝光），再设置 `exposure` 和 `analogue_gain`。
6. 回读确认实际值。

示例：

```bash
# 1) 切换到 free_run
echo free_run | sudo tee /sys/bus/i2c/devices/1-001a/run_mode

# 2) 查看当前真实范围
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls-ext

# 3) 下发参数（示例：5ms 曝光，增益 120）
v4l2-ctl -d /dev/v4l-subdevX --set-ctrl=vblank=30,exposure=5000,analogue_gain=120

# 4) 回读
v4l2-ctl -d /dev/v4l-subdevX --get-ctrl=vblank,exposure,analogue_gain
```

---

## 9. 换算示例（默认 frame_lines=1118）

以下示例用于“验证应用层公式是否与驱动一致”：

- `exposure=29us` -> `exposure_lines=1` -> `SHS1=1117 (0x45d)`
- `exposure=5000us` -> `exposure_lines=337` -> `SHS1=781 (0x30d)`
- `exposure=10000us` -> `exposure_lines=674` -> `SHS1=444 (0x1bc)`
- `exposure=16518us` -> `exposure_lines=1114` -> `SHS1=4 (0x004)`

当请求超过当前最大曝光时，会被 clamp 到 `max_lines`，最终 `SHS1` 不会小于 `4`（默认帧高场景）。

---

## 10. 常见问题与排查

## 10.1 为什么设置 exposure 没生效？

常见原因：

- 当前模式是 `master_fast_trigger`，曝光控件 inactive。
- 用错节点（应在 IMX296 的 `v4l-subdevX` 上设置）。
- 目标值超范围被框架/驱动 clamp。

排查建议：

```bash
cat /sys/bus/i2c/devices/1-001a/run_mode
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls-ext
v4l2-ctl -d /dev/v4l-subdevX --get-ctrl=exposure,vblank,analogue_gain
```

## 10.2 为什么长曝光上不去？

因为最大曝光行数由 `frame_lines - 4` 决定，`frame_lines = height + vblank`。  
需要先增加 `vblank`，再设曝光。

## 10.3 为什么应用显示的“增益 dB”不准确？

当前驱动 `analogue_gain` 暴露的是 raw code，不是已换算 dB。  
应用显示 dB 需要独立映射逻辑（建议以 Sony 官方规格书为准）。

---

## 11. 给应用与 AI 的最小调用模板

```bash
# 输入：subdev 节点、曝光(us)、增益(raw)、vblank
# 输出：实际设置后的 ctrl 值

SUBDEV=/dev/v4l-subdevX
EXP_US=5000
GAIN=120
VBLANK=30

echo free_run | sudo tee /sys/bus/i2c/devices/1-001a/run_mode >/dev/null
v4l2-ctl -d "$SUBDEV" --set-ctrl=vblank=$VBLANK,exposure=$EXP_US,analogue_gain=$GAIN
v4l2-ctl -d "$SUBDEV" --get-ctrl=vblank,exposure,analogue_gain
```

---

## 12. 参考代码位置

- `drivers/media/i2c/imx296.c`
  - 控件初始化：`imx296_ctrls_init()`
  - 曝光换算：`imx296_exposure_us_to_lines_locked()`
  - 曝光下发：`imx296_apply_exposure_locked()`
  - 增益下发：`imx296_apply_analog_gain_locked()`
  - 控件写回入口：`imx296_set_ctrl()`
  - 模式切换与可见性：`imx296_mode_switch()` / `imx296_update_ctrl_visibility_locked()`
- `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi`
  - IMX296 默认 `trigger-mode`、`pwms`、电源等板级配置。

