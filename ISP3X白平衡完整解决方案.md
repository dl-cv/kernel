# ISP3X (ISP30) 白平衡完整解决方案

## 确认信息
- **ISP版本**: ISP3X (ISP30)
- **不需要AWB1_GAIN**（那是ISP32特有的）
- **需要同时写入gain0、gain1、gain2**

## ISP3X白平衡寄存器

### 绝对地址（用于AIQ模式 mode=0x100）
- `ISP3X_ISP_AWB_GAIN0_G` = `0x538` (0x400 + 0x138)
- `ISP3X_ISP_AWB_GAIN0_RB` = `0x53c` (0x400 + 0x13c)
- `ISP3X_ISP_AWB_GAIN1_G` = `0x540` (0x400 + 0x140)
- `ISP3X_ISP_AWB_GAIN1_RB` = `0x544` (0x400 + 0x144)
- `ISP3X_ISP_AWB_GAIN2_G` = `0x548` (0x400 + 0x148)
- `ISP3X_ISP_AWB_GAIN2_RB` = `0x54c` (0x400 + 0x14c)

## 完整写入命令

### 标准命令（同时写入gain0/1/2）

```bash
# 1. 设置AIQ模式
echo "mode=0x100" > /proc/rkisp1-vir0

# 2. 使能AWB
echo "400=0x6197" > /proc/rkisp1-vir0

# 3. 同时写入gain0、gain1、gain2（R=3.0, G=1.0, B=1.0）
# R=3.0 = 768 = 0x300
# G=1.0 = 256 = 0x100
# B=1.0 = 256 = 0x100
echo "538=0x01000100 53c=0x01000300 540=0x01000100 544=0x01000300 548=0x01000100 54c=0x01000300" > /proc/rkisp1-vir0
```

### 更大的增益值测试（R=5.0）

```bash
# R=5.0 = 1280 = 0x500
echo "mode=0x100" > /proc/rkisp1-vir0
echo "400=0x6197" > /proc/rkisp1-vir0
echo "538=0x01000100 53c=0x01000500 540=0x01000100 544=0x01000500 548=0x01000100 54c=0x01000500" > /proc/rkisp1-vir0
```

## 一键测试脚本

```bash
#!/bin/bash
# ISP3X白平衡测试脚本

PROC_PATH="/proc/rkisp1-vir0"

echo "=== ISP3X白平衡测试 ==="

# 1. 设置AIQ模式
echo "1. 设置AIQ模式..."
echo "mode=0x100" > $PROC_PATH
sleep 0.1

# 2. 使能AWB
echo "2. 使能AWB模块..."
echo "400=0x6197" > $PROC_PATH
sleep 0.1

# 3. 写入所有增益寄存器（R=5.0, G=1.0, B=1.0）
echo "3. 写入gain0/1/2 (R=5.0, G=1.0, B=1.0)..."
echo "538=0x01000100 53c=0x01000500 540=0x01000100 544=0x01000500 548=0x01000100 54c=0x01000500" > $PROC_PATH
sleep 0.2

# 4. 验证
echo "4. 验证结果："
cat $PROC_PATH | grep AWBGAIN

echo ""
echo "提示：如果画面没有变化，请尝试："
echo "  1. 检查cheese使用的video设备"
echo "  2. 重新打开视频流（停止并重启cheese）"
echo "  3. 使用gstreamer测试：gst-launch-1.0 v4l2src device=/dev/video11 ! autovideosink"
```

## 排查步骤

### 1. 检查cheese使用的设备

```bash
# 检查cheese使用的video设备
lsof -p $(pgrep -f cheese) | grep video

# 或者检查所有video设备
ls -l /dev/video* | grep rkisp
v4l2-ctl --list-devices | grep -A 5 rkisp
```

### 2. 重新打开视频流

```bash
# 停止cheese
killall cheese

# 写入白平衡值
echo "mode=0x100" > /proc/rkisp1-vir0
echo "400=0x6197" > /proc/rkisp1-vir0
echo "538=0x01000100 53c=0x01000500 540=0x01000100 544=0x01000500 548=0x01000100 54c=0x01000500" > /proc/rkisp1-vir0

# 等待
sleep 1

# 重新启动cheese
cheese &
```

### 3. 使用gstreamer测试

```bash
# 在一个终端运行
gst-launch-1.0 v4l2src device=/dev/video11 ! autovideosink

# 在另一个终端写入白平衡值
echo "mode=0x100" > /proc/rkisp1-vir0
echo "400=0x6197" > /proc/rkisp1-vir0
echo "538=0x01000100 53c=0x01000500 540=0x01000100 544=0x01000500 548=0x01000100 54c=0x01000500" > /proc/rkisp1-vir0
```

### 4. 检查ISP状态

```bash
# 检查ISP是否在工作
cat /proc/rkisp1-vir0 | head -20

# 检查是否有错误
dmesg | tail -50 | grep -i "awb\|isp\|error"
```

## 增益值计算

### Q8格式说明
- 增益值 = 实际值 × 256
- 例如：R=3.0 → 3.0 × 256 = 768 = 0x300
- 例如：R=5.0 → 5.0 × 256 = 1280 = 0x500

### 常用增益值
- R=1.0 → 0x100
- R=2.0 → 0x200
- R=3.0 → 0x300
- R=5.0 → 0x500
- R=10.0 → 0xA00

### 寄存器值格式
- GAIN_G: `(green_b << 16) | green_r`
- GAIN_RB: `(blue << 16) | red`

例如：R=5.0, G=1.0, B=1.0
- GAIN_G = `(0x100 << 16) | 0x100` = `0x01000100`
- GAIN_RB = `(0x100 << 16) | 0x500` = `0x01000500`

## 注意事项

1. **ISP3X不需要AWB1_GAIN**（那是ISP32特有的）
2. **必须同时写入gain0、gain1、gain2**（即使不使用HDR）
3. **使用绝对地址**（0x538等，不是偏移地址）
4. **使用AIQ模式**（mode=0x100）
5. **必须先使能AWB**（写入0x400=0x6197）

## 如果仍然无效

1. 确认cheese使用的video设备是否正确
2. 尝试重新打开视频流
3. 使用gstreamer测试排除cheese的问题
4. 检查内核日志是否有错误
5. 确认ISP正在工作（不是idle状态）
