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

### 界面点"退出"后多久才会自动拉起来

**期望行为**：点"退出"是为了腾出桌面（开 RustDesk 让远程协助连进来、改网络设置等），
全屏置顶的 kiosk 窗口必须马上消失；但又不希望它一直不起来，所以 **2.5 分钟**后自动恢复。

**实现**：延时不在 systemd 里，在程序里（`Config::EXIT_RESTART_DELAY_SEC`，单位秒，
默认 150 = 2.5 分钟）。`RestartSec` 保持 3 秒不动，好处是**崩溃恢复仍然只要 3 秒**——
真挂了是产线停机的紧急情况，不能跟着一起等下去。

点"退出"之后的顺序：

| 步骤 | 发生什么 |
|---|---|
| 1 | `win.hide()` —— 全屏置顶窗口从 X11 unmap，桌面立刻可用（不是最小化） |
| 2 | 停 PLC / 停相机 / 等存图线程收尾（此时界面已看不见） |
| 3 | 出 try 作用域：推理引擎、PlcLink、SaveWorker 析构，显存释放、线程 join |
| 4 | 进程**不退出**，原地倒数 2.5 分钟（每秒查一次信号） |
| 5 | 进程正常退出 → systemd 按 `RestartSec=3` 拉起，界面回来 |

第 4 步是整件事的关键：systemd 是 `Type=simple`，只看主进程死没死，**进程活着就不会重启**，
所以这 2.5 分钟界面不会回来。这段时间进程里没有窗口、没有相机、没有 GPU 占用、没有后台线程，
纯粹是个"闹钟"。

> **注意**：点"退出"就是真的停检测——相机不取流、PLC 不响应、不推理，这 2.5 分钟里
> **过板不会判 OK/NG**。现在的行为也是这样（退出即停），只是以前 3 秒就重启、现在 2.5 分钟。
> 如果产线那 2.5 分钟还在过板，这是不行的。

**常用操作**（两个都是立即生效，不用等满 2.5 分钟，因为倒数循环每秒查一次信号）：

```bash
sudo systemctl restart wood-defect-detector   # 界面提前回来
sudo systemctl stop wood-defect-detector      # 让界面一直别回来（维护完再 start）
```

**调整时长**：改 `include/config.h` 里的 `EXIT_RESTART_DELAY_SEC` 后重新编译（改成 `0`
就是恢复成"退出即走、3 秒重启"的老行为）。

**验证**：点"退出"看窗口消失，`sudo journalctl -u wood-defect-detector -n 20` 会看到
`[Exit] 界面已隐藏, 150 秒后再由 systemd 自动拉起`；再 `systemctl start` 一下就能提前叫回来。

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
