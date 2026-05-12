# GPIO0_B7 Python 控制实施计划

## 背景

现场需要对 `GPIO0_B7` 提供用户态可控输出能力，覆盖高电平、低电平、可选脉冲次数，且明确区分：

- 默认低，输出高脉冲
- 默认高，输出低脉冲

## 目标

1. 使用 Python `gpiod` 直控 GPIO，不新增内核驱动。
2. 在执行输出前可检查运行时占用关系，避免与现有 DTS/驱动冲突。
3. 给出统一参数模型，避免脉冲极性歧义。
4. 输出落地专题文档并更新总目录索引。

## 实施范围

| 项目 | 内容 |
| --- | --- |
| 代码落地 | 新增 `gpio0_b7_control.py`（仓库根目录） |
| 能力范围 | `inspect/high/low/pulse-high/pulse-low/pulse/latch-high/latch-low/latch-status/latch-stop` |
| 不在本次范围 | 将脉冲升级为内核驱动或 PWM 硬实时输出 |

## 参数模型

| 参数 | 说明 | 约束 |
| --- | --- | --- |
| `idle_level` | 空闲电平 | `0/1` |
| `active_level` | 有效电平 | 与 `idle_level` 相反 |
| `pulse_width_s` | 单脉冲有效时长（秒） | `>0` |
| `count` | 脉冲次数 | `>=1` |
| `interval_s` | 脉冲间隔（秒） | `>=0` |

## 验证计划

| 步骤 | 命令 | 预期 |
| --- | --- | --- |
| 占用检查 | `python3 gpio0_b7_control.py inspect` | 可看到 line 是否已被占用及 consumer 名称 |
| 高电平输出 | `python3 gpio0_b7_control.py high --hold-s 1` | 目标脚输出高电平约 1 秒 |
| 低电平输出 | `python3 gpio0_b7_control.py low --hold-s 1` | 目标脚输出低电平约 1 秒 |
| 高脉冲输出 | `python3 gpio0_b7_control.py pulse-high --count 3` | 默认低，输出 3 次高脉冲 |
| 低脉冲输出 | `python3 gpio0_b7_control.py pulse-low --count 3` | 默认高，输出 3 次低脉冲 |
| 持续拉高 | `python3 gpio0_b7_control.py latch-high` | 命令返回后仍持续高电平，直到 `latch-low` 或 `latch-stop` |
| 持续拉低 | `python3 gpio0_b7_control.py latch-low` | 命令返回后仍持续低电平，直到 `latch-high` 或 `latch-stop` |
| 状态查看/停止 | `python3 gpio0_b7_control.py latch-status` / `latch-stop` | 可查看后台保持进程并释放占用 |

## 风险与回退

- 若 `GPIO0_B7` 已被内核消费者占用，则脚本会请求失败；需先释放冲突占用。
- 若需要微秒级稳定脉冲，应回退到 PWM/内核方案，不继续放大 Python 方案复杂度。

## 状态

已落地，详见 `docs/10_GPIO0_B7_Python输出控制说明.md`。
