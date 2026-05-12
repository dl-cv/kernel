# trigger-dev 结果指示灯（OK 绿 / NG 红）

## 文档范围

| 项目 | 说明 |
| --- | --- |
| 本文主责 | `trigger-dev` 可选 GPIO 结果灯：行为语义、sysfs、`ok-led-gpios`/`ng-led-gpios`、5IO overlay 接线、与 IMX296 硬触发的时序关系 |
| 不写进本文 | `input_mode`/`trigger-path` 等通用说明（见 `Documentation/trigger-dev.md`）；曝光调参（见 `docs/07_IMX296_v4l2曝光与增益说明.md`） |
| 关联 | `docs/06_IMX296_cam1_适配记录.md`（整条硬触发链路）；`docs/plans/2026-04-17_trigger_dev_result_led_plan.md`（实施前设计） |

## 行为摘要

- 上电 probe 完成后两灯均为熄灭（逻辑 inactive）。
- 每次外部触发进入 `trigger_work` 处理**一条** pending 前，若 `result_led_enable=1`，先将 OK/NG 两路设为灭，再执行相机 `trigger` 转发。
- 用户态 `echo ok > result` / `echo ng > result`：互斥亮灯（先关对立灯再亮本灯）；`echo off > result`：两灯灭。
- `echo 0 > result_led_enable`：关闭驱动对结果灯 GPIO 的操作，并灭灯；此后触发与 `result` 写均不再改 GPIO，直到写回 `1`。

## 文件表

| 文件路径 | 类型 | 作用 | 调用方 | 依赖项 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `drivers/input/misc/trigger-dev.c` | 驱动 | 解析 LED GPIO、触发时灭灯、sysfs `result`/`result_led_enable` | 内核 platform、`trigger_work`、sysfs | `ok-led-gpios`、`ng-led-gpios` | 未配置 LED 时相关 sysfs 返回 `-ENODEV` |
| `arch/arm64/boot/dts/rockchip/overlay/rk3588-lubancat-5io-trigger-dev-overlay.dts` | DTS overlay | 绑定 `GPIO1_D2`/`GPIO1_D3` 为 OK/NG | U-Boot 加载 overlay | `&gpio1`、`pinctrl` | 极性默认 `GPIO_ACTIVE_HIGH`，与原理图不一致时改 DT |

## 函数表

| 函数/方法 | 所在文件 | 作用 | 入参/出参 | 上下游依赖 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `trigger_dev_leds_off_locked()` | `trigger-dev.c` | 两灯 inactive，`result_led=none` | `tdev`；持 `led_mutex` | `trigger_work`、sysfs | |
| `trigger_dev_leds_set_ok_locked()` | `trigger-dev.c` | 先灭 NG 再亮 OK | 同上 | sysfs `result` | |
| `trigger_dev_leds_set_ng_locked()` | `trigger-dev.c` | 先灭 OK 再亮 NG | 同上 | sysfs `result` | |
| `trigger_dev_trigger_work()` | `trigger-dev.c` | 每轮 pending 前条件灭灯，再 `pulse`/`sysfs` 触发 | workqueue | IMX296 `trigger` sysfs | |

## 依赖表

| 依赖项 | 类型 | 用途 | 所在位置 | 备注 |
| --- | --- | --- | --- | --- |
| `ok-led-gpios` | DT | OK 绿灯 | `trigger-dev` 节点 | con_id `ok-led` |
| `ng-led-gpios` | DT | NG 红灯 | `trigger-dev` 节点 | con_id `ng-led` |
| `/sys/devices/platform/trigger-dev.*/result` | sysfs | `ok`/`ng`/`off` | 驱动创建 | Python `open().write()` |
| `/sys/devices/platform/trigger-dev.*/result_led_enable` | sysfs | `0`/`1` | 驱动创建 | 关功能时灭灯 |

## 命令表

| 调试命令 | 执行位置 | 用途 | 示例 |
| --- | --- | --- | --- |
| `cat result` | 板端 shell | 查看当前逻辑状态 | `cat /sys/devices/platform/trigger-dev.0/result` |
| `echo ok > result` | 板端 shell | 仅 OK 灯亮 | 同上路径 |
| `echo 0 > result_led_enable` | 板端 shell | 关闭结果灯驱动 | 同上路径 |

## Python 示例

```python
from pathlib import Path

base = Path("/sys/devices/platform/trigger-dev")  # 或 glob trigger-dev.*
(base / "result").write_text("ok\n")
(base / "result").write_text("off\n")
(base / "result_led_enable").write_text("0\n")
```

## 变更记录

| 日期 | 说明 |
| --- | --- |
| 2026-04-17 | 初版：驱动实现、`5io` overlay 增加 `GPIO1_D2`/`D3`、本文与 `Documentation/trigger-dev.md` 同步。 |
