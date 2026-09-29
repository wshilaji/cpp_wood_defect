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

### 界面"最小化"：让出桌面但不停检测

> 这一节 2026-09-29 重写过。原来这里讲的是「界面点‘退出’后多久才会自动拉起来」——
> 那套机制（hide + 原地倒数 150 秒 + 靠 systemd 拉起）连同那个「退出」按钮一起删了。

**期望行为**：工人要腾出桌面用浏览器 / 开 RustDesk 让远程协助连进来 / 改网络设置。
全屏置顶的 kiosk 窗口必须让开，但**检测不能停**——产线还在过板。

**现在的做法**：界面右下角那个按钮是「最小化」（原来叫「退出」）。点一下：

| 步骤 | 发生什么 |
|---|---|
| 1 | `hide()` —— 全屏置顶窗口从 X11 unmap，桌面立刻可用 |
| 2 | 屏幕右下角出现一个小条「返回检测界面」（`_restoreTab`，一直置顶） |
| 3 | 点那个小条 → 小条消失，`showFullScreen()` 回到全屏置顶 |

**进程从头到尾没停过**：相机继续取流、PLC 继续收触发、推理继续跑、存图继续写。
所以最小化期间过板**照样判 OK/NG**，`total`/`ng` 计数照涨——这点跟老的「退出」是本质区别
（老的会停 PLC / 停相机 / 释放显存，那 2.5 分钟里过板不判）。

几个实现上的选择，改这块之前先看一眼：

- **用 `hide()` 不用 `showMinimized()`**。主窗口是 `Frameless + WindowStaysOnTop +
  showFullScreen()`，`showMinimized()` 在这种组合下行为由 WM 决定，有的 WM 直接不理它——
  那窗口还盖着桌面、小条出现在它下面，等于按了没反应还点不到恢复。`hide()` 是 unmap，
  一定会消失，恢复完全由我们自己的小条负责，不赌 WM。
- **小条是顶层窗口**（`parent = nullptr`）。挂成主窗口的子控件不行：主窗口一藏，子控件
  一起不可见，就没有恢复入口了。
- 小条设了 `Qt::WA_QuitOnClose, false`。不然万一它被 close（而不是 hide），Qt 会以为
  「最后一个窗口关了，该退出程序」——正好撞在「主窗口已藏、只剩小条」这个状态上，
  一关就把整个检测程序带走了。
- 位置每次显示前按**当前屏幕的 `availableGeometry()`** 重算（不是 `geometry()`，前者会
  避开任务栏/程序坞），尺寸/留白在 `src/mainwindow.cpp` 的 `TAB_W/TAB_H/TAB_MARGIN`。

**退出程序现在只能靠 systemd**（或者崩溃）：主循环里没有任何界面来的退出条件了，
`running` 只在收到 SIGINT/SIGTERM 时置 false。恢复时间就是 `RestartSec`（3 秒），
主动重启和崩溃恢复一致：

```bash
sudo systemctl restart wood-defect-detector   # 重启界面（3 秒回来）
sudo systemctl stop wood-defect-detector      # 停掉，维护完再 start
```

**验证**：点「最小化」看窗口消失、右下角出现小条，此时产线过板看 `total` 是否还在涨
（涨 = 检测没停）；再点小条看是否回到全屏置顶。

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
