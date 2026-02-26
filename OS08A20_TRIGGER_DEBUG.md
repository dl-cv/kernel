# OS08A20 `trigger` 不稳定出帧排查手册（Rockchip 平台）

> 目的：把“`echo 1 > /sys/bus/i2c/devices/1-0036/trigger` 有时 1～3 次才出 1 帧”的问题，拆解成可复现、可采集、可归因、可修复的步骤，方便你和 AI 来回迭代。

---

## 1. 现象与目标

- **现象**：执行 `echo 1 > /sys/bus/i2c/devices/1-0036/trigger` 后，**不是每次都能稳定获得 1 帧**；有时需要重复 `echo` 1～3 次才“看到”一帧。
- **目标**：在系统处于正常采集链路（CIF/CSI/ISP 已准备好）的前提下，实现 **“每次触发，都能稳定得到 1 帧（或至少 1 帧）”**。

> 注意：这里的“看到帧”可能来自用户态应用、ISP 输出、或 debug 打点。必须先明确“帧”是从哪里观测到的（详见 §5 的信息采集模板）。

---

## 2. 驱动中 `trigger` 的真实实现（关键前提）

文件：`drivers/media/i2c/os08a20.c`

### 2.1 `trigger` sysfs 节点做了什么

- `trigger` 是一个 **write-only** sysfs 属性：`echo 1` 会调用 `os08a20_trigger_one_frame_locked()`。
- **它不是“打开 stream”**，而是在 **streaming 已经为 true** 的情况下，进行一次“单帧触发”动作。

### 2.2 触发模式是否启用：`os08a20_use_fsin_trigger()`

触发路径只在以下条件满足时生效：

- DT 属性 `rockchip,camera-module-sync-mode` 必须是 `"slave"`（来自 `include/uapi/linux/rk-camera-module.h`）
- 并且设备树里必须绑定 `fsin-gpios`（驱动通过 `devm_gpiod_get_optional(dev, "fsin", ...)` 获取）

也就是说，**只有当 `sync-mode=slave + fsin_gpio 存在` 时，`trigger` 才被认为是 FSIN 单帧触发模式**。

### 2.3 触发流程（按驱动当前逻辑）

驱动大致做以下步骤：

1. 检查是否在 FSIN trigger 模式；检查 `os08a20->streaming`；检查 runtime PM 是否“正在使用中”
2. 计算帧周期（从 `max_fps` + `V4L2_CID_VBLANK` 推导）
3. 写 `0x0100 = 1`：把传感器从 standby 拉到 streaming
4. 重新写曝光/模拟增益（避免 standby/quick-stream 的瞬态造成曝光增益不对）
5. 等待 `fsin_settle_us`（默认 200us）
6. GPIO 打一个 FSIN 脉冲（默认 50us，极性由 DT `GPIO_ACTIVE_LOW/HIGH` 决定）
7. 等待一段时间（默认 **2 帧**，可由 DT 覆盖）
8. 写 `0x0100 = 0`：回到 standby

> 关键启示：现在的实现属于“**每次触发都开关 0x0100**”的策略；同时，等待时长对“有没有完整出一帧”非常敏感。

---

## 3. 可能原因树（把问题归类成可行动的方向）

下面每一类都能解释“有时要 echo 多次才出一帧”，但排查优先级不同。

### A 类：触发前提条件不满足（最常见、也最容易被忽略）

表现：你 echo 了，但驱动直接返回错误，触发流程根本没执行完成。

可能点：

- **A1**：`os08a20->streaming` 实际为 false → `trigger` 返回 `-EBUSY`
- **A2**：runtime PM 未在 use 状态 → `pm_runtime_get_if_in_use()` 失败 → `trigger` 返回 `-EIO`
- **A3**：DT 不是 slave 或没有 fsin gpio → `trigger` 返回 `-EINVAL`

定位方法：看 `echo` 的返回值、看 `dmesg` 是否打印了 `trigger capture: OK/FAIL`（如果连打印都没有，多半就是早退）。

### B 类：FSIN 波形/极性/电平导致传感器偶发识别失败

表现：驱动打印“触发 OK”，但传感器并未按预期输出帧；或驱动偶发写寄存器成功但仍无帧。

可能点：

- **B1 极性**：DT 的 `GPIO_ACTIVE_LOW/HIGH` 与传感器期望的有效边沿相反
- **B2 脉宽**：50us 在你的硬件链路上太窄（电平转换/线长/上升沿）
- **B3 pin 复用/驱动冲突**：FSIN 引脚被 pinctrl 或其他外设抢占/短暂切换

定位方法：示波器/逻分抓 FSIN；对照触发时刻；尝试把 `fsin-pulse-us` 提高到 500us～2ms。

### C 类：传感器寄存器未真正配置到“FSIN 外同步/触发”模式

表现：FSIN 并不是“每个脉冲都出一帧”的严格单帧触发，而是需要多次脉冲“锁定”、或只有在某些内部状态下生效。

为什么会发生：

- 驱动目前用“DT 的 slave+fsin_gpio”来判定走触发路径，但**寄存器表中未必显式开启了 FSIN 触发模式**（不同 sensor 的外触发通常需要寄存器开关）。

定位方法：对照 datasheet/厂商寄存器表；或者抓“连续 FSIN 脉冲”时的响应规律（是否需要 N 次才开始稳定出帧）。

### D 类：等待时间不足 / 帧周期估算错误 → 回 standby 太早

表现：触发动作执行了，但传感器还没来得及输出完整帧就被写回 standby；导致上层只在“碰巧相位合适”时收到帧。

典型原因：

- **D1**：驱动用 `V4L2_CID_VBLANK` 估算帧周期，但实际 VTS 可能被上层/3A 直接写寄存器改了（控件值没跟上）
- **D2**：低光/长曝光导致帧周期变长，而驱动仍按“名义 30fps”估算

定位方法：问题是否强相关“低光/长曝光/降 fps”；临时把 `trigger-frame-wait-us` 调大到 200ms 验证是否消失。

### E 类：接收端（CIF/CSI/ISP/用户态）丢掉了这次触发产生的唯一一帧

表现：驱动每次都 `trigger capture: OK`，但用户态/ISP 有时没拿到帧。

常见原因：

- **E1**：MIPI link 从静默到有数据时第一帧不稳定，被 CSI/CIF 丢弃
- **E2**：buffer 不足、skip 策略、同步组策略（尤其 quick-stream/多摄同步）导致这一帧被丢
- **E3**：ISP/用户态只在某些时刻拉取数据，单帧窗口太窄

定位方法：同时抓 `rkcif/rkisp/csi` 日志（overflow/size err/not active buffer 等）；看是否“帧确实到 CSI，但没进 buffer”。

### F 类：I2C/寄存器写偶发失败（电气/时序/总线问题）

表现：`trigger capture: FAIL (ret=...)` 或出现 I2C NACK/timeout。

定位方法：抓 `dmesg`；必要时提高 I2C 稳定性、检查上拉/时钟、降低总线速率等。

---

## 4. 一次性跑完的排查闭环（建议按此顺序执行）

> 目标：把问题快速归类为 A/B/C/D/E/F 中的 1～2 类，而不是“感觉不稳定”。

### Step 0：确认你观测“出帧”的位置

请明确你所谓“出一帧”是下面哪一种：

- ( ) **用户态应用收到 1 帧**（例如 v4l2 视频节点 `VIDIOC_DQBUF`）
- ( ) **ISP/AIQ 框架认为收到 1 帧**
- ( ) **内核 rkcif/rkisp 打印了 sof/frame_idx 变化**

不同观测点意味着不同责任边界（sensor vs 接收端 vs 用户态）。

### Step 1：触发时同时抓日志（最关键）

建议开两个窗口：

- 窗口 A：不断触发（手动或循环）
- 窗口 B：`dmesg -w` 观察日志

重点关注：

- `os08a20` 是否每次都有 `trigger capture: OK/FAIL`
- 同一时间段 `rkcif/rkisp/csi` 是否有：
  - overflow / size err
  - not active buffer
  - multi fs in oneframe
  - stream reset / quick stream on/off

### Step 2：检查是否属于 A 类“前提没满足”

判断标准：

- 如果 `echo 1 > trigger` 直接报错（返回码非 0），或 dmesg 完全没有 `trigger capture` 相关打印 → 优先查 A 类。

重点核对：

- pipeline 是否真正 `s_stream=1`（`os08a20->streaming` 必须为 true）
- runtime PM 是否把设备挂起了（触发路径用的是 `pm_runtime_get_if_in_use()`，而不是 `pm_runtime_get_sync()`）

### Step 3：如果驱动每次都 OK，但仍“看不到帧”

优先怀疑 E 类：接收端/缓冲/丢帧。

做法：

- 同步抓 `rkcif/rkisp` 日志是否出现 buffer/err
- 检查是否存在 quick-stream/sync-group 行为导致的丢帧窗口

### Step 4：如果 OK/FAIL 本身就不稳定

优先在 B/D/F 之间收敛：

- 调大 `fsin-pulse-us`、`fsin-settle-us`、`trigger-frame-wait-us` 看稳定性是否显著提升
- 若调大等待后稳定了 → 强指向 D（等待不足/帧周期估算不准）或 E（首帧丢）
- 若调大脉宽后稳定了 → 强指向 B（波形/极性/电平）
- 若看到 I2C 错误码 → F

---

## 5. 信息采集模板（复制给 AI 作为输入最有效）

请把下面内容填好（能大幅减少来回问答）。

### 5.1 基本信息

- **板卡/SoC**：
- **内核版本**：
- **os08a20 I2C 地址**：`1-0036`（确认是否一致）
- **触发节点**：`/sys/bus/i2c/devices/1-0036/trigger`
- **观测出帧位置**（勾选 §4 Step0）：

### 5.2 设备树关键信息（贴 dts 片段）

请贴出 sensor 节点里这些：

- `rockchip,camera-module-sync-mode = "slave";` 是否存在
- `fsin-gpios = <... GPIO_ACTIVE_LOW/HIGH ...>;`（请包含极性）
- `rockchip,fsin-pulse-us`
- `rockchip,fsin-settle-us`
- `rockchip,trigger-frame-wait-us`
- `rockchip,trigger-frame-margin-us`

### 5.3 触发时的内核日志（必须包含时间窗口）

请至少贴出以下两部分（从同一时间段截取）：

1) `os08a20` 的 `trigger capture: OK/FAIL` 打印（多次触发的连续输出）

2) 同时段的 `rkcif/rkisp/csi` 相关打印（特别是 error/warn）

### 5.4 波形信息（如果有示波器/逻分）

- FSIN 脉冲极性（高有效/低有效）：
- 脉宽（us）：
- 上升沿/下降沿是否干净：
- 触发频率（你 echo 的间隔）：

---

## 6. 快速缓解（不改代码：先用 DT 参数把问题“拉稳”）

> 这些用于验证“问题是不是时序/波形/等待窗口导致”，不是最终根治。

建议按顺序试，且每次只改一个变量，记录结果：

1) **增大 FSIN 脉宽**

- `rockchip,fsin-pulse-us = <500>;`（500us 起步）
- 若仍不稳：试 `<1000>` / `<2000>`

2) **增大 FSIN 前 settle**

- `rockchip,fsin-settle-us = <5000>;`（5ms）
- 进一步可试 `<20000>`

3) **强制扩大触发等待窗口**

- `rockchip,trigger-frame-wait-us = <200000>;`（200ms，先保守验证）
- 或者保持 wait_us=0，增加 `rockchip,trigger-frame-margin-us = <50000>;`

判断：

- 如果 wait_us 拉大后“几乎 100% 稳”→ D/E 概率大（等待不足/首帧丢/帧周期估算不准）
- 如果 pulse_us 拉大后“几乎 100% 稳”→ B 概率大（波形/极性/电气）

---

## 7. 长期修复方向（需要改驱动/或软硬件配合）

### 7.1 改进等待时间的“真实度”（解决 D 类）

当前等待的帧周期来自 `max_fps + VBLANK 控件` 推导，**可能低估真实 VTS**。

更稳的做法之一：

- 在触发前读回传感器真实的 `VTS`（例如 `0x380e/0x380f`），结合 `HTS` 与像素时钟估算真实帧时间
- 或至少确保所有改 VTS 的路径都会同步更新 `V4L2_CID_VBLANK`

### 7.2 尽量避免每次触发都开关 `0x0100`（解决 E/B/C 的一部分）

如果传感器支持“streaming 状态下，仅在 FSIN 才输出帧/或才对外同步”，通常会比频繁切换 standby/stream 更稳定：

- 把 0x0100 长期开在 streaming
- 用寄存器把输出/帧启动绑定到 FSIN（需要 datasheet 支持/寄存器配置）

### 7.3 允许“首帧丢”的工程应对（解决 E 类）

如果接收端经常丢第一帧，而你每次只给 1 帧窗口，那么“有时看不到”就会出现。

策略：

- 一次 trigger 内输出 >=2 帧，但上层只消费 1 帧（需要明确“只要 1 帧”的语义如何实现）
- 或者让接收端不要丢首帧（更难，需 CSI/CIF/ISP 时序配合）

### 7.4 明确/补齐传感器外触发寄存器配置（解决 C 类）

如果 datasheet 要求开启 FSIN/外同步模式寄存器，必须在：

- mode reg_list 或 global regs 中显式配置
- 或在 `__os08a20_start_stream()` / 触发前置步骤中配置

---

## 8. 常见误区（排查时避免走弯路）

- **误区 1**：把“用户态没拿到帧”直接归因于 sensor。  
  可能是 E 类（CIF/CSI/ISP 丢帧、buffer 不足、skip）。

- **误区 2**：只调 FSIN 脉宽，不考虑帧周期变化。  
  长曝光/降 fps 时，如果等待窗口不足，依然会“偶发无帧”。

- **误区 3**：忽略 `pm_runtime_get_if_in_use()` 的语义。  
  触发路径不会主动拉起 runtime PM；设备没处于 use 状态时会直接失败。

---

## 9. 你可以直接复制给 AI 的“问题描述模板”

把下面整段复制出去并填空即可：

```text
我在 Rockchip 平台上用 OS08A20（I2C: 1-0036）。驱动提供 sysfs: /sys/bus/i2c/devices/1-0036/trigger。
现象：echo 1 > trigger 不能稳定出 1 帧，有时要 echo 1～3 次才出一帧。
目标：每次 echo 都稳定得到 1 帧。

观测点：我通过【用户态取帧/ISP 输出/内核帧计数】观测到“出帧”。

DT 关键片段：
<贴 dts：sync-mode、fsin-gpios（含极性）、fsin-pulse-us、fsin-settle-us、trigger-frame-wait-us、trigger-frame-margin-us>

dmesg（触发时段）：
<贴 os08a20 trigger capture OK/FAIL>
<贴 rkcif/rkisp/csi 同时段日志>

是否和低光/长曝光相关：是/否（描述）
FSIN 波形（如有）：极性=，脉宽=us，上升沿=，频率=
```

