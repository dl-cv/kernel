---
name: trigger-dev 结果灯
overview: 在 `drivers/input/misc/trigger-dev.c` 增加可选 OK/NG GPIO 指示灯；probe 结束两灯灭；每次 `trigger_work` 处理 pending 前再灭灯；sysfs `result` 支持 ok/ng/off；`result_led_enable` 关闭驱动对灯的输出；互斥单灯亮。
status: 已落地（见 `docs/09_trigger-dev_结果指示灯_GPIO.md`）
---

# trigger-dev 结果指示灯（OK 绿 / NG 红）实现计划

## 需求与现有代码关系

- 硬件：`GPIO1_D2` → OK_LED（绿），`GPIO1_D3` → NG_LED（红）。设备树一般为 `&gpio1 RK_PD2`、`&gpio1 RK_PD3`（极性以原理图为准）。
- **默认开机**：probe 完成后两灯熄灭。
- **外部脉冲**：`trigger_work` 每轮迭代开头两灯熄灭，再执行相机触发。
- **有结果**：用户态写 `ok`/`ng` 后常亮，至下次触发灭灯；可写 `off` 显式灭灯。
- **互斥**：同一临界区内先关对立灯再亮本灯，禁止双灯同时亮。
- 挂接点：[`trigger_dev_trigger_work()`](drivers/input/misc/trigger-dev.c) 的 `while` 循环内、`pulse_output`/`write_once` 之前。

## Sysfs

| 属性 | 行为 |
|------|------|
| `result` | `ok` / `ng` / `off`；读回 `ok`/`ng`/`none` |
| `result_led_enable` | `0` 关（并灭灯）、`1` 开；关闭后触发与 `result` 均不改 GPIO |

## 设备树

- `ok-led-gpios`、`ng-led-gpios`（可选）；overlay 见 `rk3588-lubancat-5io-trigger-dev-overlay.dts`。

## 文档

- `Documentation/trigger-dev.md`、`docs/09_trigger-dev_结果指示灯_GPIO.md`、`docs/00_文档总目录.md`。

## 验证建议

- 上电两灯不亮；脉冲后先灭再触发；`echo ok|ng > result` 仅单灯亮；`echo off > result` 两灯灭；`result_led_enable=0` 后 GPIO 不再随触发/result 变化。
