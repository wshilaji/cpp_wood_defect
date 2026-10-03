# 木板瑕疵检测系统

木板流水线上的实时瑕疵检测，跑在 **Jetson Nano** 上。

- **相机** — 海康 MV-CS050-60GC（2448×2048），由 PLC 触发拍照
- **推理** — TensorRT 在 GPU 上跑 YOLO，识别 14 类瑕疵
- **判定** — 按各类的数量 / 尺寸门槛判 OK / NG
- **PLC** — Modbus TCP（Nano 做从站，502 口）：HR0 触发、HR1 写结果、HR2 报就绪
- **界面** — Qt 工人操作面板，systemd 托管自启

编译运行（Jetson 上，需要 JetPack 的 CUDA/TensorRT/OpenCV + 海康 MVS SDK）：

```bash
./build.sh && ./build/wood_defect_detector   # 需要 ./models/best.engine 在位
```

具体配置见 `include/config.h` 的注释，各模块细节见同名的 `.md`。
