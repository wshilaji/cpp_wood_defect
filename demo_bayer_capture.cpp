/**
 * Bayer 采集 + debayer 测速 Demo（修正版）
 *
 * 目的:
 *   1. 测「Bayer 原始格式 + Nano 端 debayer」的拍照延迟，对比 RGB8（相机硬件 ISP）基线
 *   2. 同一次运行内拍同一场景，方便肉眼对比图像质量
 *
 * 修正点（上一版踩过的坑）:
 *   1. 切换像素格式前先【枚举】相机支持的格式，用【整型值】设置而不是字符串，
 *      避免 "BayerRG8" 这个字符串与相机实际暴露的名字不一致导致 Set 静默失败。
 *   2. 每次 Set 都检查返回值 + 读回校验；切换失败会明确报错，不再静默。
 *   3. HB 无损压缩独立探测：先枚举 ImageCompressionMode 节点，不支持就明确提示，
 *      不影响普通 Bayer 测速。
 *   4. RGB8 与 Bayer 都做预热 + 多次取平均，A/B 对比更公平。
 *
 * 用法:
 *   ./demo_bayer_capture [相机IP] [曝光us] [是否开HB压缩 0/1]
 *   例: ./demo_bayer_capture 192.168.2.10 7000 1
 *
 * 输出:
 *   rgb8_debug.png    — RGB8 基线（硬件 ISP）
 *   bayer_debug.png   — Bayer + Nano debayer
 */
#include <opencv2/opencv.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "MvCameraControl.h"

// ============================================================
// 像素格式 值 → 可读名称
// ============================================================
static const char* pfName(unsigned int v) {
    switch (v) {
        case PixelType_Gvsp_Mono8:       return "Mono8";
        case PixelType_Gvsp_BayerGR8:    return "BayerGR8";
        case PixelType_Gvsp_BayerRG8:    return "BayerRG8";
        case PixelType_Gvsp_BayerGB8:    return "BayerGB8";
        case PixelType_Gvsp_BayerBG8:    return "BayerBG8";
        case PixelType_Gvsp_RGB8_Packed: return "RGB8Packed";
        case PixelType_Gvsp_BGR8_Packed: return "BGR8Packed";
        default: return "?";
    }
}

static bool isBayer8(unsigned int v) {
    return v == PixelType_Gvsp_BayerGR8 || v == PixelType_Gvsp_BayerRG8 ||
           v == PixelType_Gvsp_BayerGB8 || v == PixelType_Gvsp_BayerBG8;
}

// ============================================================
// 枚举相机支持的像素格式；返回第一个 Bayer8 值（没有则返回 0）
// ============================================================
static unsigned int enumeratePixelFormats(void* handle) {
    MVCC_ENUMVALUE pf;
    memset(&pf, 0, sizeof(pf));
    if (MV_CC_GetEnumValue(handle, "PixelFormat", &pf) != MV_OK) {
        std::cerr << "[Camera] 枚举 PixelFormat 失败" << std::endl;
        return 0;
    }
    std::cout << "[Camera] PixelFormat 支持 " << pf.nSupportedNum << " 种:" << std::endl;
    unsigned int bayer = 0;
    for (unsigned int i = 0; i < pf.nSupportedNum; ++i) {
        unsigned int v = pf.nSupportValue[i];
        std::cout << "    [" << i << "] 0x" << std::hex << v << std::dec
                  << " = " << pfName(v);
        if (isBayer8(v)) { std::cout << "   ← Bayer8 可用"; if (!bayer) bayer = v; }
        std::cout << std::endl;
    }
    return bayer;
}

// ============================================================
// 回调交接状态
// ============================================================
struct CaptureState {
    std::mutex                     mtx;
    std::condition_variable        cv;
    cv::Mat                        raw;
    unsigned int                   fmt = 0;
    uint64_t                       count = 0;
    std::chrono::steady_clock::time_point arrival;
};

static void __stdcall onFrame(unsigned char* pData,
                              MV_FRAME_OUT_INFO_EX* pInfo,
                              void* pUser) {
    if (!pData || !pInfo || !pUser) return;
    CaptureState* st = static_cast<CaptureState*>(pUser);

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
// 像素格式 → OpenCV 边缘感知 demosaic 转换码
// ============================================================
static int bayerCodeEA(unsigned int fmt) {
    switch (fmt) {
        case PixelType_Gvsp_BayerRG8: return cv::COLOR_BayerRG2BGR_EA;
        case PixelType_Gvsp_BayerGR8: return cv::COLOR_BayerGR2BGR_EA;
        case PixelType_Gvsp_BayerGB8: return cv::COLOR_BayerGB2BGR_EA;
        case PixelType_Gvsp_BayerBG8: return cv::COLOR_BayerBG2BGR_EA;
        default:                      return cv::COLOR_BayerRG2BGR_EA;
    }
}

// ============================================================
// Nano 端轻 ISP：边缘感知 demosaic + 灰世界白平衡 + gamma
// 相机 ISP 在 RGB8 下做 demosaic/WB/CCM/gamma；裸 cvtColor 只有 demosaic，
// 缺 WB/gamma 所以偏绿、偏暗、边缘有伪彩。这里补上（约 20~30ms）。
// ============================================================
static cv::Mat debayerIsp(const cv::Mat& raw, unsigned int fmt) {
    cv::Mat bgr;

    // 1. 边缘感知 demosaic（比双线性清晰、伪彩少）
    cv::cvtColor(raw, bgr, bayerCodeEA(fmt));

    // 2. 灰世界白平衡：拉平三通道均值，去掉 Bayer 无 WB 导致的偏色
    cv::Scalar m = cv::mean(bgr);
    double avg = (m[0] + m[1] + m[2]) / 3.0;
    if (avg > 1.0) {
        double gB = avg / std::max(m[0], 1e-6);
        double gG = avg / std::max(m[1], 1e-6);
        double gR = avg / std::max(m[2], 1e-6);
        cv::multiply(bgr, cv::Scalar(gB, gG, gR), bgr);
    }

    // 3. gamma 0.7（相机 ISP 同款，线性图偏暗，这里拉开；对不上就改这个值或换成 1/0.7）
    static cv::Mat lut;
    if (lut.empty()) {
        lut.create(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * std::pow(i / 255.0, 0.7));
    }
    cv::LUT(bgr, lut, bgr);

    return bgr;
}

static double ms(const std::chrono::steady_clock::time_point& a,
                 const std::chrono::steady_clock::time_point& b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ============================================================
// 软触发一次 + 等帧返回
// ============================================================
struct Frame {
    cv::Mat      raw;
    unsigned int fmt = 0;
    double       capture_ms = 0;
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

// 原始数据 → BGR（RGB8 转色序，Bayer 走轻 ISP）
static cv::Mat toBGR(const cv::Mat& raw, unsigned int fmt) {
    cv::Mat bgr;
    if (fmt == PixelType_Gvsp_RGB8_Packed) {
        cv::cvtColor(raw, bgr, cv::COLOR_RGB2BGR);
    } else {
        bgr = debayerIsp(raw, fmt);
    }
    return bgr;
}

// ============================================================
// 探测 HB 无损压缩是否可用（可用则开启）
// ============================================================
static bool tryEnableHB(void* handle) {
    int ret = MV_CC_SetEnumValueByString(handle, "ImageCompressionMode", "HB");
    if (ret == MV_OK) {
        std::cout << "[Camera] HB 无损压缩已开启" << std::endl;
        // 高带宽突发模式，配合 HB 提升 GigE 吞吐
        MV_CC_SetEnumValueByString(handle, "HighBandwidthMode", "Burst");
        return true;
    }
    std::cout << "[Camera] HB 无损压缩不可用 (ret=0x" << std::hex << ret << std::dec
              << " = " << (ret == MV_E_PARAMETER ? "参数错误(节点不存在或值不支持)" :
                           ret == MV_E_SUPPORT ? "功能不支持" : "其他错误")
              << ")，按普通 Bayer 继续" << std::endl;
    return false;
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char** argv) {
    std::string ip       = (argc > 1) ? argv[1] : "192.168.2.10";
    float       exposure = (argc > 2) ? std::stof(argv[2]) : 7000.0f;
    bool        use_hb   = (argc > 3) ? (std::stoi(argv[3]) != 0) : true;

    std::cout << "=== Bayer 采集测速 & 对比 Demo（修正版）===\n"
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

    // 关键：先枚举相机到底支持哪些像素格式
    unsigned int bayer_fmt = enumeratePixelFormats(handle);
    if (bayer_fmt == 0) {
        std::cerr << "\n[Camera] ⚠ 该相机不暴露任何 Bayer8 像素格式，"
                     "Bayer+debayer 方案在此机型不可行，请改用其他优化手段。\n" << std::endl;
    }

    CaptureState st;
    MV_CC_RegisterImageCallBackEx(handle, onFrame, &st);

    const int N = 20;

    // ============================================================
    // ---- A. RGB8 基线（相机硬件 ISP） ----
    // ============================================================
    double rgb8_capture = 0, rgb8_convert = 0;
    {
        ret = MV_CC_SetEnumValue(handle, "PixelFormat", PixelType_Gvsp_RGB8_Packed);
        MV_CC_StartGrabbing(handle);

        // 预热一帧
        { Frame w; grabOne(handle, st, 800, w); }

        bool saved = false;
        for (int i = 1; i <= N; ++i) {
            Frame f;
            if (!grabOne(handle, st, 800, f)) { std::cerr << "[RGB8] 取帧超时" << std::endl; break; }

            auto d0 = std::chrono::steady_clock::now();
            cv::Mat bgr = toBGR(f.raw, f.fmt);
            auto d1 = std::chrono::steady_clock::now();

            rgb8_capture += f.capture_ms;
            rgb8_convert += ms(d0, d1);

            if (!saved && !bgr.empty()) {
                cv::imwrite("rgb8_debug.png", bgr);
                std::cout << "[Debug] 已存 RGB8 基线 → rgb8_debug.png" << std::endl;
                saved = true;
            }
        }
        MV_CC_StopGrabbing(handle);
    }

    // ============================================================
    // ---- B. Bayer + Nano debayer ----
    // ============================================================
    double bayer_capture = 0, bayer_debayer = 0;
    bool   bayer_ok = false;

    if (bayer_fmt != 0) {
        // 用【值】设置，并读回校验
        ret = MV_CC_SetEnumValue(handle, "PixelFormat", bayer_fmt);
        MVCC_ENUMVALUE pf;
        memset(&pf, 0, sizeof(pf));
        MV_CC_GetEnumValue(handle, "PixelFormat", &pf);
        std::cout << "\n[Camera] 设置 " << pfName(bayer_fmt)
                  << " ret=0x" << std::hex << ret
                  << "  读回=0x" << pf.nCurValue << "(" << pfName(pf.nCurValue) << ")"
                  << std::dec << std::endl;

        if (ret != MV_OK || pf.nCurValue != bayer_fmt) {
            std::cerr << "[Camera] ⚠ Bayer 切换失败，本次仍按 RGB8 跑，"
                         "结果不能代表 Bayer 方案！" << std::endl;
        } else {
            bayer_ok = true;

            if (use_hb) tryEnableHB(handle);

            MV_CC_StartGrabbing(handle);

            // 预热一帧
            { Frame w; grabOne(handle, st, 800, w); }
            std::cout << "\n[Warmup] 预热完成，开始测 " << N << " 次...\n" << std::endl;

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

                bayer_capture += f.capture_ms;
                bayer_debayer += debayer_ms;

                if (!saved && !bgr.empty()) {
                    cv::imwrite("bayer_debug.png", bgr);
                    std::cout << "[Debug] 已存第一张 Bayer 结果 → bayer_debug.png" << std::endl;
                    saved = true;
                }

                std::cout << "[" << std::setw(2) << i << "] 拍照(触发→帧): "
                          << std::fixed << std::setprecision(1) << f.capture_ms
                          << "ms | ISP: " << debayer_ms
                          << "ms | 总计: " << (f.capture_ms + debayer_ms) << "ms" << std::endl;
            }
            MV_CC_StopGrabbing(handle);
        }
    }

    // ============================================================
    // ---- 汇总 ----
    // ============================================================
    std::cout << "\n==== 结果汇总 ====" << std::endl;
    std::cout << "RGB8(硬件ISP):  拍照=" << std::fixed << std::setprecision(1)
              << rgb8_capture / N << "ms  转色序=" << rgb8_convert / N
              << "ms  总计=" << (rgb8_capture + rgb8_convert) / N << "ms" << std::endl;

    if (bayer_ok) {
        std::cout << "Bayer(Nano):    拍照=" << bayer_capture / N
                  << "ms  ISP=" << bayer_debayer / N
                  << "ms  总计=" << (bayer_capture + bayer_debayer) / N << "ms" << std::endl;
    } else {
        std::cout << "Bayer: 不可用（详见上方报错）" << std::endl;
    }

    std::cout << "\n对比图: rgb8_debug.png (基线)  vs  bayer_debug.png (Bayer)" << std::endl;

    // ---- 清理 ----
    MV_CC_StopGrabbing(handle);
    MV_CC_SetEnumValue(handle, "TriggerMode", MV_TRIGGER_MODE_OFF);
    MV_CC_CloseDevice(handle);
    MV_CC_DestroyHandle(handle);
    std::cout << "\n[Camera] 已关闭" << std::endl;
    return 0;
}
