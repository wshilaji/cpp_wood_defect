#pragma once

#include <NvInfer.h>
#include <cuda_runtime.h>

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/**
 * TensorRT 推理引擎
 *
 * 支持:
 *  - 从 .trt 文件加载预构建引擎
 *  - 从 .onnx 文件构建引擎（首次运行）
 *  - FP16 推理（Jetson Nano 必须）
 *  - 异步 CUDA stream 执行
 */
class TensorRTEngine {
public:
    TensorRTEngine();
    ~TensorRTEngine();

    /**
     * 从 ONNX 构建 TensorRT 引擎并序列化到磁盘
     * @param onnx_path    ONNX 模型路径
     * @param engine_path  输出 .trt 文件路径
     * @param fp16         是否启用 FP16
     * @param max_batch    最大 batch size
     * @param workspace    最大工作空间（字节）
     */
    bool buildFromONNX(const std::string& onnx_path,
                       const std::string& engine_path,
                       bool fp16 = true,
                       int max_batch = 1,
                       size_t workspace = 1UL << 28);

    /**
     * 加载已序列化的 TensorRT 引擎
     * @param engine_path .trt 文件路径
     */
    bool loadEngine(const std::string& engine_path);

    /**
     * 执行推理
     * @param input   输入数据指针 (float*, 大小 = batch * C * H * W)
     * @param outputs 输出数据数组 (vector<float*>)
     * @param batch   当前 batch 大小
     * @return 是否成功
     */
    bool infer(const float* input, std::vector<float*>& outputs, int batch = 1);

    /** 获取输入张量维度 (C, H, W) */
    std::vector<int> getInputShape() const { return _input_dims; }

    /** 获取各输出张量维度 */
    const std::vector<std::vector<int>>& getOutputShapes() const { return _output_dims; }

    /** 引擎是否就绪 */
    bool isReady() const { return _engine != nullptr && _context != nullptr; }

private:
    /** 分配 GPU 输入/输出 buffer */
    bool allocateBuffers();

    /** 释放所有资源 */
    void cleanup();

    // TRT 核心对象 — 用 shared_ptr 自定义 deleter 自动管理生命周期
    std::shared_ptr<nvinfer1::IRuntime>       _runtime;
    std::shared_ptr<nvinfer1::ICudaEngine>    _engine;
    std::shared_ptr<nvinfer1::IExecutionContext> _context;

    cudaStream_t    _stream = nullptr;

    // 输入/输出 buffer
    std::vector<void*>  _gpu_buffers;      // GPU 内存指针
    std::vector<void*>  _cpu_buffers;      // CPU 内存指针（固定内存，加速拷贝）
    std::vector<size_t> _buffer_sizes;     // 每个 buffer 的大小

    std::vector<int>                _input_dims;
    std::vector<std::vector<int>>   _output_dims;

    // Logger
    class TRTLogger : public nvinfer1::ILogger {
        void log(Severity severity, const char* msg) noexcept override;
    };
    TRTLogger _logger;
};
