/**
 * Bayer 采集 + debayer 纯拍照测速 & 对比 Demo
 *
 * 目的:
 *   1. 测「Bayer 原始格式 + 相机无损压缩(HB) + Nano 端 debayer」的拍照延迟
 *   2. 同一次运行内，先拍一张 RGB8（当前产线格式）基线，再切 Bayer 拍，
 *      两张图是同一场景，方便肉眼对比图像质量。
 *
 * 独立文件，不动主程序任何代码。
 *
 * 用法:
 *   ./demo_bayer_capture [相机IP] [曝光us] [是否开HB压缩 0/1]
 *   例: ./demo_bayer_capture 192.168.2.10 7000 1
 *
 * 输出:
 *   rgb8_debug.png    — RGB8 基线（硬件 ISP，当前产线格式）
 *   bayer_debug.png   — Bayer + debayer 结果
 *   终端日志          — 两种格式的拍照延迟 + debayer 耗时
 */
#include <opencv2/opencv.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "MvCameraControl.h"

// ============================================================
// 回调交接状态
// ============================================================
struct CaptureState {
    std::mutex                     mtx;
    std::condition_variable        cv;
    cv::Mat                        raw;                   // 原始数据（Bayer=1通道 / RGB8=3通道）
    unsigned int                   fmt = 0;               // 该帧像素格式
    uint64_t                       count = 0;             // 已到帧总数
    std::chrono::steady_clock::time_point arrival;        // 帧到达时刻
};

static void __stdcall onFrame(unsigned char* pData,
                              MV_FRAME_OUT_INFO_EX* pInfo,
                              void* pUser) {
    if (!pData || !pInfo || !pUser) return;
    CaptureState* st = static_cast<CaptureState*>(pUser);

    // SDK 已透明解压（HB 模式），拿到的是解压后的原始数据
    cv::Mat m;
    if (pInfo->enPixelType == PixelType_Gvsp_RGB8_Packed) {
        m = cv::Mat(pInfo->nHeight, pInfo->nWidth, CV_8UC3, pData).clone();
    } else {
        m = cv::Mat(pInfo->nHeight, pInfo->nWidth, CV_8UC1, pData).clone();
    }

    std::lock_guard<std::mutex> lock(st->mtx);
    st->raw = m;
    st->fmt = pInfo->enPixelType;
    st->count++;
    st->arrival = std::chrono::steady_clock::now();
    st->cv.notify_all();
}

// ============================================================
// 像素格式 → OpenCV debayer 转换码
// ============================================================
static int bayerCodeFor(unsigned int fmt) {
    switch (fmt) {
        case PixelType_Gvsp_BayerRG8: return cv::COLOR_BayerRG2BGR;
        case PixelType_Gvsp_BayerGR8: return cv::COLOR_BayerGR2BGR;
        case PixelType_Gvsp_BayerGB8: return cv::COLOR_BayerGB2BGR;
        case PixelType_Gvsp_BayerBG8: return cv::COLOR_BayerBG2BGR;
        default:                      return cv::COLOR_BayerRG2BGR;
    }
}

static double ms(const std::chrono::steady_clock::time_point& a,
                 const std::chrono::steady_clock::time_point& b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ============================================================
// 软触发一次 + 等帧返回；raw=原始数据, capture_ms=触发→帧到耗时
// ============================================================
struct Frame {
    cv::Mat    raw;
    unsigned int fmt = 0;
    double     capture_ms = 0;
};

static bool grabOne(void* handle, CaptureState& st, int timeout_ms, Frame& out) {
    uint64_t before = st.count;
    auto trig = std::chrono::steady_clock::now();
    MV_CC_SetCommandValue(handle, "TriggerSoftware");

    std::unique_lock<std::mutex> lk(st.mtx);
    if (!st.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&]{ return st.count > before; })) {
        return false;
    }
    out.raw        = st.raw.clone();
    out.fmt        = st.fmt;
    out.capture_ms = ms(trig, st.arrival);
    return true;
}

// 原始数据 → BGR（RGB8 转色序，Bayer 做 debayer）
static cv::Mat toBGR(const cv::Mat& raw, unsigned int fmt) {
    cv::Mat bgr;
    if (fmt == PixelType_Gvsp_RGB8_Packed) {
        cv::cvtColor(raw, bgr, cv::COLOR_RGB2BGR);
    } else {
        cv::cvtColor(raw, bgr, bayerCodeFor(fmt));
    }
    return bgr;
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char** argv) {
    std::string ip        = (argc > 1) ? argv[1] : "192.168.2.10";
    float       exposure  = (argc > 2) ? std::stof(argv[2]) : 7000.0f;
    bool        use_hb    = (argc > 3) ? (std::stoi(argv[3]) != 0) : true;

    std::cout << "=== Bayer 采集测速 & 对比 Demo ===\n"
              << "IP=" << ip << " 曝光=" << exposure << "us  HB压缩="
              << (use_hb ? "开" : "关") << "\n" << std::endl;

    // ---- 1. 枚举 + 打开 ----
    MV_CC_DEVICE_INFO_LIST dev_list;
    memset(&dev_list, 0, sizeof(dev_list));
    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &dev_list);
    if (ret != MV_OK || dev_list.nDeviceNum == 0) {
        std::cerr << "枚举设备失败或无设备, 0x" << std::hex << ret << std::dec << std::endl;
        return -1;
    }
    int idx = -1;
    for (unsigned int i = 0; i < dev_list.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO* info = dev_list.pDeviceInfo[i];
        if (info->nTLayerType == MV_GIGE_DEVICE) {
            uint32_t ipv = info->SpecialInfo.stGigEInfo.nCurrentIp;
            char buf[32];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (ipv >> 24) & 0xFF, (ipv >> 16) & 0xFF,
                     (ipv >> 8) & 0xFF, ipv & 0xFF);
            if (ip == buf) { idx = (int)i; break; }
        }
    }
    if (idx < 0) { std::cerr << "未找到 IP=" << ip << " 的相机" << std::endl; return -2; }

    void* handle = nullptr;
    MV_CC_CreateHandle(&handle, dev_list.pDeviceInfo[idx]);
    MV_CC_OpenDevice(handle);
    std::cout << "[Camera] 已打开" << std::endl;

    // 曝光/增益/软触发（两种格式通用）
    MV_CC_SetFloatValue(handle, "ExposureTime", exposure);
    MV_CC_SetFloatValue(handle, "Gain", 0.0f);
    MV_CC_SetEnumValue(handle, "TriggerMode", MV_TRIGGER_MODE_ON);
    MV_CC_SetEnumValue(handle, "TriggerSource", MV_TRIGGER_SOURCE_SOFTWARE);

    CaptureState st;
    MV_CC_RegisterImageCallBackEx(handle, onFrame, &st);

    // ============================================================
    // ---- A. RGB8 基线（当前产线格式） ----
    // ============================================================
    {
        MV_CC_SetEnumValueByString(handle, "PixelFormat", "RGB8Packed");
        MV_CC_StartGrabbing(handle);

        Frame f;
        if (grabOne(handle, st, 800, f)) {
            cv::Mat bgr = toBGR(f.raw, f.fmt);
            cv::imwrite("rgb8_debug.png", bgr);
            std::cout << "[RGB8基线] 拍照(触发→帧): " << std::fixed << std::setprecision(1)
                      << f.capture_ms << "ms → 已存 rgb8_debug.png" << std::endl;
        } else {
            std::cerr << "[RGB8基线] 取帧超时" << std::endl;
        }
        MV_CC_StopGrabbing(handle);
    }

    // ============================================================
    // ---- B. Bayer 格式 + 可选 HB 压缩 ----
    // ============================================================
    MV_CC_SetEnumValueByString(handle, "PixelFormat", "BayerRG8");
    MVCC_ENUMVALUE pf;
    MV_CC_GetEnumValue(handle, "PixelFormat", &pf);
    std::cout << "[Camera] 当前像素格式: 0x" << std::hex << pf.nCurValue
              << std::dec << std::endl;

    if (use_hb) {
        ret = MV_CC_SetEnumValueByString(handle, "ImageCompressionMode", "HB");
        if (ret == MV_OK) {
            std::cout << "[Camera] HB 无损压缩已开启" << std::endl;
            MV_CC_SetEnumValueByString(handle, "HighBandwidthMode", "Burst");
        } else {
            std::cout << "[Camera] HB 压缩不可用(0x" << std::hex << ret << std::dec
                      << ")，按普通 Bayer 继续" << std::endl;
        }
    }

    MV_CC_StartGrabbing(handle);

    // 预热一帧
    { Frame w; grabOne(handle, st, 800, w); }
    std::cout << "\n[Warmup] 预热完成，开始测 20 次...\n" << std::endl;

    const int N = 20;
    double sum_capture = 0, sum_debayer = 0, sum_total = 0;
    bool saved = false;

    for (int i = 1; i <= N; ++i) {
        Frame f;
        if (!grabOne(handle, st, 800, f)) {
            std::cerr << "[" << i << "] 触发后超时未取到帧" << std::endl;
            continue;
        }

        auto d0 = std::chrono::steady_clock::now();
        cv::Mat bgr = toBGR(f.raw, f.fmt);
        auto d1 = std::chrono::steady_clock::now();
        double debayer_ms = ms(d0, d1);

        double total_ms = f.capture_ms + debayer_ms;
        sum_capture += f.capture_ms; sum_debayer += debayer_ms; sum_total += total_ms;

        if (!saved && !bgr.empty()) {
            cv::imwrite("bayer_debug.png", bgr);
            std::cout << "[Debug] 已存第一张 Bayer 结果 → bayer_debug.png" << std::endl;
            saved = true;
        }

        std::cout << "[" << std::setw(2) << i << "] 拍照(触发→帧): "
                  << std::fixed << std::setprecision(1) << f.capture_ms
                  << "ms | debayer: " << debayer_ms
                  << "ms | 总计: " << total_ms << "ms" << std::endl;
    }

    std::cout << "\n==== Bayer 平均 ====" << std::endl;
    std::cout << "拍照(触发→帧): " << std::fixed << std::setprecision(1)
              << sum_capture / N << "ms" << std::endl;
    std::cout << "debayer:       " << sum_debayer / N << "ms" << std::endl;
    std::cout << "总计:          " << sum_total / N << "ms" << std::endl;
    std::cout << "\n对比图: rgb8_debug.png (基线)  vs  bayer_debug.png (Bayer)" << std::endl;

    // ---- 清理 ----
    MV_CC_StopGrabbing(handle);
    MV_CC_SetEnumValue(handle, "TriggerMode", MV_TRIGGER_MODE_OFF);
    MV_CC_CloseDevice(handle);
    MV_CC_DestroyHandle(handle);
    std::cout << "\n[Camera] 已关闭" << std::endl;
    return 0;
}
