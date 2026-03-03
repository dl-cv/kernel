# trigger-dev 使用说明

通用 GPIO 输入设备，用于外部触发相机单帧采集。支持两种触发模式，可运行时切换。

## 模式说明

| 模式 | 说明 | 配置 |
|------|------|------|
| **模式 A** | 直接驱动 FSIN GPIO 脉冲，延迟最低 | `trigger-output-gpios` |
| **模式 B** | 通过 sysfs 写入触发，与 os08a20 共用 `fsin-gpios` | `trigger-path` |

- **模式 A**：trigger-dev 独占 FSIN GPIO，相机节点**不得**声明 `fsin-gpios`
- **模式 B**：os08a20 保留 `fsin-gpios`，trigger-dev 写入其 sysfs 触发，**同时支持硬触发和手动软触发**
- 若同时配置两者，可通过 sysfs `mode` 属性运行时切换

## 硬触发与软触发（模式 B）

当使用 `trigger-path` 且 os08a20 保留 `fsin-gpios` 时：

- **硬触发**：外部 GPIO 下降沿 → trigger-dev → 写入 os08a20 sysfs → os08a20 驱动 FSIN 脉冲
- **软触发**：用户手动执行 `echo 1 > /sys/bus/i2c/devices/1-0036/trigger` → os08a20 驱动 FSIN 脉冲

两种方式共用同一 FSIN 引脚，无需切换配置。

## Sysfs 接口

设备路径：`/sys/devices/platform/trigger-dev.*/`

| 属性 | 读写 | 说明 |
|------|------|------|
| `mode` | RW | 模式切换：`gpio` / `sysfs`（仅当同时配置两种模式时可用） |
| `pulse_count` | RW | 模式 A：每次外部触发产生的脉冲个数 (1–255) |
| `pulse_interval_us` | RW | 模式 A：脉冲间隔（微秒） |

### 示例

```bash
# 手动软触发（os08a20 的 trigger sysfs）
echo 1 > /sys/bus/i2c/devices/1-0036/trigger

# 查看当前模式
cat /sys/devices/platform/trigger-dev.*/mode

# 切换模式（需同时配置 output-gpios 和 trigger-path）
echo gpio > /sys/devices/platform/trigger-dev.*/mode
echo sysfs > /sys/devices/platform/trigger-dev.*/mode

# 模式 A：每次触发发 3 个脉冲，间隔 100μs
echo 3 > /sys/devices/platform/trigger-dev.*/pulse_count
echo 100 > /sys/devices/platform/trigger-dev.*/pulse_interval_us
```

## Device Tree 配置

### 常用属性

| 属性 | 说明 | 默认 |
|------|------|------|
| `input-gpios` | 外部触发输入 GPIO | 必填 |
| `interrupts` | 边沿类型（如 `IRQ_TYPE_EDGE_FALLING`） | 必填 |
| `debounce-ms` | 防抖时间(ms)，0=无防抖 | 0 |
| `trigger-output-gpios` | 模式 A：输出 GPIO（如 FSIN） | - |
| `trigger-output-pulse-us` | 单脉冲宽度(μs) | 50 |
| `trigger-output-pulse-count` | 每次触发的脉冲个数 | 1 |
| `trigger-output-pulse-interval-us` | 脉冲间隔(μs) | 0 |
| `trigger-path` | 模式 B：sysfs 路径 | - |
| `trigger-value` | 模式 B：写入内容 | "1" |

### Overlay 加载

```bash
# 加载 os08a20 相机 overlay 后再加载 trigger-dev
sudo fdtoverlay -i /boot/dtbs/.../rk3588-lubancat-5io.dtb \
  -o /tmp/merged.dtb \
  rk3588-lubancat-5io-cam1-os08a20-3840x2160-30fps.dtbo \
  rk3588-lubancat-5io-trigger-dev.dtbo
```

参考：`arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dts`

## 硬件说明（LubanCat-5IO）

- **输入**：GPIO0_C6，下降沿有效（平时高电平）
- **FSIN**：GPIO1_D6，由 os08a20 的 `fsin-gpios` 驱动，trigger-dev 通过 sysfs 写入触发
- 默认 overlay 使用 `trigger-path`，保留 os08a20 的 `fsin-gpios`，同时支持硬触发和软触发

---

## 模式 A/B 延迟与优化

### 典型延迟（外部触发 → FSIN 脉冲）

| 模式 | 典型延迟 | 主要耗时 |
|------|----------|----------|
| **模式 A** | ~50–150 μs | workqueue 调度 + 50μs 脉冲 |
| **模式 B** | ~1.5–2.5 ms | sysfs 路径 + I2C 曝光/增益/WB + 脉冲 |

**模式 A**：IRQ → debounce_work(0) → trigger_work → 直接 GPIO 脉冲，几乎无 I2C。

**模式 B**：IRQ → debounce_work → trigger_work → filp_open/kernel_write → os08a20 的 trigger_store → STREAMING + 曝光/增益/WB（约 8–10 次 I2C）+ 脉冲 + STANDBY。

### 可优化点

| 优化项 | 适用 | 说明 |
|--------|------|------|
| **高优先级 workqueue** | 模式 A/B | trigger-dev 使用 `alloc_workqueue(..., WQ_HIGHPRI)` 替代 `system_wq`，减少调度延迟 |
| **合并 debounce + trigger** | 模式 A/B | debounce_ms=0 时，可省去 debounce_work，IRQ 直接 schedule_work(trigger_work) |
| **跳过 3A 重写** | 模式 B | ✅ 已实现：若曝光/增益/WB 未变，仅 STREAMING + 脉冲 + STANDBY，可省 ~1 ms I2C |
| **批量 I2C** | 模式 B | ✅ 已实现：STREAMING + 曝光 + 增益合并为一次 I2C 传输 |
| **模式 A + fsin 共用** | 硬件 | 若需最低延迟且保留软触发，需硬件支持：FSIN 可由 SoC 或 os08a20 驱动，或使用多路复用 |
