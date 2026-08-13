# IMX296 底部绿线（2026-08-10）— ceiling-force 排查记录

> 状态：**#1009 源码已改（未刷板）——C1+C2：gate 仅 warm==0 后递减；IRQ 路径 warm 期 mi_drop 并消耗 warm。见 §12。**  
> 板端内核（当前联调仍为）：`#2026081008`；树内待刷：`#2026081201` `skip=2 drop=2 warmup=8 pub_gate=6 force_post=70000`  
> 历史复现基线：`#2026081003`；CamOS 已弃 discard forced-clear 开闸，warmup=脉冲预算（post=3 回退，见 §11.4）  
> 早期复现时 CamOS：warmup 默认 **2**，`residual_discard` forced clear，`post_drop_ms=100`  
> 产物：板端 `/tmp/green_capture_run/`（`summary.json`、绿图、`dmesg_full.txt` / `dmesg_ft.txt`）  
> 相关：`IMX296_N1相位滞后_诊断与修复记录.md`（N-1 / ceiling-force 背景）、`IMX296_绿线修复_内核修改与验证记录.md`（早期绿线策略）

---

## 1. 现象（用户 + 存图）

- 触发模式下底部约 **8 行绿带**（经典 NV12 UV 未写完），**概率性**。
- **不固定** mon→trig 第 1 帧或第 5–6 帧；也可能首帧；常见 **连续 2 帧**。
- **触发下 ROI set/reset** 后同样概率出现。
- 用户指名样例（已像素核实）：
  - `images/2026-08-10/NG/20260810_164309_812_042048.jpg` bot8≈(70,88,40) gdom≈88%
  - `images/2026-08-10/NG/20260810_164310_668_456645.jpg` bot8≈(70,88,40) gdom≈90%（连续下一张）
  - 同段还有 pure 绿底：`...164301_870...` bot8≈(6,141,5)

自动化误判过「0 绿」：software 路径/取图缓存与 **line0 真实节拍 + 产品存图** 不一致；**以用户 NG + 带 n1trace 的复现为准**。

---

## 2. 受控复现（协议一轮，`#2026081003`，n1trace 短开）

脚本：`/tmp/green_capture_run.py` → 板端 `/tmp/green_capture_run/`

| 段 | 结果 |
|----|------|
| **A mon→software head16** | **GREEN ×2：sw#3、sw#4 连续** gdom≈84% bot8≈(70,87,41) |
| B mon→line0 | 本轮 `debug/trigger_once` 在 line0 下 tag 不涨，**采样无效**（不能当 line0 已好） |
| C ROI 592 / reset | 本轮 0 绿（未打中） |

sw 序列：

```
sw0–2  暗底、不绿
sw3–4  GREEN 带（连续）
sw5+   正常 (92,87,74)
```

CamOS 同次切换：

```
warmup 2/2, residual_discard=1 forced clear, post_drop_ms=100, mode=software
warmup dt≈0.63s + quarantine 0.1s
```

---

## 3. n1trace 对齐结论（问题点）

进 FT 后 arm：

```
isp_ft_arm height=1088 skip=2 drop=2 warmup=8
ceiling_us=70790 force_post=70000
```

### 3.1 已确认的机制

1. **V39 + IMX296 FT 上 MI OFFS 长期为 0**（`y_shd=y_st=cb_*=0`）  
   → early-done **几乎 100% 走 `ceiling-force`**，再 `early complete` + `mi_pub WORK early=1`。  
   （与 N-1 文档一致：不能 abandon 到下一拍 MI FE。）

2. 绿带形态 = **底 ~8 行 UV 不完整**（与 wait_line 终端 8 行历史问题同类）。

3. **force→complete 的 post-hold 在绿帧上已经跑满**  
   - warmup 段 hold ≈ **138ms**（force 70 + ~¾ residual）  
   - warm 用尽后 ≈ **123ms**  
   - complete 时 **`warm_left>0`**，**不是**「下一 SOF 打断 stale arm 导致 hold 被砍」。

4. **同一条 ceiling-force 路径上：前段发布可绿，后段发布正常**  
   - 绿帧 hold **更长**（warmup），后段 ok 帧 hold **更短**仍干净。  
   - ⇒ **否定「再盲目加长 force_post 就能根治」** 作为主因假设。

### 3.2 mi_pub 与用户帧粗对齐（drop=2 耗尽后）

| mi_pub（约） | warm_left@complete | 用户 sw#（约） | 画质 |
|--------------|--------------------|----------------|------|
| （skip/drop 阶段） | — | 被内核丢掉 | — |
| pub1–3 | 5→3 | sw0–2 | 暗、不绿 |
| **pub4–5** | **2→1** | **sw3–4** | **GREEN** |
| pub6+ | 0 | sw5+ | 正常 |

cnt=1、2 的 `early complete` 日志侧 **无对应 mi_pub**（与 `mi_drop`/半开会话交错）——发布门槛与 drop 计数**不完全同一条可见路径**，仍属待挖点。

### 3.3 一句话问题点

> **进 FT 后早期已发布的 ceiling-force 帧，在 post-hold 已满的情况下仍可能带未完成 UV；管道稳定后同一 force 路径不再绿。**  
> 产品上再叠加：**CamOS warmup=2 过短**，脏发布会漏到用户可见 tag；ROI 重 arm 会再次打开同一窗口。

不是：

- 单纯 mon→trig「固定第 N 帧」  
- 单纯 hold 从 45ms→70ms 不够再加一点就能完事  
- 与业务 NG 分类误报（像素已是真绿带）

---

## 4. 今日 NG 簇形态（辅助）

`2026-08-10/NG` classic 绿约 22 张 / 11 簇：多为 **1×PURE 或 2×BAND 连发**，簇间隔随机——与「进 FT / 重 arm 后短窗口」一致。

16:43 用户簇与内核：

```
16:42:56 再进 FT arm skip=2 drop=2 force=70ms
16:43:01 seq≈87 PURE
16:43:09–10 seq≈90–91 BAND×2   ← 用户两张
随后恢复正常
```

---

## 5. 已做 / 未做的改动（避免继续盲改）

| 项 | 状态 |
|----|------|
| force_post 45→70、warmup hold 85×8、residual floor 抬高（`#1002/#1003`） | 已刷；**绿线仍在** |
| SKIP_BUF 2↔4 来回 | 只影响前几枪是否出图；**单独不能当根治** |
| 仅 CamOS warmup 2→6 A/B（更早） | 当时矩阵显示不够；**需在长 hold 内核上重验** |
| 本地树 `DLCVCAM_BUILD_VERSION=1004` + 源码 SKIP=4 | **未证明已刷**；板端复现时仍是 **1003 / drop=2** |
| line0 外部真触发 + n1trace | **未完成**（本轮 line0 采样失败） |
| ROI 绿 + n1trace 打中 | **未完成** |

---

## 6. 还差什么（排查清单）

优先级从高到低：

1. **[P0] 验证「隔离早期脏发布」是否足够**  
   - CamOS `SMARTCAM_TRIGGER_WARMUP_FRAMES≥6`（吃掉 sw3–4 一类脏帧）  
   - 与内核 `drop/skip` 对齐：用户首枪在 quarantine 之后  
   - 重跑 `/tmp/green_capture_run.py`：mon→sw 连续多轮，**GREEN_COUNT=0**

2. **[P0] line0 真路径**  
   - 外部触发或产品存图，不要只靠 `debug/trigger_once`  
   - 对时 `trigger-dev seq` ↔ `mi_pub` ↔ NG 文件名

3. **[P1] 为何 early force 帧脏、后期同路径干净**  
   - `mi_pub` 时 `vb_seq` / buffer index 与 `sof` 是否严格一对一  
   - FT 入口 dummy×3 / 半开帧是否污染前几块 buffer 的 UV  
   - cnt=1,2 complete 无 mi_pub 的精确分支（drop vs skip_frame vs 无 buf）

4. **[P1] ROI 重 arm**  
   - 高度 592/832/1088 切换时是否重新 `isp_ft_arm` + 短 quarantine  
   - 与 mon→trig 是否同一类早期脏 pub

5. **[P2] 若隔离不够：再动完成条件**  
   - 仅对 `warm_left>0` 的 force 完成 **禁止 pub / 强制 drop**（内核）  
   - 或 force 路径额外等 MI FE（可能拉回 N-1，需极度谨慎）  
   - **禁止**无 n1trace 对照的大跳 force_post / drop=10

---

## 7. 修复方向（最小、可验证）

### 7.1 产品向（优先做）

| 层 | 动作 | 理由 |
|----|------|------|
| CamOS | `SMARTCAM_TRIGGER_WARMUP_FRAMES=6`（runtime_settings 或等价） | 复现上脏帧在用户 sw#3–4；warmup=2 盖不住 |
| 内核 | 保持 ceiling-force + 现 hold；`SKIP_BUF/SKIP_EARLY` 与 warmup **对齐且 ≤warmup**（建议 4/4 或维持 2/2 但 warmup≥6） | 半开帧仍丢；用户首枪在 gate 打开后 1:1 |
| 验证 | mon→sw ≥3 轮 head16；ROI set/reset；line0 人工/外部触发 | GREEN=0 且首枪可出图 |

### 7.2 不做（除非 7.1 失败且有新 n1trace）

- 再把 `force_post` 加到 100ms+ 当唯一手段  
- `SKIP_BUF≥10`（已知「前 10 枪不出图」）  
- 关掉 ceiling-force 回到 abandon（N-1 回归）

### 7.3 成功标准

- mon→software / line0 / ROI reset：用户可见帧 bot8 gdom&lt;40% 且无 pure 绿底  
- 模式切换后 **第 1 次用户触发有图**（warmup 在 gate 内消耗）  
- dmesg arm 与 n1trace 抽检仍为 ceiling-force（N-1 不回退）

---

## 8. 时间线摘要

| 时间 | 事件 |
|------|------|
| 早前 | N-1 ceiling-force 合入；绿线靠 residual+hold+drop |
| 2026-08-10 | 用户再报随机底绿 + ROI 绿；hold 加长刷 `#1002/#1003` |
| 16:43 | 用户 NG 实锤 BAND×2 + PURE；arm 已是 force=70ms |
| 17:08 | 受控 mon→sw 复现 GREEN sw#3–4 + 完整 n1trace |
| 结论 | 早期 ceiling-force **已发布**帧脏；hold 满仍可绿；后段同路径干净 |

---

## 9. 板端/树状态备忘

- 复现内核：`#2026081003` skip=2 drop=2  
- 本地未提交 diff 曾指向 1004 + SKIP=4（**以实机 uname/dmesg arm 为准**）  
- n1trace 默认关；抓数时短开，结束后必须 `echo 0 > .../n1trace`  
- 备份示例：`/root/boot-backups/boot-before-2026081003.img`

---

## 10. 修复实施与验证日志（2026-08-10 晚）

> 状态：**#1008 mon→sw 4 轮 TOTAL_GREEN=0（2026-08-10 18:01）**  
> 板端：`#2026081008` pub_gate=6（仅 drop+warm 后计数）+ CamOS warmup=12  
> 仍待：ROI / line0；CamOS residual_discard forced clear 仍是产品侧缺口。

### 10.1 问题点（不变）

- FT 进线后 **ceiling-force** 几乎 100%（MI OFFS=0）。
- **post-hold 已满**（~123–138ms）的 early `mi_pub` 仍可底 8 行绿 UV；同路径稍后干净。
- 产品缺口：CamOS warmup 过短 + residual_discard **forced clear** 会在 sample 未到时提前开闸。

### 10.2 版本轨迹

| 构建 | 关键改动 | mon→sw 矩阵 | 备注 |
|------|----------|-------------|------|
| #1003 | force=70 / warm hold / skip=drop=2 | GREEN sw#3–4 | 复现基线 |
| #1005 | `warm_left>0` → mi_drop early WORK | **14/36** 头 0–4 绿 | warm 后仍绿 |
| CamOS warmup 6→8→**12** | runtime_settings + src clamp 16 | 单独不够 | residual_discard 常 forced clear |
| #1006 | `pub_gate=10` 仅 `frame_early` | r0/r2 ok，**r1 全绿 0–4** | r1 仅 `mi_pub IRQ early=0`，gate 未吃 IRQ |
| #1007 | `gate_drop` 不限 early | **15/48** 多轮头 0–4 绿 | gate 与 warm **并行递减**，warm 尽时 gate=0 |
| #1008 | **gate 仅在 `!drop && !warm_drop` 时递减**；`PUB_GATE=6` | **0/48** mon→sw×4 head12 | 见 §10.9；n1trace 确认 gate 在 warm 后 6→0 再 mi_pub |

### 10.3 n1trace 关键证据

**#1005（warm_left only）**  
进 FT 后 `mi_drop WORK warm=8→1`，随后 `mi_pub` 且用户 sw0–4 绿 → quarantine 长度不够。

**#1006 r1（early-done 未跑起来）**

```
isp_ft_arm ... pub_gate=10
mi_drop IRQ ×2 (drop_left)
随后大量 mi_pub IRQ early=0   ← gate 因要求 frame_early 未扣
residual_discard=11 forced clear
用户 r1_0..4 GREEN
```

**#1007（gate 与 warm 抢计数）**

```
mi_drop ... warm=8 gate=9
...
mi_drop ... warm=0 gate=0 cnt=10   ← 第 10 次 drop 后 gate 已空
mi_pub pub=1..  → 用户头帧仍绿
```

### 10.4 #1008 设计

| 层 | 配置 |
|----|------|
| 内核 `ISP39_IMX296_PUB_GATE_FRAMES` | **6**（覆盖 #1005 头 0–4 +1） |
| `capture_v39` | `gate_drop = ft_diag && gate && !drop && !warm_drop`；IRQ/WORK 皆可 |
| `warm_drop` | 仍仅 `frame_early && warm` |
| CamOS | `SMARTCAM_TRIGGER_WARMUP_FRAMES=12`，interval 150ms；src clamp max 16 |
| 期望 dmesg | `pub_gate 6`；`mi_drop ... gate=6→1` **出现在 warm/drop 之后**；再 `mi_pub` |

### 10.5 CamOS 已知缺口（未改二进制逻辑）

- `_warmup_sensor` 脉冲后若 sample 未减 `warmup_discard_cnt`，结束时 **forced clear residual** → 用户闸门可早于内核 quarantine。
- journal 常见 `triggered 12/12 residual_discard=9..11 forced clear`。
- 源码树：`/root/smartcam_bs/.../gst_v4l2_grabber.py`；运行时为 **nuitka** `camos` 二进制，靠 `runtime_settings_dlcvcam.json` `startup_env` 注入。
- 彻底修应用侧：warmup 必须等到 discard 真扣完或与内核 pub 对齐，禁止 forced clear 开真触发（**待做，非本轮内核**）。

### 10.6 板端备忘

- 备份：`/root/boot-backups/boot-before-202608100{5,6,7}.img`
- 交付镜像：`boot-rk3576-6.1.99-2026081008-01726d6f-dirty.img`
- n1trace 默认关；抓完 `echo 0 > /sys/module/video_rkisp/parameters/n1trace`
- 产物目录：`/tmp/green_verify_1005` `1006` `1007`（及后续 1008）

### 10.7 成功标准（复述）

- mon→software ≥3 轮 head12：`TOTAL_GREEN=0`，首枪有图  
- dmesg 仍 ceiling-force（N-1 不回退）  
- ROI set/reset + line0 另验  

### 10.8 下一步

1. 刷 `#1008`，CamOS warmup=12，重跑 mon→sw 4 轮矩阵  
2. 若仍绿：对照 n1trace 看 `gate` 是否在 pub 前耗尽；考虑 gate≥8 或修 CamOS forced clear  
3. 绿清零后再 ROI / line0  

---

*文档随刷机验证持续追加；勿只改常量不写 n1trace。*

### 10.9 #1008 验证结果（2026-08-10 18:01–18:03）

- 内核：`#2026081008` dmesg `pub_gate 6`
- CamOS：`warmup triggered 12/12`（仍常见 `residual_discard=8..12 forced clear`）
- 矩阵：mon→software **4 轮 × head12**，`TOTAL_GREEN **0**`（`/tmp/green_verify_1008`）
- n1trace 形态（正常 early-done 轮）：
  - `mi_drop` drop×2 + warm 递减时 **gate 保持 6**
  - warm=0 后 `gate=5→0` 共 6 次再 `mi_pub`
- 首用户帧偶发 `dt≈2–3s / pulses=3–4`：gate 尚未耗尽时 tag 慢进，属 quarantine 代价，不是绿线
- **未验**：ROI set/reset、line0 外部真触发、产品存图路径长时间压测

### 10.10 仍建议的应用侧收尾

1. 禁止 warmup 结束 `forced clear` 开真触发；或 warmup 脉冲等到 discard 真扣完  
2. 将 `SMARTCAM_TRIGGER_WARMUP_FRAMES` 与内核 skip+drop+warm+gate 文档化对齐（当前 12 够用）  
3. ROI 边界强制同一套 quarantine（与 mon→FT 同 arm）

### 10.11 4h 稳定性复测（2026-08-10 20:57 起）

- 脚本：板端 `/tmp/imx296_4h_endurance.py --hours 4` → `/tmp/imx296_4h_endurance_1008`
- 报告：`IMX296_4h稳定性复测_20260810.md`
- 覆盖：mon/trig×software/line0、ROI set/reset、底绿、N-1 LO/HI
- 内核保持 `#2026081008`，n1trace 关

### 10.12 4h 复测结论（有效窗口 ~2.21h）

- **green=0 / n1=0**，frames=3570，cycles=64；唯一 bug 为小 ROI `352x328` phase=MIX（非绿、非 hard N-1）
- 未跑满 4h：23:11 CamOS signal 退出，`/tmp` 脚本丢失
- 详见 `IMX296_4h稳定性复测_20260810.md`


---

## 11. 2026-08-12 复盘：line0 底绿复发 + 文档/实现偏差（CamOS 侧已回退 post=6）

> 状态：**文档 §10.9「warm 期间 gate 保持 6」在当前 line0/IRQ 主导路径上不成立**  
> 板端内核仍为 `#2026081008`（`skip=2 drop=2 warmup=8 pub_gate=6 force_post=70000`）  
> CamOS 分支：`fix/roi-trigger-n1-master-rebase`（warmup 已改为脉冲预算 + quarantine；**已回退** post=6/min_appsink=5）  
> 产物：板端 `/userdata/imx296_warmup_ab/n1trace_gate_20260812_145230/`、`line0_sw_green_*`、`line0_post6_*`、`line0_post3_revert_*`  
> **本轮未改内核代码**，只修订记录与方案。

### 11.1 本轮用户侧现象（相对 §10.9 mon→sw green=0）

| 项 | 观察 |
|----|------|
| mon→line0 头枪 | 早期曾 leading miss 2–4 枪；CamOS 修 PLAYING/prime/post=3 后可稳定 **first_hit=1** |
| apply 墙钟 | 目标 ≤5s；post=3 路径 avg≈4–5s，prime 最坏 ~16 枪时偶发 >5.5s |
| 底绿 / 底绿线 | **line0 多轮抽检仍绿**：`line0_sw_green_20260812_133403` line0_green=22；`135804` 仍 13（且 first_hit 全 1） |
| CamOS 加 post | post=6 + min_appsink=5：green 仍 12，**first_hit 退化** `[4,1,1,5,1,1,3,7]` → **已回退** post=3 / min_appsink=1 |

结论：在 first_hit 与 ≤5s 约束下，**CamOS 再堆 residual post 消不掉底绿，且伤头枪**。绿帧主因回到内核发布相位。

### 11.2 内核契约（源码，#1008 树，与板一致）

`rkisp.c` 进 FT arm（常数未改）：

| 符号 | 值 | 含义 |
|------|----|------|
| `SKIP_EARLY_FRAMES` | 2 | 头 SOF 不做 early-done |
| `SKIP_BUF_FRAMES` | 2 | `early_done_drop_left` + `stream->skip_frame` |
| `WARMUP_FRAMES` | 8 | `early_done_warmup_left`；warmup 段 post-MI hold 更长 |
| `PUB_GATE_FRAMES` | 6 | `early_done_pub_gate_left` |
| `FORCE_POST_MI_US` | 70000 | ceiling-force 后 hold |
| `WARMUP_POST_MI_US` | 85000 | warm 段 hold |

`capture_v39.c` `mi_frame_end` 丢帧条件（摘要）：

```text
warm_drop = ft_diag && frame_early && warm>0
gate_drop = ft_diag && gate>0 && !drop && !warm_drop
if (drop || warm_drop || gate_drop) → mi_drop（requeue，无 vb2_done）
```

**关键点（相对 §10.7–10.9 叙述）**：

1. **`warm_drop` 要求 `frame_early==1`**。  
   当 complete/发布走 **`early=0`（IRQ / MI FE）** 时，即使 `warm_left>0`，**也不会**走 warm_drop。
2. 此时若 `drop_left==0` 且 `gate>0`，**`gate_drop` 为真** → **gate 在 warm 尚未耗尽时被 IRQ 路径打掉**。  
   这与 §10.9 写的「drop×2 + warm 递减时 **gate 保持 6**；warm=0 后 gate 6→0 再 mi_pub」**只覆盖 early-done WORK 主导轮**，**不覆盖当前 line0 实测的 IRQ 主导轮**。
3. `#1006` 曾要求「IRQ 也要吃 gate」（防 r1 全绿），`#1007` 要求「gate 不与 warm 并行扣」。  
   现行布尔式把两者捏在一起：**IRQ + warm>0 ⇒ 仍扣 gate**，形成 **#1007 在 IRQ 路径上的回退洞**。
4. `warm_left` 主要在 `capture.c` early **complete** 路径递减；`mi_drop` 日志里的 `warm=` 是读快照，**IRQ mi_drop 不替代 warm 语义丢帧**。
5. `mi_pub` 日志**不打印** `warm_left`/`gate`；对齐时需夹在邻近 `early complete` / 上一条 `mi_drop` 之间读。

源码注释仍写：`CamOS SMARTCAM_TRIGGER_WARMUP_FRAMES must cover skip_sof+drop+warm+pub_gate`。  
CamOS 已不再以 appsink discard==0 为成功条件，但 **gate 若在 warm 期被 IRQ 耗尽，用户态脉冲再多也盖不住「gate=0 后的 early mi_pub」画质**。

### 11.3 板端 n1trace 证据（2026-08-12 14:52，`n1trace_gate_20260812_145230`）

同一次 `isp_ft_arm ... skip=2 drop=2 warmup=8 pub_gate=6` 后（节选逻辑序）：

```text
mi_drop IRQ left=1→0  warm=8 gate=6          ← drop 耗尽，gate 仍 6
mi_drop IRQ left=0    warm=8 gate=5          ← drop 已 0，warm 仍 8，gate 开始减（early=0）
mi_drop IRQ           warm=7 gate=4
...
mi_drop IRQ           warm=4 gate=0 cnt=8    ← gate 在 warm 仍 4 时归零
mi_skip_frame skip=2 → skip=1
early complete        warm_left=3
mi_pub  IRQ pub=1 early=0 drop_left=0        ← 首次发布；gate 早已 0，warm 轨迹仍在中后段
mi_pub  pub=2..12 ...
```

对照：

| §10.9 宣称（early 主导） | 本迹（line0/IRQ 主导） |
|--------------------------|------------------------|
| warm 期 gate **保持 6** | warm 期 gate **6→0**（IRQ mi_drop） |
| warm=0 后 gate 再 6→0 | gate 先于 warm 耗尽 |
| 其后 mi_pub 为「gate 后」 | **首 mi_pub 不在 pub_gate 计数内**（gate 已 0），且 complete 侧 warm_left 仍可 >0 |

与用户 green tag 对齐的含义：

- 绿帧 **不是**「仍卡在 pub_gate 计数器里没放行」；
- 而是 **gate 被 IRQ 提前烧光后，仍处 FT 前段（warm/ceiling-force 未稳定）的 mi_pub** 进入 appsink/预览；
- 故 CamOS 在「首 appsink 后再 +3/+6 枪」只能偶尔错位相消，**统计上消不干净**，加长 post 还拉高 first_hit/apply。

### 11.4 CamOS 侧已做 / 已否（避免文档继续写「加 warmup 帧必绿清」）

| 动作 | 结果 |
|------|------|
| 去掉 discard==0 / forced-clear 开闸 | 正确方向；与 §10.10.1 一致（已落地思路） |
| 脉冲预算 + PLAYING/appsink 见帧 + quarantine double-flush | first_hit 可稳 1，apply ~4–5s |
| post residual 3→6 + min_appsink 5 | **绿仍在 + 头枪回退** → **回退** |
| 默认（回退后） | `POST_PLAYING=3`，`MIN_APPSINK_SEEN=1`，`TOTAL_PULSE_MAX=22`，interval 100ms |

**不要**再把「CamOS 再加 post/min_appsink」写成消绿主路径；主路径应回到内核 gate 与 warm/IRQ 的布尔关系。

### 11.5 故障点清单（按优先级）

1. **P0 内核逻辑洞（本迹已坐实）**  
   `gate_drop` 在 `early=0 && warm>0` 时仍为真 → **pub_gate 与 warm 在 IRQ 路径并行消耗**，违背 #1007 设计意图。  
   **建议修复（尚未改代码，需单独评审/刷机）**：
   - 方案 C1（推荐）：`gate_drop` 增加 **`warm==0`（或 `!warm`）** 条件，使 gate **仅在 drop 耗尽且 warm 耗尽之后** 才递减；IRQ 与 early 共用。  
     伪代码：`gate_drop = ft_diag && gate && !drop && !warm && /* 可选：允许 IRQ */ true`  
     同时保留「IRQ 也必须过 gate」：即 warm 完后的 IRQ pub 仍走 gate_drop，而不是 #1006 那样在 warm 期误 pub。
   - 方案 C2：`warm_drop` 改为 **`warm>0` 不依赖 `frame_early`**（warm 期所有 complete 都 mi_drop）。更猛，需防和 skip_frame/drop 双重计数、以及 hold 路径死等。
   - 方案 C3：`mi_pub` 时若 `warm_left>0` **强制 mi_drop**（发布门槛与 complete 计数解耦）。与 C1 可组合。
   - **验证必须**：n1trace 看 `gate` 在 `warm` 归零前是否恒定；首 `mi_pub` 时 `warm_left==0` 且刚经历 gate 6→0；mon→line0 / mon→sw 绿矩阵 + first_hit + apply。

2. **P1 观测缺口**  
   - `mi_pub` 行无 `warm`/`gate` 字段 → 排障易误判「在 gate 内」。建议 n1trace 增补。  
   - 自动化 green 曾被 stale preview / 曝光 API 假阳性干扰；应对 **命中帧** 做 bot8 判定（CamOS QA 已部分改正）。

3. **P2 文档过时**  
   - §10.9 / §10.12 的 green=0 **不推广到 line0 2026-08-12 产品路径**。  
   - `IMX296_绿线修复_内核修改与验证记录.md` 仍停在 08-06 早期 skip/drop/warmup 数字，**未反映 #1008 pub_gate**；以本文件 §10–§11 与源码为准。  
   - CamOS 仓库 plan（`docs/plans/active/2026-08-11_IMX296_warmup...`）写「画质主防护在内核 pub_gate」——**方向对，但须注明 IRQ 洞未修前 line0 仍可能绿**。

4. **P3 非根因（已排除作主手段）**  
   - 单纯再加 `force_post` / CamOS post residual（§3、§11.1 已否定为主因）。  
   - 用户态 appsink discard 收齐（多数 pulse 本就无 sample）。

### 11.6 与 CamOS 的协作契约（修订）

| 层 | 应保证 | 不应指望 |
|----|--------|----------|
| 内核 | 进 FT 后 **首帧 mi_pub 时 warm_left==0 且 pub_gate 已按设计耗尽**；IRQ/early 一致 | 用户态多脉冲「盖住」gate 提前烧光后的脏 pub |
| CamOS | mon→FT / ROI 后 XTRIG 预算 + quarantine；**success 后才** line0-stable gate；first_hit=1 与 apply≤5s | 用 post≥6/min_appsink≥5 消绿 |
| 联调 | 短 n1trace：arm → drop → warm → gate → 首 mi_pub 单调 | 只看 CamOS success=1 或只看 endurance green 汇总 |

### 11.7 建议下一动作（内核树，未执行）

1. 按 **C1** 改 `capture_v39.c` 布尔条件 + `mi_pub` 日志带上 `warm/gate`。  
2. 本地编译刷 `#20260812xx`，重跑：  
   - mon→line0 ×8 + mon→sw ×4 绿矩阵  
   - 短 n1trace 确认首 mi_pub 相位  
   - first_hit / apply 回归（CamOS 保持 post=3）  
3. 通过后回写本节为「已修」，并摘一句到 `IMX296_绿线修复_内核修改与验证记录.md` 文首「最后更新」。

### 11.8 文档正确性结论（本次 SSH 核对）

| 文档 | 结论 |
|------|------|
| 本文件 §1–§3 ceiling-force / hold 满仍可绿 | **仍正确** |
| §10.5–10.8 #1005–#1008 迭代故事 | **历史正确** |
| §10.9「warm 期 gate 保持 6」 | **仅 early 主导轮成立；line0/IRQ 主导轮错误 → 以 §11 为准** |
| §10.10 CamOS forced clear | **方向正确**；CamOS 已转向脉冲预算（discard 门槛已弃） |
| §10.12 4h green=0 | **有效窗口结论保留**；**不覆盖 08-12 line0 复发** |
| `IMX296_绿线修复_*.md`（08-06） | **过旧常数**；勿当 #1008 真源 |
| 源码 `rkisp.c` / `capture_v39.c` 注释 | 与 #1008 意图一致，但 **gate_drop 实现与 #1007 文字意图在 IRQ 路径不一致**（§11.2） |



---

## 12. 2026-08-12 #1009 内核修复（gate/IRQ warm 洞）

> 状态：**源码已合入工作区，构建号 `2026081201`，尚未编译刷板验证**  
> 依据：§11.3 板端 `n1trace_gate_20260812_145230` + 源码布尔式复核；line0 矩阵 `line0_sw_green_*` / `line0_post6_*` green 仍在且加 CamOS post 无效。

### 12.1 根因确认（板端坐实，非猜测）

板 `root@192.168.1.180` 仍跑 `#2026081008`。`n1trace_gate_20260812_145230/dmesg_n1trace.txt` 进 FT 后：

```text
isp_ft_arm ... skip=2 drop=2 warmup=8 pub_gate=6
mi_drop IRQ left=1→0 warm=8 gate=6     ← drop 耗尽
mi_drop IRQ left=0   warm=8 gate=5     ← warm 仍 8，gate 开始减（early=0）
...
mi_drop IRQ          warm=4 gate=0     ← gate 在 warm 中段烧光
early complete       warm_left=3
mi_pub  IRQ pub=1 early=0              ← 首发布：gate 已 0，warm 轨迹仍 >0
```

对照 `#1008` 代码：

- `warm_drop = ft_diag && frame_early && warm` → **IRQ early=0 永不 warm_drop**
- `gate_drop = ft_diag && gate && !drop && !warm_drop` → **IRQ+warm>0 仍 gate_drop**
- ⇒ 与 §10.9「warm 期 gate 保持 6」只在 early-WORK 主导轮成立；**line0/IRQ 主导轮不成立**

产品侧：`line0_sw_green_20260812_135804` totals.line0_green=13、first_hit 全 1、apply≈3.5–5s；`line0_post6` green=12 且 first_hit 退化。**再加 CamOS post 不是正解。**

### 12.2 代码改动（#1009）

| 文件 | 改动 |
|------|------|
| `capture_v39.c` | `warm_drop = ft_diag && warm`（IRQ+WORK）；`gate_drop = ... && !drop && !warm`；IRQ warm 路径 `warm_left--`（early complete 已减则不双扣）；`mi_pub` n1trace 增 `warm/gate` |
| `capture.h` / `rkisp.c` | 注释与 #1009 契约对齐；常数不变 skip/drop/warm/gate=2/2/8/6 |
| `DLCVCAM_BUILD_VERSION` | `2026081201` |

期望 n1trace（刷板后）：

```text
mi_drop ... warm 递减时 gate 恒为 6
warm→0 后 gate 6→0
首 mi_pub 时 warm=0 且刚经历 gate 耗尽（或 gate 刚到 0 的下一帧）
```

### 12.3 成功标准（刷板后）

- mon→line0 ×8 + mon→sw ×4：`TOTAL_GREEN=0`，无底串帧
- first_hit=1（用户触发不被吞）；apply 墙钟 **≈5s 或更短**（CamOS 保持 post=3，不靠加 post）
- ROI set/reset 后同样 green=0 + 有图
- dmesg 仍 ceiling-force（N-1 不回退）；短开 n1trace 核对相位后必须 `echo 0`

### 12.4 未做

- 未编译 / 未刷 `#2026081201`
- 未改 CamOS；禁止再把加 post 当消绿主路径
- 未动 force_post / SKIP 常数（隔离变量）


*记录人：CamOS 联调 2026-08-12；构建机 `root@192.168.1.59` 工作区 `fix_imx296_n1_pwm`；板 `180` `#2026081008`。*
