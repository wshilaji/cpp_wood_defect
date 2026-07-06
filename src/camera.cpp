#include "camera.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <cstring>
#include <cmath>

// ============================================================
// 构造 / 析构
// ============================================================
HikvisionCamera::HikvisionCamera() {
    memset(&_device_list, 0, sizeof(_device_list));
}

HikvisionCamera::~HikvisionCamera() {
    stop();
}

// ============================================================
// 设备枚举
// ============================================================
bool HikvisionCamera::enumDevices() {
    memset(&_device_list, 0, sizeof(_device_list));

    // 枚举所有 GigE / USB / CameraLink 设备
    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &_device_list);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 枚举设备失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    if (_device_list.nDeviceNum == 0) {
        std::cerr << "[Camera] 未找到任何海康相机设备" << std::endl;
        return false;
    }

    std::cout << "[Camera] 找到 " << _device_list.nDeviceNum
              << " 个设备:" << std::endl;

    for (unsigned int i = 0; i < _device_list.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO& info = _device_list.pDeviceInfo[i];
        char serial[64] = {0};
        char model[64]  = {0};

        if (info.nTLayerType == MV_GIGE_DEVICE) {
            // GigE 相机
            snprintf(serial, sizeof(serial), "%s",
                     info.SpecialInfo.stGigEInfo.chSerialNumber);
            snprintf(model, sizeof(model), "%s",
                     info.SpecialInfo.stGigEInfo.chModelName);
            uint32_t ip = info.SpecialInfo.stGigEInfo.nCurrentIp;
            std::cout << "  [" << i << "] " << model
                      << " | SN: " << serial
                      << " | IP: " << ((ip >> 24) & 0xFF) << "."
                                    << ((ip >> 16) & 0xFF) << "."
                                    << ((ip >> 8) & 0xFF) << "."
                                    << (ip & 0xFF) << std::endl;
        } else if (info.nTLayerType == MV_USB_DEVICE) {
            // USB 相机
            snprintf(serial, sizeof(serial), "%s",
                     info.SpecialInfo.stUsb3VInfo.chSerialNumber);
            snprintf(model, sizeof(model), "%s",
                     info.SpecialInfo.stUsb3VInfo.chModelName);
            std::cout << "  [" << i << "] " << model
                      << " | SN: " << serial
                      << " | USB" << std::endl;
        }
    }
    return true;
}

// ============================================================
// 在设备列表中查找匹配设备
// ============================================================
int HikvisionCamera::findDevice(const std::string& serial,
                                 const std::string& ip,
                                 const std::string& user_id) {
    for (unsigned int i = 0; i < _device_list.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO& info = _device_list.pDeviceInfo[i];
        char dev_serial[64] = {0};
        char dev_user_id[64] = {0};
        char dev_ip_str[32] = {0};

        if (info.nTLayerType == MV_GIGE_DEVICE) {
            snprintf(dev_serial, sizeof(dev_serial), "%s",
                     info.SpecialInfo.stGigEInfo.chSerialNumber);
            snprintf(dev_user_id, sizeof(dev_user_id), "%s",
                     info.SpecialInfo.stGigEInfo.chUserDefinedName);
            uint32_t ip_val = info.SpecialInfo.stGigEInfo.nCurrentIp;
            snprintf(dev_ip_str, sizeof(dev_ip_str), "%u.%u.%u.%u",
                     (ip_val >> 24) & 0xFF, (ip_val >> 16) & 0xFF,
                     (ip_val >> 8) & 0xFF, ip_val & 0xFF);
        } else if (info.nTLayerType == MV_USB_DEVICE) {
            snprintf(dev_serial, sizeof(dev_serial), "%s",
                     info.SpecialInfo.stUsb3VInfo.chSerialNumber);
            snprintf(dev_user_id, sizeof(dev_user_id), "%s",
                     info.SpecialInfo.stUsb3VInfo.chUserDefinedName);
        }

        // 按序列号匹配（最高优先级）
        if (!serial.empty() && serial == dev_serial) {
            return static_cast<int>(i);
        }

        // 按 IP 匹配
        if (!ip.empty() && ip == dev_ip_str) {
            return static_cast<int>(i);
        }

        // 按用户自定义名称匹配
        if (!user_id.empty() && user_id == dev_user_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ============================================================
// 按条件连接相机
// ============================================================
bool HikvisionCamera::connectBySerial(const std::string& serial,
                                       const std::string& ip,
                                       const std::string& user_id) {
    if (!enumDevices()) return false;

    int idx = findDevice(serial, ip, user_id);
    if (idx < 0) {
        std::cerr << "[Camera] 未找到匹配的相机 (serial=" << serial
                  << ", ip=" << ip
                  << ", user_id=" << user_id << ")" << std::endl;
        return false;
    }
    return openDevice(idx);
}

bool HikvisionCamera::connectByIP(const std::string& ip) {
    if (!enumDevices()) return false;
    int idx = findDevice("", ip, "");
    if (idx < 0) {
        std::cerr << "[Camera] 未找到 IP=" << ip << " 的相机" << std::endl;
        return false;
    }
    return openDevice(idx);
}

bool HikvisionCamera::connectByUserID(const std::string& user_id) {
    if (!enumDevices()) return false;
    int idx = findDevice("", "", user_id);
    if (idx < 0) {
        std::cerr << "[Camera] 未找到 UserID=" << user_id << " 的相机" << std::endl;
        return false;
    }
    return openDevice(idx);
}

// ============================================================
// 打开设备
// ============================================================
bool HikvisionCamera::openDevice(int index) {
    _device_index = index;

    // 创建句柄
    int ret = MV_CC_CreateHandle(&_handle, _device_list.pDeviceInfo + index);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 创建相机句柄失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    // 打开设备
    ret = MV_CC_OpenDevice(_handle);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 打开相机失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        MV_CC_DestroyHandle(_handle);
        _handle = nullptr;
        return false;
    }

    std::cout << "[Camera] 相机已打开 (index=" << index << ")" << std::endl;

    // 获取当前像素格式
    MVCC_ENUMVALUE pixel_fmt;
    ret = MV_CC_GetEnumValue(_handle, "PixelFormat", &pixel_fmt);
    if (ret == MV_OK) {
        _pixel_format = pixel_fmt.nCurValue;
        std::cout << "[Camera] 当前像素格式: 0x"
                  << std::hex << _pixel_format << std::dec << std::endl;

        // 判断是否是 Bayer 格式
        unsigned int bayer_fmts[] = {
            PixelType_Gvsp_BayerGR8, PixelType_Gvsp_BayerRG8,
            PixelType_Gvsp_BayerGB8, PixelType_Gvsp_BayerBG8,
            PixelType_Gvsp_BayerGR10, PixelType_Gvsp_BayerRG10,
            PixelType_Gvsp_BayerGB10, PixelType_Gvsp_BayerBG10,
            PixelType_Gvsp_BayerGR12, PixelType_Gvsp_BayerRG12,
            PixelType_Gvsp_BayerGB12, PixelType_Gvsp_BayerBG12,
        };
        for (unsigned int fmt : bayer_fmts) {
            if (_pixel_format == fmt) {
                _is_bayer = true;
                break;
            }
        }
    }

    return true;
}

// ============================================================
// 关闭设备
// ============================================================
void HikvisionCamera::closeDevice() {
    if (_grabbing.load()) {
        MV_CC_StopGrabbing(_handle);
        _grabbing.store(false);
    }
    if (_handle) {
        MV_CC_CloseDevice(_handle);
        MV_CC_DestroyHandle(_handle);
        _handle = nullptr;
        std::cout << "[Camera] 相机已关闭" << std::endl;
    }
}

// ============================================================
// 设置像素格式
// ============================================================
bool HikvisionCamera::setPixelFormat(const std::string& format) {
    if (!_handle) return false;
    int ret = MV_CC_SetEnumValue(_handle, "PixelFormat",
                                  const_cast<char*>(format.c_str()));
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置像素格式 " << format
                  << " 失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 像素格式设为: " << format << std::endl;
    return true;
}

// ============================================================
// 设置触发模式
// ============================================================
bool HikvisionCamera::setTriggerMode(int mode) {
    if (!_handle) return false;

    switch (mode) {
        case 0: {
            // 连续采集
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_OFF);
            break;
        }
        case 1: {
            // 软触发
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_ON);
            MV_CC_SetEnumValue(_handle, "TriggerSource",
                                (void*)(uintptr_t)MV_TRIGGER_SOURCE_SOFTWARE);
            break;
        }
        case 2: {
            // 硬触发 (Line0 接收外部传感器信号)
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_ON);
            MV_CC_SetEnumValue(_handle, "TriggerSource",
                                (void*)(uintptr_t)MV_TRIGGER_SOURCE_LINE0);
            // 设置触发极性: 上升沿触发
            MV_CC_SetEnumValue(_handle, "TriggerActivation",
                                (void*)(uintptr_t)MV_TRIGGER_ACTIVATION_RISING_EDGE);
            break;
        }
        default:
            std::cerr << "[Camera] 无效的触发模式: " << mode << std::endl;
            return false;
    }

    std::cout << "[Camera] 触发模式: "
              << (mode == 0 ? "连续" : mode == 1 ? "软触发" : "硬触发(Line0)")
              << std::endl;
    return true;
}

// ============================================================
// 设置曝光时间
// ============================================================
bool HikvisionCamera::setExposureTime(float us) {
    if (!_handle || us < 0) return false;
    int ret = MV_CC_SetFloatValue(_handle, "ExposureTime", us);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置曝光失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 曝光时间: " << us << " us" << std::endl;
    return true;
}

// ============================================================
// 设置增益
// ============================================================
bool HikvisionCamera::setGain(float db) {
    if (!_handle || db < 0) return false;
    int ret = MV_CC_SetFloatValue(_handle, "Gain", db);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置增益失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 模拟增益: " << db << " dB" << std::endl;
    return true;
}

// ============================================================
// 获取曝光
// ============================================================
float HikvisionCamera::getExposureTime() {
    if (!_handle) return -1.0f;
    MVCC_FLOATVALUE val;
    if (MV_CC_GetFloatValue(_handle, "ExposureTime", &val) == MV_OK) {
        return val.fCurValue;
    }
    return -1.0f;
}

// ============================================================
// 获取增益
// ============================================================
float HikvisionCamera::getGain() {
    if (!_handle) return -1.0f;
    MVCC_FLOATVALUE val;
    if (MV_CC_GetFloatValue(_handle, "Gain", &val) == MV_OK) {
        return val.fCurValue;
    }
    return -1.0f;
}

// ============================================================
// 软触发：发送一次拍照指令
// ============================================================
bool HikvisionCamera::softwareTrigger() {
    if (!_handle) return false;
    int ret = MV_CC_SetCommandValue(_handle, "TriggerSoftware");
    return (ret == MV_OK);
}

// ============================================================
// 注册图像回调
// ============================================================
bool HikvisionCamera::registerCallback() {
    if (!_handle) return false;
    int ret = MV_CC_RegisterImageCallBackEx(_handle, onImageCallback, this);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 注册图像回调失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 图像回调已注册" << std::endl;
    return true;
}

// ============================================================
// 启动采集
// ============================================================
bool HikvisionCamera::start(int width, int height,
                             float exposure_us,
                             float gain_db,
                             int trigger_mode) {
    if (!_handle) {
        std::cerr << "[Camera] 相机未打开，无法启动" << std::endl;
        return false;
    }

    // ---- 1. 触发模式 ----
    setTriggerMode(trigger_mode);

    // ---- 2. 曝光时间 ----
    if (exposure_us >= 0) setExposureTime(exposure_us);

    // ---- 3. 增益 ----
    if (gain_db >= 0) setGain(gain_db);

    // ---- 4. 图像尺寸（如果需要修改） ----
    if (width > 0 && height > 0) {
        MV_CC_SetIntValue(_handle, "Width",  width);
        MV_CC_SetIntValue(_handle, "Height", height);
    }

    // ---- 5. 注册回调 ----
    if (!registerCallback()) return false;

    // ---- 6. 开始取流 ----
    int ret = MV_CC_StartGrabbing(_handle);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 开始取流失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    _grabbing.store(true);
    _running.store(true);

    std::cout << "[Camera] 取流已启动 "
              << "(宽=" << width << ", 高=" << height
              << ", 曝光=" << exposure_us << "us"
              << ", 增益=" << gain_db << "dB)"
              << std::endl;

    return true;
}

// ============================================================
// 停止采集
// ============================================================
void HikvisionCamera::stop() {
    _running.store(false);
    closeDevice();

    // 清空缓冲
    std::lock_guard<std::mutex> lock(_mutex);
    _buffer.clear();

    std::cout << "[Camera] 已停止" << std::endl;
}

// ============================================================
// 读取最新帧（非阻塞）
// ============================================================
cv::Mat HikvisionCamera::read() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_buffer.empty()) {
        return cv::Mat();
    }
    return _buffer.back().clone();
}

// ============================================================
// SDK 图像回调入口 (static → handleImage)
// ============================================================
void __stdcall HikvisionCamera::onImageCallback(unsigned char* pData,
                                                  MV_FRAME_OUT_INFO_EX* pFrameInfo,
                                                  void* pUser) {
    if (pUser) {
        auto* self = static_cast<HikvisionCamera*>(pUser);
        self->handleImage(pData, pFrameInfo);
    }
}

// ============================================================
// 回调中处理图像: Bayer→BGR → 写入缓冲队列
// ============================================================
void HikvisionCamera::handleImage(unsigned char* pData,
                                   MV_FRAME_OUT_INFO_EX* pFrameInfo) {
    if (!pData || !pFrameInfo) return;

    int w = pFrameInfo->nWidth;
    int h = pFrameInfo->nHeight;

    cv::Mat bgr;

    if (_is_bayer) {
        // Bayer 格式 → BGR
        cv::Mat raw(h, w, CV_8UC1, pData);
        bgr = bayerToBGR(raw);
    } else if (_pixel_format == PixelType_Gvsp_RGB8_Packed) {
        // RGB → BGR
        cv::Mat rgb(h, w, CV_8UC3, pData);
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    } else if (_pixel_format == PixelType_Gvsp_BGR8_Packed) {
        // 直接 BGR，仅拷贝
        bgr = cv::Mat(h, w, CV_8UC3, pData).clone();
    } else if (_pixel_format == PixelType_Gvsp_Mono8) {
        // 灰度 → BGR
        cv::Mat gray(h, w, CV_8UC1, pData);
        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    } else {
        // 未知格式，尝试当作 8-bit 单通道处理
        cv::Mat unknown(h, w, CV_8UC1, pData);
        cv::cvtColor(unknown, bgr, cv::COLOR_GRAY2BGR);
    }

    if (!bgr.empty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        _buffer.push_back(bgr);
        while (_buffer.size() > _MAX_BUFFER) {
            _buffer.pop_front();
        }
    }
}

// ============================================================
// BayerRG8 → BGR
// 使用 OpenCV demosaicing
// ============================================================
cv::Mat HikvisionCamera::bayerToBGR(const cv::Mat& bayer) {
    cv::Mat bgr;
    // bayer 格式对应的 OpenCV 转换码
    static const std::map<unsigned int, int> BAYER_CODES = {
        {PixelType_Gvsp_BayerRG8,  cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR8,  cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB8,  cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG8,  cv::COLOR_BayerBG2BGR},
        {PixelType_Gvsp_BayerRG10, cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR10, cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB10, cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG10, cv::COLOR_BayerBG2BGR},
        {PixelType_Gvsp_BayerRG12, cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR12, cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB12, cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG12, cv::COLOR_BayerBG2BGR},
    };

    auto it = BAYER_CODES.find(_pixel_format);
    if (it != BAYER_CODES.end()) {
        cv::cvtColor(bayer, bgr, it->second);
    } else {
        // 默认 BayerRG
        cv::cvtColor(bayer, bgr, cv::COLOR_BayerRG2BGR);
    }

    return bgr;
}
