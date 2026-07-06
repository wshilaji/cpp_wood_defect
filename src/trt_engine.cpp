#include "trt_engine.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cuda_runtime.h>

// ============================================================
// Logger 实现
// ============================================================
void TensorRTEngine::TRTLogger::log(Severity severity, const char* msg) noexcept {
    if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR) {
        std::cerr << "[TensorRT] ERROR: " << msg << std::endl;
    } else if (severity == Severity::kWARNING) {
        std::cerr << "[TensorRT] WARNING: " << msg << std::endl;
    } else if (severity == Severity::kINFO) {
        std::cout << "[TensorRT] " << msg << std::endl;
    }
    // VERBOSE 忽略
}

// ============================================================
// 构造 / 析构
// ============================================================
TensorRTEngine::TensorRTEngine() {
    cudaStreamCreate(&_stream);
}

TensorRTEngine::~TensorRTEngine() {
    cleanup();
}

// ============================================================
// 从 ONNX 构建引擎
// ============================================================
bool TensorRTEngine::buildFromONNX(const std::string& onnx_path,
                                    const std::string& engine_path,
                                    bool fp16,
                                    int max_batch,
                                    size_t workspace)
{
    std::cout << "[TensorRT] 正在从 ONNX 构建引擎..." << std::endl;
    std::cout << "  ONNX:    " << onnx_path << std::endl;
    std::cout << "  Output:  " << engine_path << std::endl;
    std::cout << "  FP16:    " << (fp16 ? "ON" : "OFF") << std::endl;

    // ---- 创建 Builder ----
    auto builder = std::shared_ptr<nvinfer1::IBuilder>(
        nvinfer1::createInferBuilder(_logger),
        [](nvinfer1::IBuilder* p) { if (p) p->destroy(); });

    if (!builder) {
        std::cerr << "[TensorRT] ERROR: 创建 Builder 失败" << std::endl;
        return false;
    }

    // 显式 batch 维度
    const auto explicitBatch =
        1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);

    auto network = std::shared_ptr<nvinfer1::INetworkDefinition>(
        builder->createNetworkV2(explicitBatch),
        [](nvinfer1::INetworkDefinition* p) { if (p) p->destroy(); });

    auto parser = std::shared_ptr<nvonnxparser::IParser>(
        nvonnxparser::createParser(*network, _logger),
        [](nvonnxparser::IParser* p) { if (p) p->destroy(); });

    // ---- 解析 ONNX ----
    std::ifstream onnx_file(onnx_path, std::ios::binary);
    if (!onnx_file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法打开 ONNX 文件: " << onnx_path << std::endl;
        return false;
    }

    // 读取文件内容
    onnx_file.seekg(0, std::ios::end);
    size_t file_size = onnx_file.tellg();
    onnx_file.seekg(0, std::ios::beg);
    std::vector<char> onnx_data(file_size);
    onnx_file.read(onnx_data.data(), file_size);
    onnx_file.close();

    if (!parser->parse(onnx_data.data(), file_size)) {
        std::cerr << "[TensorRT] ERROR: ONNX 解析失败" << std::endl;
        for (int i = 0; i < parser->getNbErrors(); ++i) {
            std::cerr << "  " << parser->getError(i)->desc() << std::endl;
        }
        return false;
    }
    std::cout << "[TensorRT] ONNX 解析成功" << std::endl;

    // ---- 配置 ----
    auto config = std::shared_ptr<nvinfer1::IBuilderConfig>(
        builder->createBuilderConfig(),
        [](nvinfer1::IBuilderConfig* p) { if (p) p->destroy(); });

    config->setMaxWorkspaceSize(workspace);

    // FP16 模式（Jetson Nano 的 Volta GPU 有 FP16 Tensor Core）
    if (fp16 && builder->platformHasFastFp16()) {
        config->setFlag(nvinfer1::BuilderFlag::kFP16);
        std::cout << "[TensorRT] FP16 推理已启用" << std::endl;
    } else if (fp16) {
        std::cout << "[TensorRT] WARNING: 平台不支持 FP16，回退到 FP32" << std::endl;
    }

    // ---- 构建引擎 ----
    std::cout << "[TensorRT] 正在构建引擎（可能需要几分钟）..." << std::endl;

    auto engine = std::shared_ptr<nvinfer1::ICudaEngine>(
        builder->buildEngineWithConfig(*network, *config),
        [](nvinfer1::ICudaEngine* p) { if (p) p->destroy(); });

    if (!engine) {
        std::cerr << "[TensorRT] ERROR: 引擎构建失败" << std::endl;
        return false;
    }

    // ---- 序列化保存 ----
    auto serialized = std::shared_ptr<nvinfer1::IHostMemory>(
        engine->serialize(),
        [](nvinfer1::IHostMemory* p) { if (p) p->destroy(); });

    std::ofstream out_file(engine_path, std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法写入引擎文件: " << engine_path << std::endl;
        return false;
    }
    out_file.write(static_cast<const char*>(serialized->data()), serialized->size());
    out_file.close();

    std::cout << "[TensorRT] 引擎已保存至 " << engine_path
              << " (" << serialized->size() / (1024 * 1024) << " MB)" << std::endl;

    // 设置为当前引擎
    _engine = engine;
    _runtime = nullptr; // build 阶段不需要 runtime
    _context.reset(
        _engine->createExecutionContext(),
        [](nvinfer1::IExecutionContext* p) { if (p) p->destroy(); });

    return allocateBuffers();
}

// ============================================================
// 加载已序列化引擎
// ============================================================
bool TensorRTEngine::loadEngine(const std::string& engine_path) {
    std::cout << "[TensorRT] 加载引擎: " << engine_path << std::endl;

    // 读取文件
    std::ifstream file(engine_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法打开引擎文件: " << engine_path << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engine_data(size);
    file.read(engine_data.data(), size);
    file.close();

    // 创建 Runtime
    _runtime.reset(
        nvinfer1::createInferRuntime(_logger),
        [](nvinfer1::IRuntime* p) { if (p) p->destroy(); });

    // 反序列化
    _engine.reset(
        _runtime->deserializeCudaEngine(engine_data.data(), size),
        [](nvinfer1::ICudaEngine* p) { if (p) p->destroy(); });

    if (!_engine) {
        std::cerr << "[TensorRT] ERROR: 引擎反序列化失败" << std::endl;
        return false;
    }

    _context.reset(
        _engine->createExecutionContext(),
        [](nvinfer1::IExecutionContext* p) { if (p) p->destroy(); });

    std::cout << "[TensorRT] 引擎加载完成" << std::endl;
    return allocateBuffers();
}

// ============================================================
// 分配输入/输出 buffer
// ============================================================
bool TensorRTEngine::allocateBuffers() {
    if (!_engine || !_context) {
        std::cerr << "[TensorRT] ERROR: 引擎未就绪，无法分配 buffer" << std::endl;
        return false;
    }

    int num_bindings = _engine->getNbBindings();
    _gpu_buffers.resize(num_bindings, nullptr);
    _cpu_buffers.resize(num_bindings, nullptr);
    _buffer_sizes.resize(num_bindings, 0);

    _input_dims.clear();
    _output_dims.clear();

    for (int i = 0; i < num_bindings; ++i) {
        auto dims = _engine->getBindingDimensions(i);
        auto name = _engine->getBindingName(i);
        size_t size = 1;

        // 处理动态维度: 将 -1 替换为 1
        std::vector<int> dim_vec;
        for (int d = 0; d < dims.nbDims; ++d) {
            int val = dims.d[d];
            if (val == -1) val = 1;  // 动态维度取 1
            dim_vec.push_back(val);
            size *= val;
        }

        // 乘以数据类型大小
        auto dtype = _engine->getBindingDataType(i);
        switch (dtype) {
            case nvinfer1::DataType::kFLOAT: size *= sizeof(float); break;
            case nvinfer1::DataType::kHALF:  size *= sizeof(uint16_t); break;
            case nvinfer1::DataType::kINT8:  size *= sizeof(int8_t); break;
            case nvinfer1::DataType::kINT32: size *= sizeof(int32_t); break;
            default: size *= sizeof(float); break;
        }

        // 分配 GPU 内存
        cudaMalloc(&_gpu_buffers[i], size);

        // 分配 CPU 固定内存（pinned memory，加速 Host↔Device 传输）
        cudaMallocHost(&_cpu_buffers[i], size);

        _buffer_sizes[i] = size;

        if (_engine->bindingIsInput(i)) {
            _input_dims = dim_vec;
            std::cout << "[TensorRT] 输入 [" << name << "]: "
                      << dim_vec[0] << "x" << dim_vec[1] << "x"
                      << dim_vec[2] << "x" << dim_vec[3]
                      << " (" << size / 1024.0f << " KB)" << std::endl;
        } else {
            _output_dims.push_back(dim_vec);
            std::cout << "[TensorRT] 输出 [" << name << "]: [";
            for (size_t d = 0; d < dim_vec.size(); ++d) {
                if (d > 0) std::cout << "x";
                std::cout << dim_vec[d];
            }
            std::cout << "] (" << size / 1024.0f << " KB)" << std::endl;
        }
    }

    std::cout << "[TensorRT] Buffer 分配完成, 总计 "
              << (std::accumulate(_buffer_sizes.begin(), _buffer_sizes.end(), 0ULL) / (1024.0f * 1024.0f))
              << " MB" << std::endl;
    return true;
}

// ============================================================
// 推理
// ============================================================
bool TensorRTEngine::infer(const float* input,
                            std::vector<float*>& outputs,
                            int batch)
{
    if (!_context) {
        std::cerr << "[TensorRT] ERROR: 引擎未就绪" << std::endl;
        return false;
    }

    int num_bindings = _engine->getNbBindings();

    // ---- 拷贝输入到 GPU ----
    // 找到输入 binding 的索引
    int input_idx = -1;
    for (int i = 0; i < num_bindings; ++i) {
        if (_engine->bindingIsInput(i)) {
            input_idx = i;
            break;
        }
    }

    if (input_idx < 0) {
        std::cerr << "[TensorRT] ERROR: 未找到输入 binding" << std::endl;
        return false;
    }

    // CPU → GPU (异步)
    cudaMemcpyAsync(_gpu_buffers[input_idx], input,
                    _buffer_sizes[input_idx],
                    cudaMemcpyHostToDevice, _stream);

    // ---- 设置动态输入维度（如有） ----
    auto input_dims = _engine->getBindingDimensions(input_idx);
    if (input_dims.d[0] == -1) {
        // batch 维度是动态的
        nvinfer1::Dims4 set_dims{batch,
                                  _input_dims.size() > 1 ? _input_dims[1] : 3,
                                  _input_dims.size() > 2 ? _input_dims[2] : 640,
                                  _input_dims.size() > 3 ? _input_dims[3] : 640};
        _context->setBindingDimensions(input_idx, set_dims);
    }

    // ---- 执行推理 ----
    bool success = _context->enqueueV2(
        reinterpret_cast<void**>(_gpu_buffers.data()), _stream, nullptr);

    if (!success) {
        std::cerr << "[TensorRT] ERROR: 推理执行失败 (enqueueV2)" << std::endl;
        return false;
    }

    // ---- 拷贝输出回 CPU ----
    outputs.clear();
    outputs.resize(num_bindings - 1); // 减去输入

    int out_idx = 0;
    for (int i = 0; i < num_bindings; ++i) {
        if (!_engine->bindingIsInput(i)) {
            cudaMemcpyAsync(_cpu_buffers[i], _gpu_buffers[i],
                            _buffer_sizes[i],
                            cudaMemcpyDeviceToHost, _stream);
            outputs[out_idx] = static_cast<float*>(_cpu_buffers[i]);
            ++out_idx;
        }
    }

    // 等待所有异步操作完成
    cudaStreamSynchronize(_stream);

    return true;
}

// ============================================================
// 清理
// ============================================================
void TensorRTEngine::cleanup() {
    // 释放 GPU buffer
    for (auto& buf : _gpu_buffers) {
        if (buf) cudaFree(buf);
    }
    _gpu_buffers.clear();

    // 释放 CPU pinned buffer
    for (auto& buf : _cpu_buffers) {
        if (buf) cudaFreeHost(buf);
    }
    _cpu_buffers.clear();

    _buffer_sizes.clear();

    // 释放 TRT 对象（shared_ptr 自动管理）
    _context.reset();
    _engine.reset();
    _runtime.reset();

    if (_stream) {
        cudaStreamDestroy(_stream);
        _stream = nullptr;
    }

    std::cout << "[TensorRT] 资源已释放" << std::endl;
}
