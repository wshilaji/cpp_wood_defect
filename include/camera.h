#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>

// 海康 MVS SDK 头文件
#include "MvCameraControl.h"

/**
 * 海康工业相机采集模块
 *
 * 使用海康 MVS SDK 直连相机，支持:
 *  - 连续采集模式（调试用）
 *  - 软触发模式（程序发指令拍照）
 *  - 硬触发模式（流水线传感器信号 → Line0 → 相机拍照）
 *
 * 图像通过 SDK 回调函数接收 → Bayer→BGR 转换 → 存入缓冲队列
 * 主线程非阻塞 read() 取最新帧
 */
class HikvisionCamera {
public:
    HikvisionCamera();
    ~HikvisionCamera();

    /**
     * 根据序列号查找并连接相机
     * @param serial 相机序列号，空字符串表示不使用
     * @param ip     相机 IP 地址
     * @param user_id 用户自定义名称，空字符串表示不使用
     */
    bool connectBySerial(const std::string& serial,
                         const std::string& ip = "",
                         const std::string& user_id = "");

    /**
     * 根据 IP 查找并连接相机
     */
    bool connectByIP(const std::string& ip);

    /**
     * 根据用户自定义名称查找并连接相机
     */
    bool connectByUserID(const std::string& user_id);

    /**
     * 配置采集参数并开始取流
     * @param width          图像宽度 (0 = 不修改)
     * @param height         图像高度 (0 = 不修改)
     * @param exposure_us    曝光时间（微秒），<0 = 不修改
     * @param gain_db        模拟增益（dB），<0 = 不修改
     * @param trigger_mode   0=连续, 1=软触发, 2=硬触发
     * @return 成功返回 true
     */
    bool start(int width, int height,
               float exposure_us = -1.0f,
               float gain_db = -1.0f,
               int trigger_mode = 2);

    /** 停止采集并关闭相机 */
    void stop();

    /** 非阻塞读取最新一帧，无新帧时返回空 Mat */
    cv::Mat read();

    /** 是否正在运行 */
    bool isRunning() const { return _running.load(); }

    /** 软触发: 发送一次拍照指令（仅 trigger_mode=1 时有效） */
    bool softwareTrigger();

    // ---- 参数读写 ----
    bool setExposureTime(float us);
    bool setGain(float db);
    bool setTriggerMode(int mode);
    float getExposureTime();
    float getGain();

private:
    // ---- 内部辅助 ----
    /** 枚举所有在线设备 */
    bool enumDevices();

    /** 在设备列表里按条件匹配设备 */
    int  findDevice(const std::string& serial,
                    const std::string& ip,
                    const std::string& user_id);

    /** 创建句柄并打开设备 */
    bool openDevice(int index);

    /** 设置像素格式 */
    bool setPixelFormat(const std::string& format);

    /** 关闭设备并销毁句柄 */
    void closeDevice();

    /** 注册图像回调 */
    bool registerCallback();

    // ---- 静态回调（SDK C 接口） ----
    static void __stdcall onImageCallback(unsigned char* pData,
                                          MV_FRAME_OUT_INFO_EX* pFrameInfo,
                                          void* pUser);

    /** 回调中执行的图像处理 */
    void handleImage(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo);

    // ---- 像素格式转换 ----
    /** BayerRG8 → BGR (OpenCV 实现) */
    cv::Mat bayerToBGR(const cv::Mat& bayer);

    // ---- 成员变量 ----
    void*                       _handle = nullptr;      // MV_CC_CreateHandle 返回
    std::atomic<bool>           _running{false};
    std::atomic<bool>           _grabbing{false};

    MV_CC_DEVICE_INFO_LIST      _device_list;
    int                         _device_index = -1;

    // 当前像素格式
    unsigned int                _pixel_format = 0;     // MvGvspPixelType 枚举值
    bool                        _is_bayer = false;

    // 帧缓冲队列
    std::deque<cv::Mat>         _buffer;
    static constexpr size_t     _MAX_BUFFER = 3;
    std::mutex                  _mutex;
    std::condition_variable     _cv;
};
