# 木板瑕疵检测系统

Jetson Nano 上的流水线实时瑕疵检测：海康工业相机拍照 → TensorRT 推理 → 按规则判 OK/NG →
结果写 PLC 剔除，带 Qt 操作界面。

---

## 目录结构

```
cpp_wood_defect/
├── build.sh                  # 一键编译（Jetson）
├── package.sh                # 打包
├── CMakeLists.txt
├── config.ini                # 运行时配置（界面自动写，第一次运行才生成）
├── 工人操作手册.docx          # 给现场工人看的那一页
├── include/
│   ├── config.h              # 全局配置中心：模型路径、相机、所有判定门槛的出厂值
│   ├── camera.h              # 海康 MVS SDK 采集
│   ├── cameraguard.h         # 相机看门狗（连续空帧判故障）
│   ├── infer.h               # TensorRT 推理
│   ├── measure.h             # 板长/板宽测量
│   ├── postprocessor.h       # NMS + 判定规则 + 画框
│   ├── plc_link.h            # PLC Modbus TCP
│   ├── saveworker.h          # 存图后台线程
│   ├── mainwindow.h          # Qt 界面
│   └── logger.h / perf.h     # 日志 / 计时
├── src/                      # 上面各模块的实现 + main.cpp（主检测循环）
├── models/
│   ├── best.engine           # 当前部署的 TensorRT 引擎
│   └── train*/               # 历次训练留档（results.csv / 混淆矩阵 / args）
├── example/                  # TensorRT-YOLO 自带的例子（example_demo，另有一份旧模型）
└── demo_widthheight.cpp      # 两个独立小 demo，不参与主程序
    demo_bayer_capture.cpp    #   （测板宽高 / Bayer 取图，各自单独编）
```

## 编译运行

在 **Jetson** 上：

```bash
./build.sh                    # 产出 build/wood_defect_detector
./build/wood_defect_detector  # 需要 ./models/best.engine 在位
```

需要 JetPack 自带的 CUDA / TensorRT / OpenCV，以及海康 MVS SDK（装在 `/opt/MVS/`）。

mac 上没有 OpenCV / Qt / TensorRT，只能做语法检查（`-fsyntax-only`）；真正的编译和一切行为
验证都得到 Jetson 上做。

## 判定规则在哪儿调

三层，改哪一层看是谁要改：

| 谁 | 在哪 | 说明 |
|---|---|---|
| 现场工人 | 界面输入框 → `config.ini` | 各类数量上限 + 尺寸门槛 + 三道置信度门槛。**改完立刻生效，不用重编** |
| 开发 | `include/config.h` | 上面那些数的**出厂默认值**，以及相机、模型路径、全局置信度阈值 |
| 训练 | `models/best.engine` | 模型本身。换引擎**必须重编**，否则类名整体错位还不报错 |

工人要看的说明在 `工人操作手册.docx`（一页）。界面上不暴露实现细节 —— 比如某一行右边量的
到底是「最长边」还是「对角线」，只写在 `config.h` 的注释里，不写给工人看。

## 模型

`models/best.engine` 是当前部署的。`CLASSES`（`config.h`）的顺序必须跟导出引擎时模型的
类别顺序**逐行一致**，这是唯一会静默全错的地方 —— 拿训练目录里的 `confusion_matrix.png`
坐标轴核。`models/labels.txt` 不参与运行。

历次训练目录整个留着（`models/train*/`），里面有 `args.yaml` / `results.csv` / 混淆矩阵，
是唯一能回查「那次到底训成什么样」的地方。

## 现场

- Nano 是 Modbus TCP **从站**（502 端口），PLC（繁易 FC5）做**主站**读结果
- 界面由 systemd 托管自启
- 出错先看 `nano.md`（环境）和 `ui和启动systemctl.md`（自启/界面）

## 其他文档

| 文件 | 内容 |
|---|---|
| `nano.md` | Jetson 环境、系统设置 |
| `camera.md` / `hikUse.md` | 海康相机 |
| `plc.md` | PLC Modbus 寄存器约定 |
| `ui和启动systemctl.md` | 界面自启、systemd |
| `remote.md` | 远程连接 |
| `刷机.md` | 重新刷 Jetson |
| `cpp_wood_defect_vs_tensorrt_yolo.md` | 跟 TensorRT-YOLO 的关系 |
