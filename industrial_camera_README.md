# IMX415 工业相机外部触发采集方案

## 概述

本方案针对**工业流水线产品检测**场景，实现：
- 外部触发信号到达时拍照（GPIO/定时器/网络指令等）
- 自动处理长间隔触发导致的白帧/过曝问题
- OpenCV 实时预览 + 产品检测

## 核心机制：双脉冲 Dummy Frame 方案

### 原理

IMX415 在 slave 外部同步模式下，像素会在两个 XVS 边界之间持续积分：

```
触发 1            触发 2（间隔很久）
  |                    |
  XVS ←——————积分——————→ XVS
                    ↑
                这一帧积分时间 = 触发间隔
                间隔越久越饱和 → 白帧/过曝
```

**解决方法**：每次触发输出 **2 个 XVS 脉冲**（间隔 1 帧时间）

```
触发指令
  |
  XVS1 ←——33ms——→ XVS2
  |               |
Dummy Frame    Real Frame
(长积分/白)    (正常曝光)
  ↓               ↓
用户态丢弃      实际使用
```

- **第 1 帧（Dummy）**：清空长间隔积累的饱和电荷，用户态自动丢弃
- **第 2 帧（Real）**：积分时间 = 33ms（1 帧），正常曝光

### 为什么用户态丢帧而不是驱动侧自动丢？

1. **架构原因**：
   - 传感器驱动（`imx415.c`）是 `v4l2_subdev`，不管理 buffer
   - Buffer 管理在 ISP 驱动层（`rkisp`），跨驱动通信复杂

2. **工业相机惯例**：
   - Basler/FLIR/海康等主流工业相机 SDK 都在**用户态做跳帧**
   - 更灵活：可以保存 Dummy 做诊断，或动态调整策略

3. **性能开销极小**：
   - 多读一帧开销 < 1ms，对流水线节拍（通常 > 100ms）影响可忽略

---

## 快速开始

### 1. 安装依赖

```bash
pip3 install opencv-python numpy
# GPIO 模式还需要（二选一）
pip3 install python3-libgpiod  # 推荐
# 或
pip3 install RPi.GPIO
```

### 2. 确保内核/驱动已更新

编译并部署最新的 `drivers/media/i2c/imx415.c`（包含双脉冲支持）。

### 3. 运行示例

**定时触发模式**（测试用）：

```bash
# 每 2 秒触发一次，实时预览
python3 industrial_camera_capture.py --mode timer --interval 2.0

# 保存每一帧到指定目录
python3 industrial_camera_capture.py --mode timer --interval 1.0 --save-dir ./captures
```

**GPIO 触发模式**（生产环境）：

```bash
# GPIO17 上升沿触发（需要 root）
sudo python3 industrial_camera_capture.py --mode gpio --gpio-pin 17

# 无头模式（无 GUI 预览，适合嵌入式）
sudo python3 industrial_camera_capture.py --mode gpio --gpio-pin 17 --no-preview
```

---

## 参数说明

### 触发脉冲数

驱动支持写入不同的脉冲数：

| 写入值 | XVS 脉冲数 | 适用场景 |
|--------|-----------|---------|
| `echo 1 > trigger` | 1 个 | **连续采集**（30Hz loop），积分时间稳定 |
| `echo 2 > trigger` | 2 个（间隔 33ms） | **稀疏触发**（间隔 >100ms），自动清 Dummy |

### 用户态采集策略

```python
# 连续 30Hz 采集（预览/录像）
while True:
    camera.trigger_and_capture(num_pulses=1)  # 单脉冲
    time.sleep(0.033)

# 稀疏触发（产品检测）
def on_product_detected():
    real, _ = camera.trigger_and_capture(num_pulses=2)  # 双脉冲，自动丢 Dummy
    if real is not None:
        detect_defects(real)
```

---

## 集成到你的工业控制系统

### 示例 1：PLC 触发 + TCP 通信

```python
import socket
from industrial_camera_capture import IMX415IndustrialCamera

camera = IMX415IndustrialCamera()
camera.open()

# 监听 PLC 发来的触发指令
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind(('0.0.0.0', 5000))
server.listen(1)

while True:
    conn, addr = server.accept()
    data = conn.recv(1024)
    
    if data == b'TRIGGER\n':
        real_frame, _ = camera.trigger_and_capture(num_pulses=2)
        
        if real_frame is not None:
            # 检测缺陷
            result = detect_defects(real_frame)
            conn.sendall(f"OK:{result}\n".encode())
        else:
            conn.sendall(b"ERROR\n")
    
    conn.close()
```

### 示例 2：ROS2 工业机器人集成

```python
import rclpy
from rclpy.node import Node
from sensor_msgs.msg import Image
from cv_bridge import CvBridge

class IMX415TriggerNode(Node):
    def __init__(self):
        super().__init__('imx415_trigger_node')
        self.camera = IMX415IndustrialCamera()
        self.camera.open()
        self.bridge = CvBridge()
        
        # 订阅触发话题
        self.sub = self.create_subscription(
            EmptyMsg, '/trigger_camera', self.on_trigger, 10)
        
        # 发布图像
        self.pub = self.create_publisher(Image, '/camera/image_raw', 10)
    
    def on_trigger(self, msg):
        real, _ = self.camera.trigger_and_capture(num_pulses=2)
        if real is not None:
            ros_img = self.bridge.cv2_to_imgmsg(real, encoding='bgr8')
            self.pub.publish(ros_img)
```

---

## 调试与优化

### 查看驱动日志

```bash
dmesg -w | grep imx415
```

每次触发会打印一行：

```
imx415 5-001a: trigger pulses=2 frame_ns=33333750 xhs=keep xhs_p=14815ns xvs_p=33333750ns reset=0 ret=0
```

字段含义：
- `pulses=2`：本次打了 2 个 XVS
- `frame_ns=33333750`：1 帧时长（ns）
- `xhs=keep`：XHS 保持常开
- `reset=1/0`：是否做了 standby→streaming 复位（长间隔时为 1）
- `ret=0`：成功

### 常见问题

**Q: 连续触发仍有紫帧/色偏交替？**

A: 确认 loop 使用的是 `echo 1`（单脉冲）而不是 `echo 2`：

```bash
# 错误：echo 2 @ 30Hz loop → 等效 60Hz 双边界 → 帧序混乱
while true; do echo 2 > .../trigger; sleep 0.033; done

# 正确：echo 1 @ 30Hz loop → 稳定 30Hz
while true; do echo 1 > .../trigger; sleep 0.033; done
```

**Q: 长间隔触发仍白帧？**

A: 检查 `dmesg` 里 `reset=` 是否为 1。如果为 0 说明间隔没超过阈值（2 帧）。
可以在驱动里把 `gap_ns > frame * 2` 改成 `gap_ns > frame * 1`（更激进）。

**Q: 如何验证 Dummy Frame 确实被丢弃？**

```python
# 保存 Dummy 做对比
real, dummy = camera.trigger_and_capture(num_pulses=2, save_dummy=True)
cv2.imwrite('dummy.png', dummy)  # 通常会白/过曝
cv2.imwrite('real.png', real)    # 正常曝光
```

---

## 性能指标（参考）

| 场景 | 触发方式 | 有效帧率 | 延迟 |
|------|---------|---------|------|
| 连续预览 | `echo 1` @ 30Hz | 30 fps | ~33 ms/帧 |
| 稀疏抓拍 | `echo 2`（间隔 2s） | N/A | ~70 ms（2 帧读出时间） |

---

## 进阶：集成机器视觉算法

```python
import cv2
from industrial_camera_capture import IMX415IndustrialCamera

camera = IMX415IndustrialCamera()
camera.open()

def detect_product_defects(frame):
    """产品缺陷检测（示例）"""
    # 1. 转灰度
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    
    # 2. 边缘检测
    edges = cv2.Canny(gray, 50, 150)
    
    # 3. 轮廓检测
    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, 
                                   cv2.CHAIN_APPROX_SIMPLE)
    
    # 4. 缺陷判定
    defect_count = len([c for c in contours if cv2.contourArea(c) > 100])
    
    return {'defects': defect_count, 'ok': defect_count == 0}

# GPIO 触发回调中集成检测
def on_product_arrive():
    real, _ = camera.trigger_and_capture(num_pulses=2)
    
    if real is not None:
        result = detect_product_defects(real)
        
        if result['ok']:
            signal_production_line('PASS')
        else:
            signal_production_line('REJECT')
            save_defect_image(real, result)
```

---

## 许可

本示例代码可自由用于工业/商业用途。
