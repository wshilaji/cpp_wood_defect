cmake_minimum_required(VERSION 3.14)
project(wood_defect_detector VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
endif()

# ---- Threads ----
find_package(Threads REQUIRED)

# ---- TensorRT-YOLO 库（本地 third_party）----
set(TRTYOLO_DIR "${CMAKE_SOURCE_DIR}/third_party/trtyolo"
    CACHE PATH "TensorRT-YOLO install 目录")
find_path(TRTYOLO_INCLUDE trtyolo.hpp
    PATHS ${TRTYOLO_DIR}/include/trtyolo ${TRTYOLO_DIR}/include REQUIRED)
find_library(TRTYOLO_LIB trtyolo
    PATHS ${TRTYOLO_DIR}/lib REQUIRED)

# ---- OpenCV ----
find_package(OpenCV REQUIRED)

# ---- 海康 MVS SDK（本地 third_party）----
set(MVS_DIR "${CMAKE_SOURCE_DIR}/third_party/mvs"
    CACHE PATH "海康 MVS SDK 路径")
find_library(MVS_LIB MvCameraControl
    PATHS ${MVS_DIR}/lib REQUIRED)
find_path(MVS_INCLUDE MvCameraControl.h
    PATHS ${MVS_DIR}/include REQUIRED)

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
    Threads::Threads
)

message(STATUS "=== Wood Defect Detector v2.0 ===")
message(STATUS "  TensorRT-YOLO: ${TRTYOLO_LIB}")
message(STATUS "  MVS SDK:       ${MVS_LIB}")
message(STATUS "  OpenCV:        ${OpenCV_VERSION}")
