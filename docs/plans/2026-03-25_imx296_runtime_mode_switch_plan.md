# IMX296 运行模式切换计划

## 需求背景

当前 `IMX296` 驱动已经支持两类内部工作模式：

- `FREE_RUN`：普通连续出图模式
- `XTRIG_ONE_SHOT`：基于传感器主模式的快速触发路径

但现有用户态入口主要是 `V4L2_CID_IMX296_OP_MODE` 自定义控件，不适合板端直接通过终端脚本快速切换。当前需求是为 `IMX296` 增加一个可通过 `echo` 操作的运行模式切换入口，使用户在终端即可选择“连续出图模式”或“主模式快速触发模式”。

## 目标与边界

### 目标

- 保持 `IMX296` 默认工作在 `FREE_RUN / Normal Mode`。
- 新增一个板端可通过 `echo` 写入的 `sysfs` 节点，用于在 `FREE_RUN` 与主模式快速触发之间切换。
- 复用驱动现有模式切换逻辑，避免重复维护寄存器序列。
- 完成宿主机最小编译验证。

### 边界

- 本轮仅处理 `rk3588-lubancat-5io` 当前使用的 `IMX296` 驱动。
- 不臆造 `GD32` 触发协议内容。
- 不新增新的用户态字符设备或 ioctl。
- 不改变现有 `V4L2` 控件语义，只补 `sysfs` 入口。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| `trigger-mode` DT 属性 | 已支持，当前默认 `<0>` | 默认就是 `FREE_RUN` |
| 驱动内部模式枚举 | 已有 `IMX296_FREE_RUN` / `IMX296_XTRIG_ONE_SHOT` | 底层切换逻辑已具备 |
| V4L2 模式控件 | 已有 `V4L2_CID_IMX296_OP_MODE` | 但不满足 `echo` 操作需求 |
| 终端切换入口 | 暂无 | 需补 `sysfs` 节点 |

## 方案设计

1. 在 `drivers/media/i2c/imx296.c` 增加模式名称辅助函数，统一 `FREE_RUN` / 快速触发字符串映射。
2. 新增 `sysfs` 属性，例如：
   - `run_mode`
3. `run_mode` 的写入逻辑复用 `v4l2_ctrl_s_ctrl(sensor->op_mode_ctrl, ...)`：
   - `echo free_run > .../run_mode`
   - `echo master_fast_trigger > .../run_mode`
4. 读取 `run_mode` 时返回：
   - 当前待生效模式
   - 当前活动模式
   - 当前 `streaming` 状态
   - 可接受的模式名
5. 在 `probe` 中创建设备属性，在 `remove` 中清理。

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `drivers/media/i2c/imx296.c` | 驱动源码 | 新增 `sysfs` 模式切换入口 | 板端用户 / V4L2 / I2C core | 现有 `imx296_mode_switch()` | 本次主要修改 |
| `docs/plans/2026-03-25_imx296_runtime_mode_switch_plan.md` | 计划文档 | 记录设计与边界 | 开发者 / Agent | 当前驱动实现 | 本次新增 |
| `docs/06_IMX296_cam1_适配记录.md` | 专题文档 | 补充模式切换入口、调试命令、风险说明 | 开发者 / 调试人员 | `imx296.c` | 本次更新 |
| `docs/00_文档总目录.md` | 文档索引 | 收录 plan 与专题更新 | 开发者 / Agent | `docs/` | 本次更新 |

## 关键函数

| 函数/方法 | 所在文件 | 作用 | 输入 | 输出 | 调用方 | 被调对象 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `imx296_mode_switch()` | `drivers/media/i2c/imx296.c` | 执行实际模式切换 | `struct imx296 *` / 模式枚举 | 切换结果 | `V4L2 ctrl` / 新增 `sysfs` | 已有逻辑 |
| `imx296_set_ctrl()` | `drivers/media/i2c/imx296.c` | 处理 `V4L2_CID_IMX296_OP_MODE` | `struct v4l2_ctrl *` | 控制结果 | V4L2 ctrl core | 已有逻辑 |
| `run_mode_show/store()` | `drivers/media/i2c/imx296.c` | 提供 `echo` 读写入口 | `device` / 字符串 | `sysfs` 结果 | 板端 shell | 本次新增 |

## 依赖项

| 依赖项 | 类型 | 用途 | 所在位置 | 使用入口 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `V4L2_CID_IMX296_OP_MODE` | V4L2 自定义控件 | 驱动内部模式切换统一入口 | `imx296.c` | `run_mode_store()` | 已有 |
| `trigger-mode = <0>` | DTS 属性 | 保持默认 `FREE_RUN` | `rk3588-lubancat-5io-csi.dtsi` | `imx296_probe()` | 当前无需修改 |
| `sysfs` | 内核接口 | 板端 `echo` 切换模式 | `/sys/bus/i2c/devices/...` | shell | 本次新增 |

## 风险点

- 快速触发模式依赖 `IMX296` 硬件处于主模式路径；如果板级 `XMASTER` 拉法不匹配，切换后仍可能不出图。
- 运行时切换若发生在正在 stream 的过程中，驱动会执行停流/改寄存器/再起流，仍需板端实测确认稳定性。
- 若用户在快速触发模式下仍按连续出图方式取流，现象可能被误判为黑屏。

## 验证方案

1. 宿主机验证：
   - `drivers/media/i2c/imx296.o` 编译通过
   - `ReadLints` 无新错误
2. 板端待验证：
   - `cat /sys/bus/i2c/devices/1-001a/run_mode`
   - `echo free_run > /sys/bus/i2c/devices/1-001a/run_mode`
   - `echo master_fast_trigger > /sys/bus/i2c/devices/1-001a/run_mode`
   - 切换后重新查看 `cat .../run_mode`
   - 结合 `dmesg | grep -i imx296` 与取流命令验证行为差异

## 当前执行结果

### 已完成

- 已新增 `run_mode` sysfs 节点，挂在 `IMX296` 对应的 `I2C client` 设备下。
- 已复用 `V4L2_CID_IMX296_OP_MODE`，避免重复维护模式切换寄存器序列。
- 已支持以下典型写法：
  - `echo free_run > .../run_mode`
  - `echo master_fast_trigger > .../run_mode`
- 已保持默认 DTS `trigger-mode = <0>`，即默认 `FREE_RUN`。
- 已完成宿主机最小验证：`drivers/media/i2c/imx296.o` 编译通过。
- 已完成 `ReadLints` 检查，当前无新增 linter 错误。

### 待板端验证

- `/sys/bus/i2c/devices/1-001a/run_mode` 是否按预期出现。
- 正在 stream 时切换模式是否稳定。
- 主模式快速触发下，外部触发脉冲与板级 `XMASTER` 拉法是否匹配。

## TODO 清单

- [x] 新增 `run_mode` sysfs 节点
- [x] 复用现有模式切换逻辑，支持字符串写入
- [x] 保持默认 `FREE_RUN`
- [x] 完成最小编译验证
- [x] 更新现有 IMX296 文档与总目录
