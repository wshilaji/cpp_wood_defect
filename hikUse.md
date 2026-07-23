问题1。就是mac ssh连接 nano连接不上。 nano这个时候网线连网，有线连相机。但是因为nano的网线和有线在同一个网段下。
导致mac ssh 连接不上nano

你的无线网卡已经固定在 192.168.10.x 了（这是你家的Wi-Fi网络），所以把有线网卡（连海康相机的）改成 192.168.2.x 或其他非 10.x 的网段。

第1步：先把有线网卡IP改掉
bash
ip addr show
# 之后看 enP8p1s0 这个是网线的
# 删除当前的有线IP
sudo ip addr del 192.168.10.100/24 dev enP8p1s0
# 添加新的IP（192.168.2.x 网段）
sudo ip addr add 192.168.2.100/24 dev enP8p1s0

第2步：修改海康相机的IP
相机现在的IP应该也是 192.168.10.x，需要改成和有线网卡一样的 192.168.2.x 网段。

用海康SADP工具或者浏览器访问相机当前的IP，把相机的IP改为：

IP: 192.168.2.10（或 2.x 网段内其他未占用的IP）

子网掩码: 255.255.255.0

网关: 留空或填 192.168.2.1

第3步：验证连接
在Nano上ping相机的新IP：

bash
ping 192.168.2.10
如果通，就说明Nano和相机已经通过有线网络正常通信了。


直接用 ip 命令临时设置（立刻生效） 这个方法重启后会丢失。
sudo ip link set dev enP8p1s0 mtu 9000



使用 nmcli（推荐，永久生效）
bash
# 1. 查看有线连接的名称
nmcli con show
# 1. 先确保有线网卡没有被其他配置占用
sudo nmcli device set enP8p1s0 managed yes
# 2. 修改 "Wired connection 1" 的 MTU
sudo nmcli con mod "Wired connection 1" 802-3-ethernet.mtu 9000
# 3. 重启这个连接
sudo nmcli con down "Wired connection 1" && sudo nmcli con up "Wired connection 1"
# 4. 验证
ip addr show enP8p1s0 | grep mtu
