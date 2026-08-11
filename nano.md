# Jetson Nano 环境搭建 - 禁用系统更新


### 安装 nvidia-jetpack
```bash
sudo apt install nvidia-jetpack
```
### 安装 jetson-stats
```bash
sudo -H pip3 install -U jetson-stats
```
### 配置 CUDA 环境变量
先找到 tensorrt 版本 和nvcc 所在路径：10.3tensorrt 和12.6的cuda是适配的
```
dys@dys-desktop:~$ sudo find /usr -name libnvinfer.so*
/usr/lib/aarch64-linux-gnu/libnvinfer.so
/usr/lib/aarch64-linux-gnu/libnvinfer.so.10.3.0
/usr/lib/aarch64-linux-gnu/libnvinfer.so.10
sudo find /usr -name nvcc
```
例如输出 `/usr/local/cuda-12.6/bin/nvcc`，则将对应路径写入 `~/.bashrc`：
```bash
echo 'export PATH=/usr/local/cuda-12.6/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda-12.6/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc
```
验证是否配置成功：
```bash
nvcc --version
```
## 锁定 NVIDIA 核心包版本

```bash
sudo apt-mark hold nvidia-l4t-kernel nvidia-l4t-kernel-dtbs nvidia-l4t-initrd
sudo apt-mark hold nvidia-l4t-core nvidia-jetpack
sudo apt-mark hold nvidia-l4t-kernel-headers nvidia-l4t-kernel-oot-headers nvidia-l4t-kernel-oot-modules
```

## 禁用更新通知服务

```bash
sudo systemctl mask update-notifier.service
sudo systemctl mask update-notifier-motd.service
```

## 关闭 unattended-upgrades（后台自动更新服务）

```bash
sudo systemctl stop unattended-upgrades
sudo systemctl disable unattended-upgrades
```

## 关闭 apt 定时更新

```bash
sudo systemctl stop apt-daily.timer
sudo systemctl disable apt-daily.timer
sudo systemctl stop apt-daily-upgrade.timer
sudo systemctl disable apt-daily-upgrade.timer
```

---

## 常见问题
**Q: docker 不用自己安装？**

A: 不用， 在apt install nvidia-jetpack 之后会自己就安装好docker

**Q: 模型文件名有要求吗？**

A: 没有，叫什么 `.engine` 都行，代码只读文件内容。

**Q: ultralytics 必须装特定版本吗？**

A: 不需要。ultralytics 和 TensorRT-YOLO 之间通过 ONNX 文件交互，没有版本耦合。装最新版即可。

**Q: 除了 ultralytics 的官方模型，其他 YOLO 能跑吗？**

A: 能。只要拿到 ONNX，就能用 `trtyolo-export` + `trtexec` 转成 engine，然后用 TensorRT-YOLO 推理。不同 YOLO 变体只是第一步导出 ONNX 的工具不同。

**Q: classify 和 detect 怎么选？**

A: classify 回答"整张图是什么"（一个标签），detect 回答"目标**在哪里**、是什么"（框 + 类别 + 位置）。需要知道目标位置就用 detect。

**Q: Jetson 设备上 `docker pull` 报 `no such host` 错误怎么办？**

错误示例：
```
Error response from daemon: failed to resolve reference "docker.io/ultralytics/ultralytics:latest-jetson-jetpack6":
dial tcp: lookup docker.mirrors.ustc.edu.cn on 127.0.0.53:53: no such host
```

A: 这是因为 Docker 镜像加速器地址失效或 DNS 无法解析。解决方法——更换稳定的镜像加速器。

编辑 Docker 配置文件：
```bash
sudo nano /etc/docker/daemon.json
```

将内容修改为（确保 JSON 格式正确，注意逗号）：
```json
{
    "runtimes": {
        "nvidia": {
            "args": [],
            "path": "nvidia-container-runtime"
        }
    },
    "registry-mirrors": [
    	"https://docker.xuanyuan.me",
    	"https://docker.1ms.run",
    	"https://registry.docker-cn.com",
    	"https://docker.m.daocloud.io",
    	"https://docker.mirrors.ustc.edu.cn",
    	"https://hub-mirror.c.163.com"
    ]
} 
```

保存后重启 Docker 服务：
```bash
sudo systemctl daemon-reload
sudo systemctl restart docker
```

然后重新拉取镜像：
```bash
docker pull ultralytics/ultralytics:latest-jetson-jetpack6
```

---

**Q: Jetson 设备上如何安装 Firefox 浏览器？**

A: 使用 Flatpak 安装是最稳妥的方案，可以绕过 Snap 的兼容性问题。

安装 Flatpak：
```bash
sudo apt install flatpak
sudo apt install gnome-software-plugin-flatpak
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
```

重启系统，然后安装 Firefox：
```bash
flatpak remote-modify flathub --url=https://mirror.sjtu.edu.cn/flathub
flatpak install firefox
```

安装过程中会出现交互式选择，按以下操作：
```
Looking for matches…
Similar refs found for 'firefox' in remote 'flathub' (system):

   1) app/org.mozilla.firefox.BaseApp/aarch64/23.08
   2) app/org.mozilla.firefox/aarch64/stable
   3) app/org.mozilla.firefox.BaseApp/aarch64/24.08
   4) app/org.mozilla.firefox.BaseApp/aarch64/25.08

Which do you want to use (0 to abort)? [0-4]: 2
```

选 `2`（`org.mozilla.firefox/aarch64/stable`），随后提示安装运行时依赖：
```
Required runtime for org.mozilla.firefox/aarch64/stable (runtime/org.freedesktop.Platform/aarch64/25.08) found in remote flathub
Do you want to install it? [Y/n]: y
```

输入 `y` 确认即可继续安装。

如果以后想卸载，可以用：
```bash
flatpak uninstall org.mozilla.firefox
```

---

**Q: Jetson 设备上如何生成 SSH 密钥连接 GitHub？**

A: 在 Jetson Nano 上打开终端，运行以下命令生成密钥对（记得把邮件地址换成你 GitHub 账号关联的邮箱）：

```bash
ssh-keygen -t ed25519 -C "你的GitHub邮箱"
```

生成后查看公钥内容：
```bash
cat ~/.ssh/id_ed25519.pub
```

将输出的公钥复制并添加到 GitHub 的 **Settings → SSH and GPG keys → New SSH key** 即可。

---

## 基础开发工具安装

### 安装 pip

```bash
sudo apt update
sudo apt install python3-pip python3-dev
```

### 安装 cmake

```bash
sudo apt install cmake build-essential
```

### 安装 pybind11

```bash
pip3 install "pybind11[global]" -i https://pypi.tuna.tsinghua.edu.cn/simple
```

### 编译 TensorRT-YOLO

```bash
rm -rf build
cmake -S . -B build -D TRT_PATH=/usr -D BUILD_PYTHON=ON -D CMAKE_INSTALL_PREFIX=./install
```

### 配置 Docker 使用 NVIDIA 运行时

> 注意：不需要安装 `nvidia-container-toolkit`，Jetpack 已经自带。

```bash
sudo nvidia-ctk runtime configure --runtime=docker
```

这条命令会修改 Docker 的配置文件（通常是 `/etc/docker/daemon.json`），告诉 Docker 去哪里找 nvidia 运行时。

重启 Docker 服务：
```bash
sudo systemctl daemon-reload
sudo systemctl restart docker
```

### 启动支持 CUDA 的容器

```bash
docker run -it --runtime=nvidia \
  --name ultralytics \
  -v /home/dys/dockerenv/ultralytics:/workspace \
  -w /workspace \
  ultralytics/ultralytics:latest-jetson-jetpack6
```

之后 Docker 容器内就可以直接使用 CUDA 了。

### 重启已存在的容器

如果容器已经创建过，直接 start 然后 attach 进去即可：

```bash
docker start ultralytics
docker attach ultralytics
```

进入容器后验证 CUDA 是否可用：
```python
import torch
print(torch.cuda.is_available())
```

### labelme 标注转 YOLO 格式

```bash
labelme2yolo --json_dir all/ --val_size 0.15 --test_size 0.15
```
### autodl 模型训练
```
nohup yolo detect train data=/root/dys/YOLODataset/dataset.yaml model=yolo11n.pt epochs=300 imgsz=640 device=0 batch=16 amp=False > train.log 2>&1 &
```

---

**Q: Windows 下标注的 JSON 文件在 Linux 下换行符不对怎么办？**

A: Windows 使用 `\r\n`（CRLF），Linux 使用 `\n`（LF）。Windows 下标注的文件在 Linux 下查看时会显示 `^M$`。训练时**必须使用 Linux 格式（LF）**，否则 `uniq` 等命令会认为行不同。

检查文件换行符：
```bash
cat -A 0704_20260708150703062.json | head -20
```

如果每行末尾有 `^M$`，说明是 Windows 格式。

使用 `dos2unix` 批量转换：
```bash
sudo apt install dos2unix -y
dos2unix *.json
```

---

**Q: Mac 和 Jetson 之间怎么传文件？**

A: 使用 `scp` 命令，在 Mac 终端执行：

从 Jetson 拉取文件到 Mac 当前目录：
```bash
scp dys@100.64.45.96:/home/dys/muye/YOLODataset.tar.gz .
```

从 Mac 推送文件到 Jetson：
```bash
scp local_file.tar.gz dys@100.64.45.96:/home/dys/muye/
```

传文件夹加 `-r`：
```bash
scp -r local_folder dys@100.64.45.96:/home/dys/muye/
```

从 Mac 推送到 Nano（将 `dys` 换成 Nano 的用户名，IP 换成 Nano 的局域网 IP）：
```bash
scp ~/Desktop/YOLODataset.tar.gz dys@<Jetson_Nano_IP>:/home/dys/muye/all/
```


跟plc modbus通信需要装一个专门的lib  nano可以写一个自定义的tcp。plc那边太蠢。写一个
```bash
sudo apt install libmodbus-dev
```
