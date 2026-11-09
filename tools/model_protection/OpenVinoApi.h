#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma push_macro("BOOLEAN")
#include <openvino/c/openvino.h>
#pragma pop_macro("BOOLEAN")
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

// Pin the native C API and resolve it from the private worker's runtime only.
// No OpenVINO import library or process-global PATH modification is required.
#define DCTS_OV_FUNCTIONS(X) \
    X(ov_core_create) X(ov_core_free) X(ov_core_read_model_from_memory_buffer) \
    X(ov_core_compile_model_props) X(ov_core_get_available_devices) \
    X(ov_available_devices_free) X(ov_core_get_property) X(ov_free) \
    X(ov_model_free) X(ov_tensor_create) X(ov_tensor_data) X(ov_tensor_free) \
    X(ov_tensor_get_shape) X(ov_shape_free) X(ov_tensor_get_element_type) \
    X(ov_tensor_get_byte_size) X(ov_compiled_model_free) \
    X(ov_compiled_model_create_infer_request) X(ov_infer_request_free) \
    X(ov_infer_request_get_input_tensor) X(ov_infer_request_get_output_tensor) \
    X(ov_infer_request_infer) X(ov_get_last_err_msg)

struct DctsOpenVinoApi {
#define DCTS_OV_FIELD(name) decltype(&name) name = nullptr;
    DCTS_OV_FUNCTIONS(DCTS_OV_FIELD)
#undef DCTS_OV_FIELD
    void load(HMODULE library) {
#define DCTS_OV_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(library, #name)); \
        if (!name) throw std::runtime_error("OpenVINO C API missing: " #name);
        DCTS_OV_FUNCTIONS(DCTS_OV_LOAD)
#undef DCTS_OV_LOAD
    }
    void check(ov_status_e status) const {
        if (status != OK) {
            const auto *message = ov_get_last_err_msg();
            throw std::runtime_error(message ? message : "OpenVINO operation failed");
        }
    }
};

// For IR the weight tensor owns its allocation. The compiled model may share
// it after the temporary decrypted envelope is wiped; never wrap that envelope
// with a borrowed host tensor.
inline ov_compiled_model_t *dctsCompileOpenVino(const DctsOpenVinoApi &api,
    ov_core_t *core, const char *device, const unsigned char *bytes, size_t size, bool ir) {
    ov_model_t *model = nullptr;
    ov_tensor_t *weights = nullptr;
    ov_compiled_model_t *compiled = nullptr;
    try {
        const char *graph = reinterpret_cast<const char *>(bytes);
        size_t graphSize = size;
        if (ir) {
            if (size < 24 || std::memcmp(bytes, "DCTOVIR1", 8))
                throw std::runtime_error("Invalid protected OpenVINO model container");
            uint64_t xmlSize = 0, weightSize = 0;
            std::memcpy(&xmlSize, bytes + 8, 8); std::memcpy(&weightSize, bytes + 16, 8);
            if (!xmlSize || !weightSize || xmlSize > size - 24 || weightSize != size - 24 - xmlSize)
                throw std::runtime_error("Invalid protected OpenVINO model lengths");
            int64_t dimension = static_cast<int64_t>(weightSize);
            const ov_shape_t shape{1, &dimension};
            api.check(api.ov_tensor_create(U8, shape, &weights));
            void *data = nullptr; api.check(api.ov_tensor_data(weights, &data));
            std::memcpy(data, bytes + 24 + xmlSize, size_t(weightSize));
            graph = reinterpret_cast<const char *>(bytes + 24); graphSize = size_t(xmlSize);
        }
        api.check(api.ov_core_read_model_from_memory_buffer(core, graph, graphSize, weights, &model));
        const bool cpu = std::strcmp(device, "CPU") == 0;
        const auto threads = std::to_string(std::min(8u, std::max(1u, std::thread::hardware_concurrency() / 2)));
        const ov_property_t properties[] = {{"PERFORMANCE_HINT", "LATENCY"},
            {"INFERENCE_PRECISION_HINT", cpu ? "f32" : "f16"},
            {"INFERENCE_NUM_THREADS", threads.c_str()}};
        api.check(api.ov_core_compile_model_props(core, model, device, cpu ? 3 : 2, properties, &compiled));
        api.ov_model_free(model); if (weights) api.ov_tensor_free(weights);
        return compiled;
    } catch (...) {
        if (compiled) api.ov_compiled_model_free(compiled);
        if (model) api.ov_model_free(model);
        if (weights) api.ov_tensor_free(weights);
        throw;
    }
}
