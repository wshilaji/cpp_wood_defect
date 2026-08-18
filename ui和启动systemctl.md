# 界面（Qt）依赖安装

编译和运行 Qt 界面需要的系统包（Ubuntu / Jetson Nano）。

## 一键安装

```bash
sudo apt update
sudo apt install -y qtbase5-dev
```

`qtbase5-dev` 一个包就够了，它会自动带上：

- 头文件：`QtCore` / `QtGui` / `QtWidgets`
- 运行库：`libQt5Core.so` / `libQt5Gui.so` / `libQt5Widgets.so`
- CMake 配置（`find_package(Qt5 Widgets)` 用的，没它编译不过）
- X11 平台插件 `platforms/libqxcb.so`（界面显示全靠它）

## 运行时报错缺 xcb / 平台插件

如果启动时提示类似 `could not load the Qt platform plugin "xcb"`，补装：

```bash
sudo apt install -y libxcb-xinerama0 libxcb-xkb1 \
                    libxkbcommon-x11-0 libxkbcommon0 \
                    libgl1-mesa-glx
```

## 中文字体（界面汉字显示）

界面汉字能正常显示，只要系统里有**任意一个中文字体**就行，**不一定要装 wqy**。

装过 `language-pack-zh-hans`（改系统中文时装的）或 GNOME 桌面的机器，通常已经自带
Noto CJK（`fonts-noto-cjk`），所以没装 wqy 界面也正常。

如果汉字显示成方块，说明系统一个中文字体都没有，才需要装（任选其一）：

```bash
sudo apt install -y fonts-noto-cjk        # 推荐，Noto 中日韩
# 或
sudo apt install -y fonts-wqy-zenhei fonts-wqy-microhei
```

查看系统现在有哪些中文字体：

```bash
fc-list :lang=zh
```

## 检查 Qt 版本

```bash
qmake --version        # 或 qmake-qt5 --version
```

## 开机自启服务（systemctl 管理）

程序以 systemd 服务运行，服务名 `wood-defect-detector`：

```bash
sudo systemctl stop wood-defect-detector                 # 停止当前程序
sudo systemctl start wood-defect-detector                # 启动
sudo systemctl restart wood-defect-detector              # 重启
sudo systemctl status wood-defect-detector               # 看状态
sudo journalctl -u wood-defect-detector -f               # 实时看日志
sudo systemctl disable --now wood-defect-detector        # 停止 + 开机不再自启
sudo systemctl enable --now wood-defect-detector         # 开机自启 + 立即启动
sudo systemctl disable --now wood-defect-detector && sudo rm /etc/systemd/system/wood-defect-detector.service   # 彻底卸载(移除服务)
```

- `stop` 是正常停止，`Restart=always` 只对崩溃自动重启，不会把主动停掉的拉起来。
- 界面由 `./run.sh` 启动，`run.sh` 会自动从桌面会话探测 DISPLAY/XAUTHORITY
  （GDM 登录前后显示号会变，写死 :0 会连不上）。
- 服务起不来先查日志：`sudo journalctl -u wood-defect-detector -n 50 --no-pager`。

### 界面"关机 / 重启电脑"按钮没反应

**现象**：确认框点"关机"后机器不关，界面无任何提示。**只在 systemd 托管后出现**。

**原因**：关机按钮执行的是 `systemctl poweroff`（非 root）。非 root 走 logind，
logind 用 polkit 校验权限。polkit 对 **活跃登录会话**里的进程默认放行
（`allow_active=yes`），但程序现在是 systemd 服务进程（`User=桌面用户`），
**不属于任何登录会话**，落到 `allow_any=auth_admin_keep` → 要求管理员认证；
服务环境里没有 polkit 认证代理，认证永远满足不了 → 调用被拒、静默失败。

**修复**：`install-systemd.sh` / `fix-systemd.sh` 会给桌面用户写一条 polkit 授权
（`/etc/polkit-1/localauthority/50-local.d/49-wood-defect-power.pkla`），
让 `poweroff/reboot`（含 multiple-sessions / ignore-inhibit 变体）免认证。
已装旧版服务的话重跑一遍即可：

```bash
sudo ./install-systemd.sh     # 或 bash fix-systemd.sh
```

**验证**：以桌面用户跑 `systemctl poweroff`，机器应立即开始关机；
不想真关就换 `systemctl status`（会打印 Access denied 之类）。

## 备注

- CMake 用的是系统 Qt（`find_package(QT NAMES Qt6 Qt5 ...)`），所以新机器编译前必须先装 `qtbase5-dev`。
- `third_party/qt5` 是另一台机器 vendor 的离线 Qt，只有 Core/Gui/Widgets 三个库、**没有平台插件**，不能拿来直接跑（缺 `libqxcb.so`），只当编译头文件/库用。
