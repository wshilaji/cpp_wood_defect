#!/bin/bash
# ============================================================
# 木板瑕疵检测系统 - 打包脚本 (在 Jetson 上运行)
#
# 用法: 先 ./build.sh 编译, 再 ./package.sh
# 产物: dist/wood_defect_app/   ← 自包含运行目录, 拷到工厂机器即可运行
#
# 为什么不能只拷 bin:
#   可执行文件还依赖项目自带的 libtrtyolo.so / libcustom_plugins.so
#   / 海康 MVS 整套运行库, 以及 models/best.engine 模型文件,
#   这些不随系统装, 必须一起带走 (详见 deploy/README.md)。
#
# 目标机器预装(系统级, 本脚本不打包):
#   Qt5 (qtbase5-dev, 含 xcb 平台插件)
#   TensorRT 10.x   (libnvinfer.so.10)
#   CUDA 12         (libcudart.so.12)
#   OpenCV 4.x
#   libmodbus (本脚本尽量一并带上, 带上就不依赖目标机)
# ============================================================

set -euo pipefail

APP="wood_defect_detector"
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"
DIST_ROOT="$ROOT/dist"
DIST="$DIST_ROOT/wood_defect_app"

say()  { echo -e "\n\033[1;32m==> $*\033[0m"; }
warn() { echo -e "\033[1;33m[!] $*\033[0m" >&2; }
die()  { echo -e "\033[1;31m[ERR] $*\033[0m" >&2; exit 1; }

# ---- 0. 必须在 Linux (Jetson) 上跑 ----
[ "$(uname -s)" = "Linux" ] || die "本脚本要在 Jetson(Linux) 上运行, 当前是 $(uname -s)"

# ---- 1. 检查可执行文件 ----
[ -f "$BUILD_DIR/$APP" ] || die "找不到 $BUILD_DIR/$APP, 请先运行 ./build.sh"
[ -x "$BUILD_DIR/$APP" ] || warn "$BUILD_DIR/$APP 没有执行权限, 将 chmod +x"

say "清理旧的 dist 目录"
rm -rf "$DIST_ROOT"
mkdir -p "$DIST/lib" "$DIST/models"

# ---- 2. 可执行文件 (去调试符号, 防逆向/减小体积) ----
say "复制可执行文件 $APP"
cp "$BUILD_DIR/$APP" "$DIST/$APP"
chmod +x "$DIST/$APP"
strip "$DIST/$APP" 2>/dev/null && echo "  → strip 完成 (已去符号)" || warn "strip 不可用, 保留原文件"

# ---- 3. TensorRT-YOLO 项目自带库 (机器上必然没有) ----
say "复制 trtyolo 库"
cp "$ROOT/third_party/trtyolo/lib/libtrtyolo.so"       "$DIST/lib/"
cp "$ROOT/third_party/trtyolo/lib/libcustom_plugins.so" "$DIST/lib/"

# ---- 4. 海康 MVS 整套运行库 (与开发机版本 4.7.0.3 一致) ----
say "复制海康 MVS 运行库"
if [ -d "$ROOT/third_party/mvs/lib" ]; then
    cp -a "$ROOT/third_party/mvs/lib/." "$DIST/lib/"
    echo "  → $(find "$DIST/lib" -maxdepth 1 -type f | wc -l | tr -d ' ') 个文件 + 符号链接"
else
    warn "third_party/mvs/lib 不存在, 跳过; 目标机必须已装 /opt/MVS 且 ldconfig 能搜到"
fi

# ---- 5. libmodbus (小, 一并带走, 免依赖目标机 apt 包) ----
say "复制 libmodbus"
if lib=$(ldconfig -p 2>/dev/null | awk '/libmodbus\.so/{print $NF; exit}') && [ -n "$lib" ]; then
    cp -L "$lib" "$DIST/lib/" && echo "  → $(basename "$lib")"
else
    warn "本机没找到 libmodbus, 目标机需 sudo apt install libmodbus-dev"
fi

# ---- 6. 模型文件 (TensorRT 引擎, 与 GPU/TRT 版本绑定) ----
say "复制模型文件"
[ -f "$ROOT/models/best.engine" ] || die "缺少 $ROOT/models/best.engine, 无法打包"
cp "$ROOT/models/best.engine" "$DIST/models/"
echo "  → best.engine ($(du -h "$DIST/models/best.engine" | cut -f1))"
[ -f "$ROOT/models/labels.txt" ] && cp "$ROOT/models/labels.txt" "$DIST/models/" || warn "无 labels.txt(非必需)"

# ---- 7. 启动脚本 (设库路径 + 切到部署目录, 保证相对路径正确) ----
say "生成 run.sh"
cat > "$DIST/run.sh" <<'EOF'
#!/bin/bash
# 木板瑕疵检测 - 启动脚本 (在部署目录内运行)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"                                   # models/ 与 output/ 相对本目录
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./wood_defect_detector "$@"
EOF
chmod +x "$DIST/run.sh"

# ---- 8. 环境自检脚本 (拷到目标机后先跑一遍) ----
say "生成 check_deps.sh"
cat > "$DIST/check_deps.sh" <<'EOF'
#!/bin/bash
# 目标机运行环境自检: 列出缺什么, 不自动安装
FAIL=0
check() { # $1=描述  $2=测试命令
    if eval "$2" >/dev/null 2>&1; then echo "  [OK]  $1"; else echo "  [缺]  $1"; FAIL=1; fi
}
has_lib() { ldconfig -p 2>/dev/null | grep -qF "$1"; }

echo "==== 木板瑕疵检测 - 运行环境自检 ===="
check "CPU 架构 aarch64 (Jetson)"       '[ "$(uname -m)" = "aarch64" ]'
check "TensorRT10  libnvinfer.so.10"    'has_lib libnvinfer.so.10'
check "TRT Plugin   libnvinfer_plugin.so.10" 'has_lib libnvinfer_plugin.so.10'
check "CUDA12       libcudart.so.12"    'has_lib libcudart.so.12'
check "OpenCV 4.x   libopencv_core.so.4" 'has_lib libopencv_core.so.4'
check "Qt5 Core     libQt5Core.so.5"    'has_lib libQt5Core.so.5'
check "Qt5 Widgets  libQt5Widgets.so.5" 'has_lib libQt5Widgets.so.5'
check "Qt5 xcb 平台插件 libqxcb.so"     'find /usr/lib -name libqxcb.so 2>/dev/null | grep -q .'
check "GPU 设备节点 /dev/nvidia* 或 /dev/tegra*" \
    'ls /dev/nvidia* >/dev/null 2>&1 || ls /dev/tegra* >/dev/null 2>&1'
check "可执行文件存在"                 '[ -x ./wood_defect_detector ]'
check "模型文件 models/best.engine"    '[ -f ./models/best.engine ]'
check "当前目录可写(output/)"          '[ -w ./ ]'
check "502 端口可用(需root或setcap)"   '[ "$(id -u 2>/dev/null)" = 0 ] || command -v setcap >/dev/null 2>&1'

echo ""
if [ "$FAIL" = 0 ]; then
    echo "全部通过 → ./run.sh 启动"
else
    echo "有缺失项 → 对照 deploy/README.md 解决后重跑本脚本"
    exit 1
fi
EOF
chmod +x "$DIST/check_deps.sh"

# ---- 9. systemd 自启动安装脚本 (可选, 需 root) ----
say "生成 install-systemd.sh"
cat > "$DIST/install-systemd.sh" <<'EOF'
#!/bin/bash
# 安装为 systemd 自启动服务 (工厂开机自动运行)
# 用法: sudo ./install-systemd.sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
APP="wood_defect_detector"
SVC="wood-defect-detector"
UNIT="/etc/systemd/system/${SVC}.service"

[ "$(id -u)" = 0 ] || { echo "请用 sudo 运行: sudo ./install-systemd.sh"; exit 1; }

cat > "$UNIT" <<UNITEOF
[Unit]
Description=Wood Defect Detector (木板瑕疵检测)
After=network.target multi-user.target

[Service]
Type=simple
WorkingDirectory=$DIR
ExecStart=$DIR/run.sh
Environment=LD_LIBRARY_PATH=$DIR/lib
Restart=always
RestartSec=3
# Modbus 502 端口(<1024) 需要 root; 若想普通用户跑:
#   去掉下面 User=root, 并执行: sudo setcap cap_net_bind_service=+ep $DIR/$APP
User=root

[Install]
WantedBy=multi-user.target
UNITEOF

systemctl daemon-reload
systemctl enable "$SVC"
echo "已安装。"
echo "  启动:    sudo systemctl start  $SVC"
echo "  状态:    sudo systemctl status $SVC"
echo "  看日志:  sudo journalctl -u $SVC -f"
echo "  卸载:    sudo systemctl disable --now $SVC && sudo rm $UNIT"
EOF
chmod +x "$DIST/install-systemd.sh"

# ---- 10. 汇总 ----
say "打包完成"
echo "  部署目录: $DIST"
echo "  体积:     $(du -sh "$DIST" | cut -f1)"
echo ""
echo "  ┌─────────────────────────────────────────────────────────┐"
echo "  │ 拷贝到工厂机器(如 /opt/wood_defect_app/)后:              │"
echo "  │   1. cd wood_defect_app && ./check_deps.sh   ← 环境自检  │"
echo "  │   2. ./run.sh                                 ← 前台运行 │"
echo "  │   3. sudo ./install-systemd.sh                ← 开机自启 │"
echo "  │   4. 删除 source/build/third_party 之前, 务必备份         │"
echo "  │      best.engine 的 ONNX 源 + 本仓库源码(离线存档)。      │"
echo "  │      TensorRT 引擎跟 GPU 绑定, 换机器要重新生成。         │"
echo "  └─────────────────────────────────────────────────────────┘"
