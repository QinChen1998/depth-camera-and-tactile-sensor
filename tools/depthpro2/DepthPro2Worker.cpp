#include "../../sensors/RvCameraView.h"
#ifdef DCTS_PROTECTED_DEPTHPRO2
#include "../model_protection/ProtectedTensorRt.h"
#endif

#include <NvInferPlugin.h>
#include <NvInferRuntime.h>
#include <cuda_runtime_api.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
constexpr int Width = RvCameraView::DepthPro2Portrait.inputWidth;
constexpr int Height = RvCameraView::DepthPro2Portrait.inputHeight;
constexpr int Pixels = Width * Height;
constexpr int RgbBytes = Pixels * 3;
constexpr int DepthBytes = Pixels * sizeof(float);
constexpr char PixelTensor[] = "pixel_values";
constexpr char FocalTensor[] = "f_px";
constexpr char DepthTensor[] = "depth_meters";

void require(bool passed, const char *message)
{
    if (!passed)
        throw std::runtime_error(message);
}

void checkCuda(cudaError_t status, const char *operation)
{
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

class Logger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char *message) noexcept override
    {
        if (severity <= Severity::kWARNING)
            std::fprintf(stderr, "%s\n", message);
    }
};

class DepthEngine final {
public:
    DepthEngine(const QDir &root, const RvCameraView::DepthPro2Runtime &asset)
    {
#ifndef DCTS_PROTECTED_DEPTHPRO2
        QFile file(root.filePath(QLatin1String(asset.view.graphFile)));
        require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 512 * 1024 * 1024,
                "Cannot read the Depth Pro 2 engine");
        const auto plan = file.readAll();
        require(QCryptographicHash::hash(plan, QCryptographicHash::Sha256).toHex() == asset.engineSha,
                "Depth Pro 2 engine fingerprint mismatch");
#endif
        require(getInferLibVersion() == NV_TENSORRT_VERSION, "TensorRT runtime version mismatch");
        checkCuda(cudaSetDevice(0), "Select NVIDIA device");
        require(initLibNvInferPlugins(&logger, ""), "TensorRT plugin initialization failed");
        runtime.reset(nvinfer1::createInferRuntime(logger));
        require(bool(runtime), "Cannot create the TensorRT runtime");
#ifdef DCTS_PROTECTED_DEPTHPRO2
        const auto path = root.filePath(QLatin1String(RvCameraView::DepthPro2ProtectedEngine)).toStdWString();
        std::array<char, 256> error{};
        engine.reset(DctsLoadProtectedTensorRtEngine(runtime.get(), path.c_str(), asset.engineSha,
                                                      error.data(), error.size()));
        require(bool(engine), error[0] ? error.data() : "Cannot deserialize the protected Depth Pro 2 engine");
#else
        engine.reset(runtime->deserializeCudaEngine(plan.constData(), size_t(plan.size())));
        require(bool(engine), "Cannot deserialize the Depth Pro 2 engine for this GPU");
#endif
        require(engine->getNbIOTensors() == 3
                && matches(PixelTensor, nvinfer1::TensorIOMode::kINPUT, {1, 3, Height, Width})
                && matches(FocalTensor, nvinfer1::TensorIOMode::kINPUT, {1})
                && matches(DepthTensor, nvinfer1::TensorIOMode::kOUTPUT, {1, Height, Width}),
                "Depth Pro 2 TensorRT tensor layout mismatch");
        context.reset(engine->createExecutionContext());
        require(bool(context), "Cannot create the TensorRT execution context");

        cudaStream_t active = nullptr;
        checkCuda(cudaStreamCreateWithFlags(&active, cudaStreamNonBlocking), "Create CUDA stream");
        stream.reset(active);
        input.reset(allocate(RgbBytes * sizeof(float), false));
        focal.reset(allocate(sizeof(float), false));
        output.reset(allocate(DepthBytes, false));
        hostInput.reset(allocate(RgbBytes * sizeof(float), true));
        hostOutput.reset(allocate(DepthBytes, true));

        const float calibratedFocal = float(asset.view.focal);
        checkCuda(cudaMemcpyAsync(focal.get(), &calibratedFocal, sizeof(calibratedFocal),
                                  cudaMemcpyHostToDevice, stream.get()), "Upload calibrated focal length");
        require(context->setTensorAddress(PixelTensor, input.get())
                && context->setTensorAddress(FocalTensor, focal.get())
                && context->setTensorAddress(DepthTensor, output.get()),
                "Cannot bind the Depth Pro 2 TensorRT tensors");
        checkCuda(cudaStreamSynchronize(stream.get()), "Initialize CUDA bindings");

        // Exercise the actual engine at startup while the host still shows loading.
        QByteArray probe(RgbBytes, Qt::Uninitialized);
        for (int i = 0; i < probe.size(); ++i)
            probe[i] = char((i / 7 + i / Width) & 255);
        infer(probe, 0);
    }

    QByteArray infer(const QByteArray &planar, int rotation)
    {
        require(planar.size() == RgbBytes && (rotation == 0 || rotation == 180),
                "Invalid 480x640 RGB input");
        auto *rgb = static_cast<float *>(hostInput.get());
        const auto *source = reinterpret_cast<const unsigned char *>(planar.constData());
        for (int c = 0; c < 3; ++c) {
            const auto *plane = source + c * Pixels;
            for (int i = 0; i < Pixels; ++i)
                rgb[c * Pixels + i] = float(plane[rotation == 0 ? i : Pixels - 1 - i]) / 255.f;
        }

        checkCuda(cudaMemcpyAsync(input.get(), rgb, RgbBytes * sizeof(float),
                                  cudaMemcpyHostToDevice, stream.get()), "Upload RGB tensor");
        require(context->enqueueV3(stream.get()), "Depth Pro 2 TensorRT inference failed");
        auto *depth = static_cast<float *>(hostOutput.get());
        checkCuda(cudaMemcpyAsync(depth, output.get(), DepthBytes, cudaMemcpyDeviceToHost,
                                  stream.get()), "Read camera-Z depth");
        checkCuda(cudaStreamSynchronize(stream.get()), "Complete Depth Pro 2-S inference");

        int valid = 0;
        for (int i = 0; i < Pixels; ++i) {
            auto &z = depth[i];
            require(!std::isnan(z) && z >= 0.f, "Invalid Depth Pro 2 depth value");
            if (std::isinf(z))
                z = 0.f;
            valid += z > 0.f;
        }
        require(valid != 0, "Depth Pro 2 did not return a finite positive depth");
        if (rotation == 180)
            std::reverse(depth, depth + Pixels);
        return QByteArray(reinterpret_cast<const char *>(depth), DepthBytes);
    }

private:
    static void *allocate(size_t bytes, bool pinned)
    {
        void *memory = nullptr;
        if (pinned)
            checkCuda(cudaMallocHost(&memory, bytes), "Allocate pinned RGB/depth buffer");
        else
            checkCuda(cudaMalloc(&memory, bytes), "Allocate CUDA tensor");
        return memory;
    }

    bool matches(const char *name, nvinfer1::TensorIOMode io, std::initializer_list<int> shape) const
    {
        const auto dims = engine->getTensorShape(name);
        return dims.nbDims == static_cast<int>(shape.size())
            && std::equal(shape.begin(), shape.end(), dims.d)
            && engine->getTensorIOMode(name) == io
            && engine->getTensorDataType(name) == nvinfer1::DataType::kFLOAT
            && engine->getTensorLocation(name) == nvinfer1::TensorLocation::kDEVICE;
    }

    Logger logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    std::unique_ptr<std::remove_pointer_t<cudaStream_t>, decltype(&cudaStreamDestroy)> stream{nullptr, cudaStreamDestroy};
    std::unique_ptr<void, decltype(&cudaFree)> input{nullptr, cudaFree}, focal{nullptr, cudaFree}, output{nullptr, cudaFree};
    std::unique_ptr<void, decltype(&cudaFreeHost)> hostInput{nullptr, cudaFreeHost}, hostOutput{nullptr, cudaFreeHost};
};

bool send(const QJsonObject &header, const QByteArray &bytes = {})
{
    const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    std::array<quint32, 3> prefix = {qToLittleEndian(quint32(json.size())),
                                    qToLittleEndian(quint32(bytes.size())), 0};
    return std::fwrite(prefix.data(), 1, sizeof(prefix), stdout) == sizeof(prefix)
        && std::fwrite(json.constData(), 1, size_t(json.size()), stdout) == size_t(json.size())
        && (bytes.isEmpty() || std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) == size_t(bytes.size()))
        && std::fflush(stdout) == 0;
}

bool read(QJsonObject &request, QByteArray &pixels)
{
    std::array<quint32, 3> prefix{};
    const auto count = std::fread(prefix.data(), 1, sizeof(prefix), stdin);
    if (count == 0 && std::feof(stdin))
        return false;
    require(count == sizeof(prefix), "Incomplete Depth Pro 2 request prefix");
    const auto size = qFromLittleEndian(prefix[0]);
    require(size > 0 && size <= 65536 && qFromLittleEndian(prefix[1]) == RgbBytes
            && qFromLittleEndian(prefix[2]) == 0, "Invalid Depth Pro 2 request sizes");
    QByteArray json(int(size), Qt::Uninitialized);
    require(std::fread(json.data(), 1, size, stdin) == size
            && std::fread(pixels.data(), 1, RgbBytes, stdin) == RgbBytes,
            "Incomplete Depth Pro 2 request");
    const auto document = QJsonDocument::fromJson(json);
    require(document.isObject(), "Invalid Depth Pro 2 request JSON");
    request = document.object();
    return true;
}

int run(const QString &root, const QString &token, const QString &model,
        const RvCameraView::DepthPro2Runtime &asset)
{
    try {
        const auto &view = asset.view;
        DepthEngine engine(QDir(root), asset);
        const auto identity = RvCameraView::depthPro2Identity(model, asset);
        QJsonObject ready = identity;
        ready["protocol"] = 1;
        ready["type"] = "ready";
        ready["token"] = token;
        ready["model_loading"] = QJsonObject{{"provider", "TensorRT"},
            {"device", RvCameraView::DepthPro2Backend}, {"tensorrt_version", RvCameraView::DepthPro2TensorRt}};
        if (!send(ready))
            return 3;

        QJsonObject request;
        QByteArray pixels(RgbBytes, Qt::Uninitialized);
        while (read(request, pixels)) {
            auto metadata = request["metadata"].toObject();
            const int rotation = metadata["assist_input_rotation_degrees"].toInt(-1);
            require(request["protocol"] == 1 && request["type"] == "infer" && request["token"] == token
                && request["host_model"] == model
                && request["request"].toInteger() > 0 && request["epoch"].toInteger() > 0
                && metadata["rgb_width"] == Width && metadata["rgb_height"] == Height
                && metadata["dtof_present"] == false
                && metadata["rectify_map_sha256"] == QLatin1String(view.mapSha), "Depth Pro 2 request identity mismatch");
            QElapsedTimer elapsed;
            elapsed.start();
            const auto depth = engine.infer(pixels, rotation);
            metadata["assist_inference_ms"] = elapsed.nsecsElapsed() / 1e6;
            for (auto item = identity.begin(); item != identity.end(); ++item)
                metadata["assist_" + item.key()] = item.value();
            metadata["assist_backend"] = RvCameraView::DepthPro2Backend;
            metadata["assist_execution_device"] = RvCameraView::DepthPro2Backend;
            metadata["assist_output_grid"] = RvCameraView::DepthPro2Grid;
            metadata["assist_output_restored_to_calibrated_grid"] = true;
            metadata["assist_input_orientation_checked"] = true;
            metadata["depth_K"] = RvCameraView::intrinsics(view);
            metadata["width"] = Width;
            metadata["height"] = Height;
            metadata["depth_stride"] = Width * int(sizeof(float));
            metadata["depth_unit"] = "camera-Z_m_float32";
            metadata["depth_location"] = "host";
            metadata["depth_method"] = "monocular_unadjusted";
            metadata["assist_fusion"] = QJsonObject{{"fusion_mode", "monocular"}, {"dtof_corrected", false}};
            if (!send({{"protocol", 1}, {"type", "result"}, {"token", token},
                       {"request", request["request"]}, {"epoch", request["epoch"]}, {"metadata", metadata}}, depth))
                return 3;
        }
        return 0;
    } catch (const std::exception &error) {
        send({{"protocol", 1}, {"type", "fatal"}, {"token", token}, {"error", QString::fromUtf8(error.what())}});
        return 2;
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5 || args[1] != "--rv-assist-worker" || args[3].isEmpty())
        return 3;
    const auto *asset = RvCameraView::depthPro2Runtime(args[4]);
    if (!asset)
        return 3;
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 3;
    return run(args[2], args[3], args[4], *asset);
}
