cmake_minimum_required(VERSION 3.14)
project(wood_defect_detector VERSION 1.0.0 LANGUAGES CXX CUDA)

# ============================================================
# C++17 标准
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# ============================================================
# Jetson Nano 优化编译选项
# ============================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=armv8.2-a -mtune=cortex-a57")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3")
endif()

# 启用 TensorRT 和 CUDA
find_package(CUDA REQUIRED)

# ============================================================
# TensorRT
# ============================================================
set(TENSORRT_PATH "/usr/src/tensorrt" CACHE PATH "TensorRT 安装路径")
find_library(TENSORRT_LIB nvinfer PATHS
    ${TENSORRT_PATH}/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib
    REQUIRED)
find_library(TENSORRT_PLUGIN_LIB nvinfer_plugin PATHS
    ${TENSORRT_PATH}/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib)
find_path(TENSORRT_INCLUDE NvInfer.h PATHS
    ${TENSORRT_PATH}/include
    /usr/include/aarch64-linux-gnu
    /usr/include
    REQUIRED)

# ============================================================
# 海康 MVS SDK
# ============================================================
set(MVS_PATH "/opt/MVS" CACHE PATH "海康 MVS SDK 安装路径")
find_library(MVS_LIB MvCameraControl PATHS
    ${MVS_PATH}/lib/aarch64
    ${MVS_PATH}/lib/armhf
    ${MVS_PATH}/lib/64
    ${MVS_PATH}/lib/32
    ${MVS_PATH}/lib
    /usr/lib
    REQUIRED)
find_path(MVS_INCLUDE MvCameraControl.h PATHS
    ${MVS_PATH}/include
    /usr/include
    REQUIRED)
message(STATUS "  MVS SDK:       ${MVS_LIB}")

# ============================================================
# OpenCV (with CUDA on Jetson)
# ============================================================
find_package(OpenCV REQUIRED)

# ============================================================
# CUDA Toolkit
# ============================================================
find_library(CUDA_LIB cuda PATHS /usr/local/cuda/lib64 /usr/local/cuda/lib REQUIRED)
find_library(CUDART_LIB cudart PATHS /usr/local/cuda/lib64 /usr/local/cuda/lib REQUIRED)
find_path(CUDA_INCLUDE cuda_runtime.h PATHS /usr/local/cuda/include REQUIRED)

# ============================================================
# 源文件
# ============================================================
set(SOURCES
    src/main.cpp
    src/camera.cpp
    src/preprocessor.cpp
    src/trt_engine.cpp
    src/postprocessor.cpp
)

set(HEADERS
    include/config.h
    include/camera.h
    include/preprocessor.h
    include/trt_engine.h
    include/postprocessor.h
)

# ============================================================
# 可执行文件
# ============================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${TENSORRT_INCLUDE}
    ${CUDA_INCLUDE}
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${TENSORRT_LIB}
    ${TENSORRT_PLUGIN_LIB}
    ${CUDA_LIB}
    ${CUDART_LIB}
    ${MVS_LIB}
    ${OpenCV_LIBS}
    pthread
)

# ============================================================
# 打印配置信息
# ============================================================
message(STATUS "========================================")
message(STATUS "  Wood Defect Detector - Build Config")
message(STATUS "========================================")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  TensorRT Lib:  ${TENSORRT_LIB}")
message(STATUS "  OpenCV:        ${OpenCV_VERSION}")
message(STATUS "  CUDA:          ${CUDA_VERSION}")
message(STATUS "========================================")
