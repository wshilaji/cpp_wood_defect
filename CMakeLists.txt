cmake_minimum_required(VERSION 3.14)
project(wood_defect_detector VERSION 2.0.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3")
endif()

# ---- TensorRT-YOLO 库 ----
set(TRTYOLO_PATH "" CACHE PATH "TensorRT-YOLO 安装路径 (包含 include/ 和 lib/)")
find_path(TRTYOLO_INCLUDE trtyolo.hpp
    PATHS ${TRTYOLO_PATH}/include /usr/local/include/trtyolo REQUIRED)
find_library(TRTYOLO_LIB trtyolo
    PATHS ${TRTYOLO_PATH}/lib /usr/local/lib REQUIRED)

# ---- OpenCV ----
find_package(OpenCV REQUIRED)

# ---- CUDA ----
find_package(CUDAToolkit REQUIRED)

# ---- 海康 MVS SDK ----
set(MVS_PATH "/opt/MVS" CACHE PATH "海康 MVS SDK 路径")
find_library(MVS_LIB MvCameraControl PATHS ${MVS_PATH}/lib/aarch64 ${MVS_PATH}/lib/64 ${MVS_PATH}/lib REQUIRED)
find_path(MVS_INCLUDE MvCameraControl.h PATHS ${MVS_PATH}/include REQUIRED)

# ---- 源文件 ----
set(SOURCES
    src/main.cpp
    src/camera.cpp
    src/postprocessor.cpp
)

set(HEADERS
    include/config.h
    include/camera.h
    include/infer.h
    include/postprocessor.h
)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${TRTYOLO_INCLUDE}
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${TRTYOLO_LIB}
    ${MVS_LIB}
    ${OpenCV_LIBS}
    CUDA::cudart
    pthread
)

message(STATUS "=== Wood Defect Detector v2.0 ===")
message(STATUS "  TensorRT-YOLO: ${TRTYOLO_LIB}")
message(STATUS "  MVS SDK:       ${MVS_LIB}")
message(STATUS "  OpenCV:        ${OpenCV_VERSION}")
