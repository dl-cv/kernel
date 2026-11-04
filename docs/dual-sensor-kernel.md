# 双 Sensor Kernel 整合记录

## 来源

- 公共基线：`a60348a3590438fba3702758b0db4e9af6d78551`（`origin/rk3576`）
- IMX296 稳定线：`46bfc944c7e8a65fab4e018241b53d80b8f26a64`
- AR0521 最新线：`435f268f36ed8168c6db12c79638c8d907f9031c`

本分支先以无提交 merge 引入 IMX 线并形成独立合并提交，再以 `--no-commit` 合入 AR 线；冲突只在 `drivers/input/misc/trigger-dev.c` 逐点处理，其余自动合并项逐文件复核。两线的 merge 父提交保留在 Git 历史中。

## 板型与产物

| 板型 | 顶层 DTS | CSI0 dtsi | 产物 |
|------|----------|-----------|------|
| IMX296（默认） | `dlcvcam-rk3576.dts` | `dlcvcam-rk3576-csi0.dtsi` | `*-imx296.img`，默认同时写入根目录 `boot.img` |
| AR0521 | `dlcvcam-rk3576-ar0521.dts` | `dlcvcam-rk3576-csi0-ar0521.dtsi` | `*-ar0521.img` |

统一 `Image` 同时启用 `CONFIG_VIDEO_IMX296=y` 和 `CONFIG_VIDEO_AR0521=y`。正式构建入口在编译前检查 `.config`，缺失任一驱动时自动合并裁剪配置并执行 `olddefconfig`；编译后再校验两项配置和两个 DTB，全部通过才打包。IMX 保持 `0x1a`、1 lane、板载 37.125 MHz、PWM inverted；AR 保持 `0x36`、4 lanes、板载 27 MHz、PWM normal。

## 冲突与共享路径

- `trigger-dev` 统一采用 AR 的无 IRQ 重发门控：关闭时先发布 gate，再同步 IRQ、取消 debounce/work 并清空 pending。保留 IMX 的输出 GPIO 复位、按键释放及 `suppressed` 统计，同时保留 AR 的 `gate_irq` / `gate_pending_dropped` 诊断。
- IMX V39 early-done 延时完成路径保留；AR 的 CIF→ISP frame event bridge 同时保留。bridge 只向当前 CIF input subdev 投递事件，AR notifier 按 terminal fwnode 过滤，不会处理 IMX 的事件。
- IMX VICAP I0CLK reset、低频 PWM 周期恢复、FT/V39 尾行路径保留；AR GRR/ERS/restart、单帧重关和帧事件桥保留。
- 两个 DTB 共用板级公共 dtsi；sensor 专属触发节点和引脚标签由对应 CSI0 dtsi 设置。

## 验证与实机审查点

已通过：

- 新增静态集成测试 `tests/test_dual_sensor_kernel.py`
- AR GRR 58 用例、mode handoff 28 用例、PWM 5 用例、ERS 16 用例
- `trigger-dev` gate 行为测试
- frame interval 宿主测试、CIF/ISP frame bridge 31 项契约测试
- 两套 DTS 独立 `cpp + dtc` 语法编译

AR 已提供实际运行证据：构建编号 `2026110302`，源码 `b009c249399261573ccb36ae15563df4baf0158e` + dirty，后续提交 `435f268f36ed8168c6db12c79638c8d907f9031c`；镜像 19,833,856 字节，SHA-256 `0aea2c043917ba677f38c09f074b32457af5bbf39dd8810fbb17a4cfa16b84a3`。构建证据中的 `ar0521.c`（SHA-256 `a0b93872b192fb56dba0c0d37962e2e384c7495cec11bf7d3f47b8bd2722807a`）与提交 `435f268f` 的文件在换行归一后内容一致。

主 Agent 需实机确认：

1. 同一 `Image` 分别烧录 IMX 与 AR 包，确认 probe、DTB 绑定和 sensor 地址/lane 与硬件一致。
2. IMX 回归低频历史尾行、I0CLK reset、FT/V39 绿线及 trigger enable 切换。
3. AR 回归 GRR 首枪/逐枪重关、ERS 首枪、重启恢复，并确认 IMX 场景下 AR notifier 不消费事件。
4. 确认两个 boot 包内的 FDT 分别为对应 DTB，且默认 `boot.img` 为 IMX 包。