# 木板瑕疵检测 - 部署手册（给工厂机器用）

> 本目录是打包好的自包含运行包，**不含源码**。把整个 `wood_defect_app/` 目录
> 拷到工厂机器即可运行，不需要任何编译工具。

## 目录结构

```
wood_defect_app/
├── wood_defect_detector    # 可执行程序（已去符号）
├── run.sh                  # 启动脚本（设置库路径 + 切到部署目录）
├── check_deps.sh           # 运行环境自检（拷过去后先跑一遍）
├── install-systemd.sh      # 开机自启动安装脚本（可选）
├── lib/                    # 项目自带库（trtyolo / 海康MVS 整套）
│   ├── libtrtyolo.so
│   ├── libcustom_plugins.so
│   ├── libMvCameraControl.so -> ...（MVS 整套）
│   └── ...
└── models/
    ├── best.engine         # TensorRT 引擎（9MB）
    └── labels.txt
```

## 一、目标机器预装（系统级，已装则跳过）

| 组件 | 版本 | 检查命令 |
|------|------|---------|
| 系统 | Ubuntu / JetPack, aarch64 | `uname -m` 应输出 `aarch64` |
| Qt5 | `qtbase5-dev`（含 xcb 平台插件） | `ldconfig -p \| grep libQt5Widgets` |
| TensorRT | **10.x** | `ldconfig -p \| grep libnvinfer`（必须是 `.so.10`） |
| CUDA | **12.x** | `ldconfig -p \| grep libcudart`（必须是 `.so.12`） |
| OpenCV | 4.x | `ldconfig -p \| grep libopencv_core` |
| 中文显示 | 任意中文字体 | `fc-list :lang=zh`（没有就 `apt install fonts-noto-cjk`） |

> 注意：本包用 TensorRT 10 + CUDA 12（不是老 JetPack 4 的 TRT7/8）。版本不对
> 会在启动时报 `libnvinfer.so.10: cannot open shared object file`。

## 二、拷贝与启动

```bash
# 1. 拷贝整个目录（U盘 / scp 均可）
scp -r wood_defect_app/ 用户@工厂机器:/opt/
# 2. 首次运行先自检
cd /opt/wood_defect_app && ./check_deps.sh
# 3. 前台启动（测试）
./run.sh
# 4. 确认正常后装开机自启
sudo ./install-systemd.sh
```

## 三、开机自启动（可选）

```bash
sudo ./install-systemd.sh
# 查看状态 / 日志
sudo systemctl status wood-defect-detector
sudo journalctl -u wood-defect-detector -f
```

程序内部就是 **Modbus TCP Server（502 端口）**，PLC 直接连这个端口即可，部署时
**不需要再装/启动任何服务**。

## 四、常见问题

### 1. `cannot open shared object file: libtrtyolo.so`
库路径没设置。请用 `./run.sh` 启动（它会设 `LD_LIBRARY_PATH`），不要直接
`./wood_defect_detector`。

### 2. 启动报 `could not load the Qt platform plugin "xcb"`
机器缺 xcb 相关库。补装：
```bash
sudo apt install -y libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 libxkbcommon0 libgl1-mesa-glx
```

### 3. `cannot open shared object file: libnvinfer.so.10`
机器 TensorRT 版本不对（装的是 TRT8 不是 10）。必须安装 TensorRT 10.x 或使用配套
JetPack 系统镜像。

### 4. 引擎加载失败 / `Engine file generation version mismatch`
`models/best.engine` 是 TensorRT 引擎，**绑定 GPU 型号 + TRT 版本**。出现此错说明
机器和生成引擎的机器不是同一套环境。解决：
- 优先：在**工厂这台机器**上重新跑 `./build.sh && ./package.sh` 生成新引擎；
- 或：换用与生成环境一致的 GPU/系统镜像。

> ⚠️ **重要**：源码/ONNX 已从交付物中删除，但请务必**在别处离线备份一份**
> （`best.engine` 对应的 ONNX 模型 + 源码仓库），否则将来换机器无法重新生成引擎。

### 5. `Address already in use` / 端口 502 起不来
502 < 1024 需要 root 权限。`install-systemd.sh` 已给程序 `setcap` 授权（非 root 也
能绑 502）；手动 `./run.sh` 请用 `sudo`，或手动加权限：
```bash
sudo setcap cap_net_bind_service=+ep ./wood_defect_detector
```
> 注意：替换了可执行文件后 `setcap` 会丢，需重跑 `install-systemd.sh` 或重新 setcap。

### 6. 相机连不上（GigE）
- 相机和机器要在同一网段：相机 `192.168.2.10`，机器网卡 `192.168.2.100`。
- 网卡 MTU 设 9000（Jumbo Frame）：`sudo nmcli con mod 有线连接 ipv4.mtu 9000 && sudo nmcli con up 有线连接`。
- 相机 IP 用海康 SADP 工具改。

### 7. 图像不显示 / 黑屏
- GPU 设备节点：`ls /dev/nvidia*`（没有说明驱动没装好或权限不足，程序需在 root 或 video 组下跑）。
- `./output/` 目录需要有写权限（`run.sh` 已自动 cd 到部署目录）。

## 五、交付物清单（交付前确认）

- [ ] 整个 `wood_defect_app/` 目录（含 `lib/` `models/`）
- [ ] 工厂机器跑通 `./check_deps.sh` 无缺失
- [ ] `./run.sh` 前台能出画面、PLC 能触发
- [ ] `sudo ./install-systemd.sh` 后重启自动运行
- [ ] **OFFLINE 备份**：ONNX + 源码仓库（仅内部留存，不随交付）
