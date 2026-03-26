# GD32 I2C 正式设备接入计划

## 需求背景

当前鲁班猫 RK3588 与外部 `GD32` MCU 通过 `i2c1` 相连，板端 `i2cdetect -r -y 1` 已确认 `0x2a` 地址存在 ACK，但 `rk3588-lubancat-5io.dts` / `rk3588-lubancat-5io-csi.dtsi` 中没有对应设备节点，内核源码中也没有可直接复用的 `GD32` I2C 驱动，因此该 MCU 目前只表现为“总线上有响应”，并未被 Linux 当作正式 I2C 设备管理。

## 目标与边界

### 目标

- 在 `i2c1` 下为 `GD32@0x2a` 增加正式 DTS 节点。
- 增加一个最小 `GD32` I2C 占位驱动，使内核可对该节点完成匹配与 probe。
- 为默认鲁班猫 RK3588 配置补齐该驱动的使能项。
- 完成宿主机最小编译验证，确保 DTS 和驱动对象可构建。

### 边界

- 本轮只解决“被 Linux 识别并正式绑定”为 I2C 设备，不实现 GD32 的具体业务协议。
- 不臆造 GD32 内部寄存器表、命令集、版本寄存器地址。
- 不修改 IMX296 现有链路和相机节点以外的无关功能。
- 不默认阻断后续根据实际协议扩展成完整功能驱动的空间。

## 现状分析

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| 板端 `i2cdetect` | `0x2a` 有 ACK | 硬件在线 |
| `rk3588-lubancat-5io*.dts*` | 无 `0x2a` / `gd32` 节点 | Linux 未实例化该 I2C 设备 |
| 内核驱动 | 无 `GD32` I2C 设备驱动 | 无法自动 probe |
| 用户态访问 | 只能通过 `i2c-dev` 直连总线 | 不是正式受管设备 |

## 方案设计

1. 在 `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` 的 `&i2c1` 下增加 `gd32_mcu: gd32-mcu@2a` 节点：
   - `compatible = "gd,gd32-mcu"`
   - `reg = <0x2a>`
   - `status = "okay"`
2. 在 `drivers/misc/` 下新增最小占位驱动：
   - 匹配 `gd,gd32-mcu`
   - 仅完成 probe/remove、日志输出和最小功能检查
   - 不访问未知业务寄存器
3. 在 `drivers/misc/Kconfig` / `drivers/misc/Makefile` 中接入新驱动。
4. 在 `arch/arm64/configs/lubancat_linux_rk3588_defconfig` 中默认使能该驱动。

## 影响文件

| 文件路径 | 类型 | 作用 | 谁会使用它 | 它依赖谁 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `arch/arm64/boot/dts/rockchip/rk3588-lubancat-5io-csi.dtsi` | 基础设备树 | 为 `i2c1` 增加 `GD32@0x2a` 节点 | 板级 DTS / I2C core | `rk3588-lubancat-5io.dts` | 本次新增 |
| `drivers/misc/gd32_mcu.c` | 内核驱动 | `GD32` 最小 I2C 设备占位驱动 | I2C core | `i2c` / `of` / `dev_*` | 本次新增 |
| `drivers/misc/Kconfig` | 构建配置 | 新增 `CONFIG_GD32_MCU` | Kconfig | `drivers/misc/gd32_mcu.c` | 本次新增 |
| `drivers/misc/Makefile` | 构建规则 | 编译 `gd32_mcu.o` | 内核构建系统 | `CONFIG_GD32_MCU` | 本次新增 |
| `arch/arm64/configs/lubancat_linux_rk3588_defconfig` | 内核配置 | 默认开启 `CONFIG_GD32_MCU` | 内核构建流程 | `drivers/misc/Kconfig` | 本次新增 |

## 关键函数 / 节点

| 函数/方法 | 所在文件 | 作用 | 输入 | 输出 | 调用方 | 被调对象 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `gd32_mcu_probe()` | `drivers/misc/gd32_mcu.c` | 绑定 `GD32` I2C 设备并打印识别日志 | `i2c_client` / `device_id` | probe 结果 | I2C core | `i2c_check_functionality()` / `dev_info()` | 本次新增 |
| `gd32_mcu_remove()` | `drivers/misc/gd32_mcu.c` | 卸载驱动占位资源 | `i2c_client` | remove 结果 | I2C core | 无 | 本次新增 |
| `gd32_mcu` | `rk3588-lubancat-5io-csi.dtsi` | `GD32@0x2a` 的正式 I2C 节点 | DTS 覆盖 | I2C client | I2C core | `gd,gd32-mcu` | 本次新增 |

## 依赖项

| 依赖项 | 类型 | 用途 | 所在位置 | 使用入口 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `aarch64-linux-gnu-` | 交叉工具链 | arm64 最小编译验证 | 宿主机 | `make ARCH=arm64 CROSS_COMPILE=...` | 当前宿主机为 Linux |
| `i2c1` | I2C 控制器 | 实例化 `GD32@0x2a` | `rk3588-lubancat-5io-csi.dtsi` | I2C core | 板端已确认地址有 ACK |

## 风险点

- 仅添加占位驱动并不会自动实现 GD32 的业务功能，后续仍需基于真实协议扩展。
- 若内核绑定占位驱动后，用户态直接用 `i2c-tools` 访问 `0x2a` 的行为可能需要重新确认。
- 如果 `GD32` 上电/复位时序不稳定，仅靠 DTS 节点和占位驱动不能解决硬件级通信异常。

## 验证方案

1. 宿主机最小验证：
   - `gd32_mcu.o` 可编译
   - `rk3588-lubancat-5io.dts` 可编译
   - overlay 叠加后 `GD32@0x2a` 节点仍存在
2. 板端待验证：
   - `dmesg | grep -i gd32`
   - `ls /sys/bus/i2c/devices/1-002a`
   - `i2cdetect -r -y 1` 观察地址状态是否变化

## 当前执行结果

### 已完成

- 已在 `rk3588-lubancat-5io-csi.dtsi` 增加 `gd32_mcu: gd32-mcu@2a`，默认 `disabled`。
- 已在 `rk3588-lubancat-5io-cam1-imx296-overlay.dts` 中将 `&gd32_mcu` 与 IMX296 一起启用。
- 已新增 `drivers/misc/gd32_mcu.c` 最小 I2C 占位驱动。
- 已接入 `drivers/misc/Kconfig` / `drivers/misc/Makefile`。
- 已在 `lubancat_linux_rk3588_defconfig` 中启用 `CONFIG_GD32_MCU=y`。
- 已完成宿主机最小构建验证：`gd32_mcu.o`、`rk3588-lubancat-5io.dtb`、`rk3588-lubancat-5io-cam1-imx296-overlay.dtbo` 编译通过。

### 待板端验证

- `dmesg` 中是否出现 `gd32-mcu` probe 日志。
- `/sys/bus/i2c/devices/1-002a` 是否出现且已绑定驱动。
- 驱动绑定后用户态对 `0x2a` 的访问方式是否符合实际使用预期。

## TODO 清单

- [x] 在 `i2c1` 下补 `GD32@0x2a` 节点
- [x] 新增 `gd32_mcu` 最小 I2C 占位驱动
- [x] 接入 `Kconfig` / `Makefile`
- [x] 补默认 `defconfig`
- [x] 完成最小编译验证
