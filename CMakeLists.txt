cmake_minimum_required(VERSION 3.14)
project(wood_defect_detector VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
endif()

# ---- Threads ----
find_package(Threads REQUIRED)

# ---- libmodbus (Modbus TCP Server) ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(MODBUS REQUIRED libmodbus)

# ---- TensorRT-YOLO 库（本地 third_party）----
set(TRTYOLO_DIR "${CMAKE_SOURCE_DIR}/third_party/trtyolo"
    CACHE PATH "TensorRT-YOLO install 目录")
find_path(TRTYOLO_INCLUDE trtyolo.hpp
    PATHS ${TRTYOLO_DIR}/include/trtyolo ${TRTYOLO_DIR}/include REQUIRED)
find_library(TRTYOLO_LIB trtyolo
    PATHS ${TRTYOLO_DIR}/lib REQUIRED)

# ---- OpenCV ----
find_package(OpenCV REQUIRED)

# ---- Qt (Widgets, 界面) ----
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

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
    src/plc_link.cpp
    src/mainwindow.cpp
    src/logger.cpp
)

set(HEADERS
    include/config.h
    include/camera.h
    include/cameraguard.h
    include/infer.h
    include/postprocessor.h
    include/plc_link.h
    include/saveworker.h
    include/mainwindow.h
    include/logger.h
)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${TRTYOLO_INCLUDE}
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
    ${MODBUS_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${TRTYOLO_LIB}
    ${MVS_LIB}
    ${OpenCV_LIBS}
    Qt${QT_VERSION_MAJOR}::Widgets
    Threads::Threads
    ${MODBUS_LIBRARIES}
)

# ---- Example Demo（YOLO11n COCO 模型 + 相机） ----
add_executable(example_demo
    example/example_demo.cpp
    src/camera.cpp
    src/plc_link.cpp
    src/logger.cpp
    include/camera.h
    include/plc_link.h
    include/logger.h
)
target_include_directories(example_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${TRTYOLO_INCLUDE}
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
    ${MODBUS_INCLUDE_DIRS}
)
target_link_libraries(example_demo PRIVATE
    ${TRTYOLO_LIB}
    ${MVS_LIB}
    ${OpenCV_LIBS}
    Threads::Threads
    ${MODBUS_LIBRARIES}
)

# ---- 木板长宽测量 Demo（相机标定 + 矩形拟合） ----
add_executable(demo_widthheight
    demo_widthheight.cpp
)
target_include_directories(demo_widthheight PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(demo_widthheight PRIVATE
    ${OpenCV_LIBS}
)

# ---- Bayer 采集测速 & 对比 Demo（独立，仅 OpenCV + MVS） ----
add_executable(demo_bayer_capture
    demo_bayer_capture.cpp
)
target_include_directories(demo_bayer_capture PRIVATE
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(demo_bayer_capture PRIVATE
    ${MVS_LIB}
    ${OpenCV_LIBS}
    Threads::Threads
)

message(STATUS "=== Wood Defect Detector v2.0 ===")
message(STATUS "  TensorRT-YOLO: ${TRTYOLO_LIB}")
message(STATUS "  MVS SDK:       ${MVS_LIB}")
message(STATUS "  OpenCV:        ${OpenCV_VERSION}")
