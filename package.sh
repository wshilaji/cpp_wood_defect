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

# ---- 动态探测桌面会话的 DISPLAY / XAUTHORITY ----
# 为什么不能写死 DISPLAY=:0: GDM 登录前 greeter 占 :0, 用户登录后会话显示号会变
# (常见变 :1), 会话的 cookie 也只对会话自己的 X 有效。从会话进程环境里读才永远对。
find_session() {
    local p env disp xauth want
    # 权威 cookie = X server 的 -auth 参数(读得到才继续; greeter 阶段的 cookie
    # 属于 gdm、当前用户读不了, 会一直等到用户会话真正起来)。
    want="$(tr '\0' ' ' < "/proc/$(pgrep -x Xorg 2>/dev/null | head -1)/cmdline" 2>/dev/null \
            | sed -n 's/.*-auth \([^ ]*\).*/\1/p')"
    [ -n "$want" ] && [ -r "$want" ] || return 1
    # 扫当前用户进程, 找 DISPLAY 有效且 XAUTHORITY 正好等于权威 cookie 的那个。
    # 只认这份 cookie 能排除 NX 远程会话、任何过期/别的 X 的 cookie。
    # 为什么不能猜编号/猜进程名: GDM 登录前后显示号会变(:0→:1), 且猜不到
    # 具体进程名(gnome-session 的 comm 在 Linux 上被截断成 15 字符)。
    for p in $(pgrep -u "$(id -un)" 2>/dev/null); do
        env="$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null || true)"
        xauth="$(printf '%s\n' "$env" | sed -n 's/^XAUTHORITY=//p')"
        [ "$xauth" = "$want" ] || continue
        disp="$(printf '%s\n' "$env" | sed -n 's/^DISPLAY=//p')"
        [ -n "$disp" ] && [ -S "/tmp/.X11-unix/X${disp#:}" ] || continue
        export DISPLAY="$disp" XAUTHORITY="$xauth"
        return 0
    done
    return 1
}

# 等桌面会话起来(最多 ~90 秒)。起不来就带空环境 exec, 交给 systemd 重启重试。
for i in $(seq 1 90); do
    if find_session; then break; fi
    sleep 1
done

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
# 安装为 systemd 自启动服务（工厂开机自动运行）
# 用法: sudo ./install-systemd.sh
# 要点: 以桌面用户身份跑 Qt GUI; DISPLAY/XAUTHORITY 不写死, 由 run.sh 启动时
#       从桌面会话动态探测(GDM 登录前后显示号会变, 写死 :0 必连不上);
#       用 setcap 解决 502 端口权限(非 root 也能绑 <1024 端口)。
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
APP="wood_defect_detector"
SVC="wood-defect-detector"
UNIT="/etc/systemd/system/${SVC}.service"

[ "$(id -u)" = 0 ] || { echo "请用 sudo 运行: sudo ./install-systemd.sh"; exit 1; }

# ---- 确定桌面用户(sudo 执行者, 或 seat0 会话用户) ----
RUNAS="${SUDO_USER:-}"
[ -z "$RUNAS" ] && RUNAS="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$5=="seat0"{print $3; exit}')"
[ -n "$RUNAS" ] || { echo "无法确定桌面用户, 请先登录桌面再运行"; exit 1; }

# ---- 界面"关机/重启"按钮授权 ----
# 程序是 systemd 服务进程(User=RUNAS, Type=simple), 不在任何登录会话里,
# polkit 对"活跃本地会话"默认放行, 但对无会话/非活跃会话要求管理员认证
# (auth_admin_keep), 服务环境又没有 polkit 认证代理 → systemctl poweroff/reboot
# 被拒, 界面点"关机"没反应。给 RUNAS 加一条规则, 让 poweroff/reboot 免认证。
PKLA_DIR="/etc/polkit-1/localauthority/50-local.d"
PKLA="$PKLA_DIR/49-wood-defect-power.pkla"
mkdir -p "$PKLA_DIR"
cat > "$PKLA" <<PKLAEOF
[Wood defect detector power control]
Identity=unix-user:$RUNAS
Action=org.freedesktop.login1.power-off;org.freedesktop.login1.power-off-multiple-sessions;org.freedesktop.login1.power-off-ignore-inhibit;org.freedesktop.login1.reboot;org.freedesktop.login1.reboot-multiple-sessions;org.freedesktop.login1.reboot-ignore-inhibit
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLAEOF
chmod 644 "$PKLA"
# localauthority 会自动感知文件变化; 主动重启一次确保立即生效
systemctl restart polkit >/dev/null 2>&1 || true

cat > "$UNIT" <<UNITEOF
[Unit]
Description=Wood Defect Detector (木板瑕疵检测)
After=graphical.target
Wants=graphical.target

[Service]
Type=simple
User=$RUNAS
Group=$RUNAS
WorkingDirectory=$DIR
# DISPLAY / XAUTHORITY 由 run.sh 启动时动态探测, 这里不写死
ExecStart=$DIR/run.sh
Restart=always
RestartSec=3
LimitCORE=0

[Install]
WantedBy=graphical.target
UNITEOF

# 非 root 运行: 给 502 端口绑定权(一次永久; 替换二进制后需重跑本脚本)
setcap cap_net_bind_service=+ep "$DIR/$APP" 2>/dev/null \
    || echo "警告: setcap 失败, 502 端口可能绑不上"

systemctl daemon-reload
systemctl enable "$SVC"
echo "已安装(以 $RUNAS 用户运行)。"
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
