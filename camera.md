# 海康相机 + 相机守护（CameraGuard）说明

## 一、硬件与网络

```
相机 (MV-CS060-10GC) ──网线──→ Nano (enP8p1s0)
   192.168.2.10               192.168.2.100
```

| 项 | 值 |
|------|-----|
| 相机型号 | 海康 MV-CS060-10GC（GigE，6MP，IMX178） |
| 相机 IP | `192.168.2.10` |
| Nano 网卡 | `enP8p1s0`，IP `192.168.2.100` |
| MTU | `9000`（Jumbo Frame，相机大帧必备，`nmcli` 永久生效） |

> 相机 IP 用海康 SADP 工具改；无线网卡和有线网卡不能在同一个网段，否则 SSH 会连不上 Nano。
> 详见 `hikUse.md`。

---

## 二、代码模块

| 文件 | 职责 |
|------|------|
| `include/camera.h` / `src/camera.cpp` | `HikvisionCamera`：MVS SDK 封装（连接 / 取流 / 软硬触发 / 像素转换） |
| `include/cameraguard.h` | `CameraGuard`：相机掉线自动重连 + 连续空帧故障判定（**解耦设计，见下**） |
| `src/main.cpp` | 接线：`connectCamera` 回调 + `CameraGuard` 实例 + PLC HR2 状态联动 |

### 采集模式（`Config::CAMERA_TRIGGER`）

| 值 | 模式 | 说明 |
|:---:|---|------|
| 0 | 连续采集 | 调试用，不停取流 |
| 1 | 软触发 | 程序发 `softwareTrigger()` 指令拍照（当前主线用） |
| 2 | 硬触发 | Line0 接收流水线传感器信号 |

启动参数：`CAMERA_WIDTH / CAMERA_HEIGHT / CAMERA_EXPOSURE(7000us) / CAMERA_GAIN(0dB)`。

### 相机温度

- MV-CS060 系列**一般支持温度**，但节点名因型号/固件而异，且常被标成 `visible=false`——MVS 客户端的**普通参数页看不到**，只有**完整节点树**模式（右侧面板顶部"树/列表"图标）才有，所以"MVS 里搜不到"不代表相机没有。
- `HikvisionCamera::getTemperature()` **自动探测**常见节点名，命中即锁存，之后每次只读那一个：
  `DeviceTemperature` → `SensorTemperature` → `CameraTemperature` → `DeviceSensorTemperature`。
  节点是 Float（°C）或 Int（值=0.1°C），均兼容。全失败返回 -1。
- 读不到温度时界面**只显示 GPU 温度**（不显示"相机 --"）；连续 3 次读失败后放弃重试，不再白读 GigE 寄存器。
- **性能**：温度是低优先级状态显示，只在主循环【空闲】分支刷新（检测路径零温度 I/O）；读一次缓存 60s。
- 换不同型号相机：节点名在这四个常见名内即可自动显示，无需改代码；不在的话在 MVS 完整节点树里找真实节点名加进数组即可。

---

## 三、CameraGuard 解耦设计（核心）

**设计目标**：main() 只负责"接线"，不写重连/故障判断逻辑；CameraGuard 自身**不依赖相机 SDK / PLC / Qt**，通过两个 `std::function` 回调对接外部。

### 回调接口

| 回调 | 签名 | 作用 |
|------|------|------|
| `connect` | `std::function<bool()>` | 执行一次完整连接（stop → 枚举打开 → start），返回成功与否 |
| `on_state` | `std::function<void(bool ready)>` | 状态变化通知：`true`=已就绪，`false`=判定故障 |

### 对外方法（主循环单线程驱动）

| 方法 | 调用时机 | 行为 |
|------|---------|------|
| `poll()` | 主循环每轮 | 未连接 → 限频 **1s** 后台重连；重连成功回调 `on_state(true)` |
| `onFrame()` | 每次正常拿到帧 | 清除连续空帧计数 |
| `onMiss()` | 每次空帧 | 累计空帧；**连续 3 次返回 `true` 判定故障**，回调 `on_state(false)` |
| `running()` | 触发前判断 | 相机是否可用（连接中且未判定故障） |

内部参数（`cameraguard.h` 静态常量）：`RECONNECT_INTERVAL_S = 1.0`（重连限频），`MAX_MISSES = 3`（故障阈值）。

### main.cpp 接线

```cpp
// 启动 + 掉线重连共用的连接函数
auto connectCamera = [&]() -> bool {
    cam.stop();                                   // 清残留状态，首次调用无操作
    if (!cam.connectByIP(Config::CAMERA_IP)) return false;
    if (!cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                   (float)win.exposureUs(), (float)win.gainDb(),   // 用界面当前曝光/增益
                   Config::CAMERA_TRIGGER)) {
        cam.stop();
        return false;
    }
    return true;
};

// 守护实例：状态变化 → 写 PLC HR2 + 界面相机灯
CameraGuard camGuard(connectCamera, [&](bool ready) {
    plc.sendReady(ready);        // 就绪 → HR2=1；故障 → HR2=0
    win.setCamFault(!ready);     // 故障 → 红灯；恢复 → 清红灯
    win.setCamRunning(ready);    // 就绪 → 绿
}, cam.isRunning());

// 主循环里三处调用：
camGuard.poll();                    // ① 每轮：未连接则限频重连
if (!camGuard.running()) continue;  // ② 触发后：相机未就绪，跳过本次拍照
if (camGuard.onMiss()) cam.stop();  // ③ 空帧：第 3 次判故障，停相机等下轮 poll 重连
camGuard.onFrame();                 // ④ 正常帧：记健康
```

---

## 四、掉线 / 故障处理流程

### 相机故障（掉线 / 连续空帧）

```
触发拍照 → readNewest 空帧
   ├─ 第 1、2 次 → 不计故障（瞬时抖动不误报），下次触发再试
   └─ 第 3 次    → onMiss() 返回 true
                    ├─ on_state(false) → plc.sendReady(false) → PLC HR2=0（报警停机）
                    │                              界面相机灯变红（故障）
                    ├─ cam.stop()      → 相机标记未运行
                    └─ 下轮 poll()      → 1s 后自动重连
                         ├─ 失败 → 继续每 1s 重试（HR2 保持 0，灯保持红）
                         └─ 成功 → on_state(true) → HR2=1，清红灯变绿
```

### 启动时相机没连上

- **不退出程序**：`connectCamera()` 失败只打日志，进后台重连模式。
- 这么设计是为了 systemd 自动重启：启动即 `return` 会让 systemd 无限循环重启。
- 期间 PLC 状态寄存器 `HR2=0`，PLC 侧知道机器未就绪；界面相机灯灰（未连接，故障才是红）。
- 相机一旦上线，`poll()` 1s 内自动连上。

> 引擎（`infer.load`）和 PLC server（`plc.start`）失败仍是 `return` 退出——这是配置/部署错误，
> 不是瞬时故障，保留退出避免无限重试掩盖问题。systemd 侧建议配 `StartLimitIntervalSec` 兜底。

---

## 五、HR2 状态联动（PLC 侧）

寄存器映射（详见 `plc.md`）：

| Holding Register | 方向 | 含义 |
|:---:|:---:|---|
| HR2 | Nano → PLC | 0=未就绪/故障，1=就绪 |

PLC 程序里应先读 HR2 再决定是否采信 HR1 结果：

```
IF D2 = 1 THEN        // Nano 就绪
    处理 D1 检测结果
ELSE                  // 故障/未开机
    报警停机，不处理结果
END_IF;
```

---

## 六、调试命令速查

```bash
# 网络连通性
ping 192.168.2.10

# 确认相机枚举
sudo nmap -sn 192.168.2.0/24

# 相机进程日志（重连/故障/就绪都有 [Camera] 前缀输出）
journalctl -u wood-defect -f
```

正常/故障输出示例：

```
[Camera] 取流启动失败           ← 启动失败进重连模式（不退出）
[Camera] 触发后未获取到图像      ← 空帧（第 1/2 次不计故障）
[Camera] 连续空帧判定故障 → 通知 PLC（HR2=0）
[Camera] 重连成功               ← 恢复，HR2=1
```

---

## 七、改动记录（方案）

### 目标

1. 相机掉线后**自动重连**，不用人去重启程序。
2. 相机故障时**通知 PLC**（HR2=0），PLC 能报警停机，不会白白等检测结果。
3. 启动时相机没连上**不退出**——配合 systemd 自动重启，避免无限重启循环。

### 方案要点（已定的设计决策）

| 决策 | 取值 | 理由 |
|------|------|------|
| 故障判定阈值 | 连续 **3 次**空帧 | 1~2 次是瞬时抖动，不误报 |
| 重连限频 | **1 秒**一次 | 不狂刷 SDK 枚举 |
| 启动失败处理 | **不退出**，进后台重连 | systemd 场景下 `return` 会无限重启循环 |
| 重连后参数 | 用**界面当前曝光/增益** | 工人调过的参数不能丢 |
| 重连时机 | 主循环 `poll()` 驱动 | 单线程架构，不引入新线程 |
| PLC 联动 | `sendReady(bool)` 写 HR2 | 复用现有状态寄存器，无需改 PLC 配置 |
| 解耦 | `CameraGuard` 用两个 `std::function` 回调 | 不依赖 SDK/PLC/Qt，方便测试和复用 |

### 涉及文件

| 文件 | 改动 |
|------|------|
| `include/cameraguard.h` | **新增**：`CameraGuard` 类（重连 + 故障判定，头文件内实现） |
| `src/main.cpp` | 启动相机失败不退出；`connectCamera` 回调 + `camGuard` 接线；空帧/触发前调用 |
| `include/plc_link.h` / `src/plc_link.cpp` | 新增 `sendReady(bool)` 写 HR2（1=就绪, 0=故障） |
| `CMakeLists.txt` | `HEADERS` 补 `cameraguard.h`、`saveworker.h` |
| `camera.md` | 本文档 |

### 行为变化

- **之前**：启动相机连不上直接 `return -1`；掉线后静默停板，PLC 干等。
- **之后**：故障 → 界面红灯 + `HR2=0` → 1s 限频自动重连 → 恢复后 `HR2=1` 绿灯；
  启动未连接/重连中 → 界面灰灯。

### 遗留（未做）

- 引擎 `infer.load`、PLC server `plc.start` 失败仍退出（配置错误，保留退出），
  systemd 侧配 `StartLimitIntervalSec` 兜底即可。
- 相机每次触发掉帧的具体次数统计（界面显示）——如有需要再加。
