# GD32 I2C 设备接入计划（已废弃）

## 状态说明

当前产品侧 **相机 XTRIG 已由 SoC 直连**：`RK3588` 通过 `PWM14_M2(GPIO1_D6)` 输出低电平单脉冲至 IMX296 触发脚；外部硬触发经 `GPIO0_C6` → `trigger-dev` → `imx296` `trigger` sysfs，与板上辅助 MCU 无关。

因此原「将 `GD32@0x2a` 纳入 I2C 设备模型」专题 **不再作为有效设计入口**。本内核树已删除 `gd32-mcu@2a` 设备树节点、`cam1-imx296` overlay 中对应片段、`drivers/misc/gd32_mcu.c` 及 `CONFIG_GD32_MCU`；**文档与实现均以 IMX296 + trigger-dev + PWM 触发链路为准**。

## 后续查阅

| 主题 | 文档路径 |
| --- | --- |
| IMX296、XTRIG、trigger-dev 全链路 | `docs/06_IMX296_cam1_适配记录.md` |
| trigger-dev 行为与 sysfs | `Documentation/trigger-dev.md` |

## 变更记录

| 日期 | 说明 |
| --- | --- |
| 2026-04-17 | 本文档由原实施计划改为废弃占位；专题正文已删除 `docs/07_GD32_I2C设备接入.md`，索引见 `docs/00_文档总目录.md`。 |
| 2026-04-17 | 从内核树移除 GD32 DTS/驱动/defconfig 符号，与产品 SoC 直连 XTRIG 一致。 |
