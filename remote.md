sudo dpkg -i nomachine_9.3.7_1_arm64.deb

安装完成后，NoMachine 服务会自动启动，无需额外操作。可以用以下命令确认服务状态：
bash

sudo systemctl status nxserver
看到 active (running) 就说明安装成功、服务正常运行了。
sudo dpkg -i nomachine_*_arm64.deb
sudo dpkg -i nomachine.deb
你可以用以下命令查看这个 .deb 包的架构信息：
bash
dpkg-deb -f nomachine.deb Architecture
如果输出 arm64，就是 ARM 版本，可以直接安装
如果输出 amd64，就是 x86 版本，不能在 Nano 上安装


第三步：Nano 端安装（Ubuntu 22.04）
通过 SSH 或终端连上 Nano，依次执行以下命令：

# 1. 一键安装 Tailscale
curl -fsSL https://tailscale.com/install.sh | sh

# 2. 设置开机自启
sudo systemctl enable --now tailscaled

# 3. 启动并登录
sudo tailscale up
执行完第三条命令后，终端会弹出一个类似 https://login.tailscale.com/a/xxxxxx 的链接。你把这个链接复制到浏览器里打开，用你刚才在 Mac 上用的那个 GitHub 账号 授权登录，两台设备就彻底打通了！
