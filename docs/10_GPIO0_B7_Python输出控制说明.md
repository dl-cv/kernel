# GPIO0_B7 Python 输出控制说明

## 文档范围

| 项目 | 说明 |
| --- | --- |
| 本文主责 | `GPIO0_B7` 裸 GPIO 的 Python 用户态控制方案：高/低电平、两类脉冲模式、运行前占用检查、精度分层决策 |
| 不写进本文 | `trigger-dev` 结果灯语义、IMX296 曝光链路调参细节 |
| 关联 | `docs/09_trigger-dev_结果指示灯_GPIO.md`、`docs/06_IMX296_cam1_适配记录.md`、`docs/plans/2026-04-24_gpio0_b7_python_control_plan.md` |

## 方案结论

- 默认采用 Python `gpiod`（`libgpiod` 绑定）直接控制，不新增内核驱动。
- 运行前先确认 `GPIO0_B7` 未被其他驱动占用，且 pinmux 为 GPIO 输出用途。
- 脉冲严格区分两类语义：
  - 默认低，输出高脉冲（idle=0, active=1）
  - 默认高，输出低脉冲（idle=1, active=0）

## LubanCat-5IO DT/uEnv 检查结论

| 检查项 | 位置 | 结果 | 结论 |
| --- | --- | --- | --- |
| `GPIO0_B7` 在 5IO 主 DTS 的占用 | `rk3588-lubancat-5io.dts` | 仅看到注释掉的 `gpio = <&gpio0 RK_PB7 ...>`；`vcc3v3_ekey_pcie_en` 仅定义 pinctrl 组 | 默认不构成已启用消费者占用 |
| 5IO trigger-dev overlay 输入脚 | `overlay/rk3588-lubancat-5io-trigger-dev-overlay.dts` | 使用 `GPIO0_C6`，不是 `GPIO0_B7` | 开启该 overlay 不会直接占用 `GPIO0_B7` |
| 非 5IO trigger-dev overlay | `overlay/rk3588-lubancat-5-trigger-dev-overlay.dts` | 调试输入脚使用 `GPIO0_B7` | 该文件面向 `lubancat-5`，不是 5IO 默认路径 |
| 5IO uEnv 默认配置 | `uEnv/uEnvLubanCat5IO.txt` | `rk3588-lubancat-5io-trigger-dev-overlay.dtbo` 默认注释 | 裸 GPIO0_B7 用户态直控通常不需要额外在 uEnv 打开该项 |

建议：

- 仅做 `gpiod` 用户态输出时，优先保持现状，不必新增/开启 overlay。
- 如果希望开机就固定 `GPIO0_B7` 初值或避免后续冲突，再新增专用 5IO overlay 并在 `uEnvLubanCat5IO.txt` 增加对应 `dtoverlay=` 行。

## 上下游链路

`业务脚本 -> gpio0_b7_control.py -> python3-gpiod/libgpiod -> /dev/gpiochipN line offset -> GPIO0_B7 -> 外部设备`

与现有触发链路关系：

`DTS/overlay 申请 GPIO0_B7(若存在) -> 内核消费者占用 -> 用户态申请可能失败`

## 文件表

| 文件路径 | 类型 | 作用 | 调用方 | 依赖项 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `gpio0_b7_control.py` | Python 脚本 | 提供 `inspect/high/low/pulse-high/pulse-low/pulse/latch-high/latch-low` 八类控制能力 | 板端 shell / Python 自动化任务 | `python3-gpiod`、`/dev/gpiochip*` | 默认按 `gpio0` + offset `15` 解析 |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5-trigger-dev-overlay.dts` | DTS overlay | 提供 `GPIO0_B7` 在历史调试场景中的占用线索 | 运行前冲突排查 | pinctrl/GPIO 子系统 | 若已加载且占用该脚，用户态申请会失败 |
| `drivers/pinctrl/pinctrl-rockchip.h` | 头文件 | 给出 `RK_GPIO0_B7=15` 的 bank 内 offset 定义 | 映射确认脚本与人工核查 | Rockchip pinctrl | 仅表示 bank 内偏移，不等于固定 gpiochip 编号 |
| `drivers/gpio/gpio-rockchip.c` | 驱动 | 说明 gpiochip base 与 bank 的关系 | 运行时映射理解 | GPIOLIB | 实机仍需以 `/dev/gpiochip*` 与 `gpioinfo` 为准 |

## 函数表

| 函数/方法 | 所在文件 | 作用 | 入参/出参 | 上下游依赖 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `resolve_chip_path()` | `gpio0_b7_control.py` | 将 `gpio0/gpiochipN/path` 解析为真实设备路径 | 入参：chip 选择器；出参：`/dev/gpiochipN` | `/dev/gpiochip*` 枚举 | 解析失败会列出可用 chip |
| `GPIOController.inspect()` | 同上 | 打印 chip/line 占用与方向信息 | 入参：无；出参：文本 | `gpiod.Chip.get_info()`/`get_line_info()` | 用于确认是否被内核消费者占用 |
| `GPIOController.set_level()` | 同上 | 输出固定高或低电平 | 入参：`level`、`hold_s`；出参：无 | `gpiod.request_lines()` | `hold_s<0` 为进程内持续保持 |
| `GPIOController.pulse_high()` | 同上 | 默认低，输出高脉冲 | 入参：次数/脉宽/间隔；出参：无 | `PulseSpec`、`pulse()` | 等价 `idle=0, active=1` |
| `GPIOController.pulse_low()` | 同上 | 默认高，输出低脉冲 | 入参：次数/脉宽/间隔；出参：无 | `PulseSpec`、`pulse()` | 等价 `idle=1, active=0` |
| `GPIOController.pulse()` | 同上 | 通用脉冲引擎 | 入参：`PulseSpec`；出参：无 | `time.sleep()`、gpiod line 请求 | 强制校验 idle/active 反相 |
| `start_latch_daemon()` | 同上 | 后台常驻保持电平，命令立即返回 | 入参：chip/offset/level 等；出参：无 | `subprocess`、`_latch-run` | 用于“设高后一直高，直到下次修改” |
| `stop_latch_daemon()` | 同上 | 停止后台保持进程并释放 line | 入参：latch 文件路径；出参：是否执行停止 | `os.kill()`、state 文件 | `latch-stop` 调用 |

## 依赖表

| 依赖项 | 类型 | 用途 | 所在位置 | 备注 |
| --- | --- | --- | --- | --- |
| `python3-gpiod` | Python 依赖 | 用户态申请 GPIO line 并切换电平 | 板端 Python 运行环境 | 缺失时脚本会报错并提示安装 |
| `/dev/gpiochip*` | 字符设备 | GPIO 控制入口 | 板端 `/dev` | 需要具备读写权限 |
| `gpioinfo/gpiodetect` | 调试工具 | 辅助确认 chip 映射与占用方 | 板端 shell | 建议与 `inspect` 命令交叉验证 |
| DTS/overlay 当前加载状态 | 配置依赖 | 决定是否已有内核消费者占用 GPIO0_B7 | `/boot` 与运行时设备树 | 占用冲突会导致用户态 request 失败 |

## 命令表

| 调试命令 | 执行位置 | 用途 | 示例 |
| --- | --- | --- | --- |
| `python3 gpio0_b7_control.py --chip gpio0 --offset 15 inspect` | 板端 shell | 查看 GPIO0_B7 当前占用方、方向、active_low 信息 | `python3 gpio0_b7_control.py --chip gpio0 --offset 15 inspect` |
| `python3 gpio0_b7_control.py high --hold-s 2` | 板端 shell | 拉高并保持 2 秒 | `python3 gpio0_b7_control.py --chip gpio0 --offset 15 high --hold-s 2` |
| `python3 gpio0_b7_control.py low --hold-s 2` | 板端 shell | 拉低并保持 2 秒 | `python3 gpio0_b7_control.py --chip gpio0 --offset 15 low --hold-s 2` |
| `python3 gpio0_b7_control.py pulse-high --count 3 --pulse-width-s 0.02 --interval-s 0.03` | 板端 shell | 默认低，输出 3 次高脉冲 | 同左 |
| `python3 gpio0_b7_control.py pulse-low --count 3 --pulse-width-s 0.02 --interval-s 0.03` | 板端 shell | 默认高，输出 3 次低脉冲 | 同左 |
| `python3 gpio0_b7_control.py pulse --idle-level 0 --active-level 1 --count 5 --pulse-width-s 0.01 --interval-s 0.01` | 板端 shell | 使用通用参数模式输出脉冲 | 同左 |
| `python3 gpio0_b7_control.py --chip gpio0 --offset 15 latch-high` | 板端 shell | 后台持续拉高，命令返回后仍保持高电平 | 再执行 `latch-low` 会自动切换 |
| `python3 gpio0_b7_control.py --chip gpio0 --offset 15 latch-low` | 板端 shell | 后台持续拉低，命令返回后仍保持低电平 | 再执行 `latch-high` 会自动切换 |
| `python3 gpio0_b7_control.py latch-status` | 板端 shell | 查看后台保持状态与 PID | `python3 gpio0_b7_control.py --latch-file /tmp/gpio0_b7_control_latch.json latch-status` |
| `python3 gpio0_b7_control.py latch-stop` | 板端 shell | 停止后台保持并释放 line | `python3 gpio0_b7_control.py latch-stop` |

## 脉冲模型与约束

- `idle_level`：空闲电平（`0` 或 `1`）
- `active_level`：有效电平（`0` 或 `1`）
- `pulse_width_s`：单个脉冲有效电平持续时间（秒）
- `count`：脉冲次数（`>=1`）
- `interval_s`：脉冲间隔（`>=0`）

约束：

- `active_level` 必须与 `idle_level` 相反。
- 脉宽必须大于 0。
- 若外设对上电状态敏感，首次请求 line 时的初值需与业务空闲态一致。

## 精度分层决策

- 毫秒级控制（例如 `>=1ms`）：优先保留当前 Python `gpiod` 方案。
- 亚毫秒且对抖动敏感（约 `0.1ms~1ms`）：先实测抖动再决定是否迁移到 C/实时线程方案。
- 微秒级且稳定性要求高（例如 `<100us`）：建议改为 PWM/内核定时/专用驱动，不建议依赖 Python 用户态翻转。

## 变更记录

| 日期 | 说明 |
| --- | --- |
| 2026-04-24 | 新增 GPIO0_B7 Python 直控专题：补齐高/低电平、两类脉冲语义、参数模型、命令示例与精度分层建议。 |
| 2026-04-24 | 脚本新增后台保持模式：`latch-high/latch-low/latch-status/latch-stop`，支持“设定高/低后持续保持直到下次修改”。 |
