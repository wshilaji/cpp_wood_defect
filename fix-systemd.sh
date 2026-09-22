#!/bin/bash
# ============================================================
# 修复 systemd 自启服务 (无需重新打包)
#
# 背景: GDM 登录前 greeter 占 :0, 登录后会话显示号会变(常见变 :1),
#       所以 unit 里写死 DISPLAY=:0 / 等 /tmp/.X11-unix/X0 必失败。
#       本脚本把 run.sh 换成"从 X server 反查 DISPLAY/XAUTHORITY"版本,
#       并精简 unit(去掉写死的 DISPLAY/XAUTHORITY/ExecStartPre)。
#
# 用法: bash fix-systemd.sh [部署目录]
#       默认取 ~/wood_defect_app ; 若在部署目录内运行则用当前目录
# 说明: 需要 sudo 权限(会写 /etc/systemd/system/ 并 restart 服务)
# ============================================================
set -e

# ---- 1. 定位部署目录 ----
if [ -n "$1" ]; then
    APP_DIR="$1"
elif [ -f ./run.sh ] && [ -f ./wood_defect_detector ]; then
    APP_DIR="$(pwd)"
elif [ -d "$HOME/wood_defect_app" ]; then
    APP_DIR="$HOME/wood_defect_app"
else
    echo "找不到部署目录。用法: bash fix-systemd.sh /path/to/wood_defect_app" >&2
    exit 1
fi
APP_DIR="$(cd "$APP_DIR" && pwd)"
APP="wood_defect_detector"
SVC="wood-defect-detector"
UNIT="/etc/systemd/system/${SVC}.service"
[ -f "$APP_DIR/$APP" ] || { echo "部署目录里没有 $APP: $APP_DIR" >&2; exit 1; }

# ---- 2. 桌面用户(服务以谁的身份跑) ----
RUNAS="${SUDO_USER:-$(id -un)}"
[ -n "$RUNAS" ] || RUNAS="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$5=="seat0"{print $3; exit}')"
[ -n "$RUNAS" ] || { echo "无法确定桌面用户" >&2; exit 1; }

# ---- 3. 覆盖 run.sh: 动态探测 DISPLAY / XAUTHORITY ----
cat > "$APP_DIR/run.sh" <<'EOF'
#!/bin/bash
# 木板瑕疵检测 - 启动脚本 (在部署目录内运行)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- 从 X server 反查 DISPLAY / XAUTHORITY ----
# GDM 登录前后显示号会变(greeter :0 → 会话 :1), 猜编号/猜进程名都不稳。
# 权威 cookie = X server 的 -auth 参数; 再从当前用户进程里找一个用这份
# cookie 且 DISPLAY 有效的会话进程。这样能排除 NX 远程会话等杂牌 cookie。
find_session() {
    local p env disp xauth want
    want="$(tr '\0' ' ' < "/proc/$(pgrep -x Xorg 2>/dev/null | head -1)/cmdline" 2>/dev/null \
            | sed -n 's/.*-auth \([^ ]*\).*/\1/p')"
    [ -n "$want" ] && [ -r "$want" ] || return 1
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

# ---- 启动前先清一次存图(双保险里的脚本那层) ----
# 放 exec 之前且同步跑完: 程序内部那层是"目录 ≥ 60G 就停写", 上次关机时目录本来就满的话,
# 不先删, 程序一起来就进停写状态, 得等 timer 跑完才恢复。清理脚本自己有 mkdir 锁防并发,
# 这里 || true 是兜底 —— run.sh 有 set -e, 清理出岔子不该拦着主程序启动。
if [ -x "$DIR/cleanup_images.sh" ]; then
    "$DIR/cleanup_images.sh" --quiet || true
fi

exec ./wood_defect_detector "$@"
EOF
chmod +x "$APP_DIR/run.sh"

# ---- 4. 精简 unit (DISPLAY/XAUTHORITY 由 run.sh 动态探测, 不写死) ----
sudo tee "$UNIT" > /dev/null <<UNITEOF
[Unit]
Description=Wood Defect Detector (木板瑕疵检测)
After=graphical.target
Wants=graphical.target

[Service]
Type=simple
User=$RUNAS
Group=$RUNAS
WorkingDirectory=$APP_DIR
# DISPLAY / XAUTHORITY 由 run.sh 启动时动态探测
ExecStart=$APP_DIR/run.sh
Restart=always
RestartSec=3
LimitCORE=0

[Install]
WantedBy=graphical.target
UNITEOF

# ---- 5. 界面"关机/重启"按钮授权 ----
# 程序是 systemd 服务进程, 不在任何登录会话里, polkit 对无会话/非活跃会话
# 默认要求管理员认证(auth_admin_keep)且服务环境没有认证代理 → systemctl
# poweroff/reboot 被拒, 界面点"关机"没反应。给 RUNAS 加规则, 免认证。
PKLA_DIR="/etc/polkit-1/localauthority/50-local.d"
sudo mkdir -p "$PKLA_DIR"
sudo tee "$PKLA_DIR/49-wood-defect-power.pkla" > /dev/null <<PKLAEOF
[Wood defect detector power control]
Identity=unix-user:$RUNAS
Action=org.freedesktop.login1.power-off;org.freedesktop.login1.power-off-multiple-sessions;org.freedesktop.login1.power-off-ignore-inhibit;org.freedesktop.login1.reboot;org.freedesktop.login1.reboot-multiple-sessions;org.freedesktop.login1.reboot-ignore-inhibit
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLAEOF
# localauthority 会自动感知文件变化; 主动重启一次确保立即生效
sudo systemctl restart polkit >/dev/null 2>&1 || true

# ---- 6.5 存图清理定时器(双保险里的脚本那层) ----
# 程序 Restart=always 一跑几周, 期间只有 C++ 那层"超 60G 停写", 没人删旧的。
# 有 timer 才能在不重启的情况下把空间腾出来, 程序下一轮复查(30 秒)自动恢复存图。
if [ -x "$APP_DIR/cleanup_images.sh" ]; then
    sudo tee /etc/systemd/system/wood-defect-cleanup.service > /dev/null <<CLEANUPEOF
[Unit]
Description=Wood Defect Detector 存图清理（raw+result 超 60G 时从最老的删到 54G）

[Service]
Type=oneshot
User=$RUNAS
Group=$RUNAS
WorkingDirectory=$APP_DIR
# 清理是后台杂活, 别跟检测抢 CPU/IO(相机那条链路对延迟敏感)
Nice=10
IOSchedulingClass=idle
ExecStart=$APP_DIR/cleanup_images.sh --quiet
CLEANUPEOF

    sudo tee /etc/systemd/system/wood-defect-cleanup.timer > /dev/null <<TIMEREOF
[Unit]
Description=每小时检查一次存图目录容量

[Timer]
OnBootSec=5min
OnUnitActiveSec=1h

[Install]
WantedBy=timers.target
TIMEREOF

    sudo systemctl enable --now wood-defect-cleanup.timer \
        || echo "警告: 清理定时器启用失败, 存图只剩 C++ 那层(超 60G 停写)" >&2
else
    echo "提示: $APP_DIR 里没有 cleanup_images.sh（重新打包时会带上），" >&2
    echo "      存图清理只剩 C++ 那层（超 60G 停写），不会自动删旧的。" >&2
fi

# ---- 7. 502 端口权限(非 root 绑 <1024) ----
sudo setcap cap_net_bind_service=+ep "$APP_DIR/$APP" 2>/dev/null \
    || echo "警告: setcap 失败, 502 端口可能绑不上" >&2

# ---- 8. 重载并重启 ----
sudo systemctl daemon-reload
sudo systemctl enable "$SVC"
sudo systemctl restart "$SVC"
echo ""
echo "已重启 $SVC, 等 8 秒看状态:"
sleep 8
sudo systemctl status "$SVC" --no-pager
