#!/bin/bash
# ============================================================
# 木板瑕疵检测系统 - 一键编译脚本
# 适用于 Jetson Nano (JetPack 4.x+)
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "========================================"
echo "  木板瑕疵检测系统 - 编译"
echo "========================================"

# 检查 TensorRT
if ! ldconfig -p | grep -q libnvinfer; then
    echo "ERROR: 未找到 TensorRT 库，请确认 JetPack 已正确安装"
    exit 1
fi

# 检查 OpenCV
if ! pkg-config --exists opencv4 2>/dev/null; then
    if ! pkg-config --exists opencv 2>/dev/null; then
        echo "ERROR: 未找到 OpenCV，请安装: sudo apt install libopencv-dev"
        exit 1
    fi
fi

# 创建 build 目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# CMake 配置
echo ""
echo "[1/2] CMake 配置..."
cmake -DCMAKE_BUILD_TYPE=Release "${SCRIPT_DIR}"

# 编译
echo ""
echo "[2/2] 编译中..."
make -j$(nproc)

echo ""
echo "========================================"
echo "  编译完成！"
echo "  可执行文件: ${BUILD_DIR}/wood_defect_detector"
echo ""
echo "  运行: ./build/wood_defect_detector"
echo "========================================"
