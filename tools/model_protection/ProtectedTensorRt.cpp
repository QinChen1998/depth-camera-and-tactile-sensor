#define DCTS_PROTECTED_TENSORRT_BUILD
#include "ProtectedTensorRt.h"
#include "ProtectionCommon.h"
#include "model_key_material.h"

#include <NvInferRuntime.h>

extern "C" nvinfer1::ICudaEngine *DctsLoadProtectedTensorRtEngine(
    nvinfer1::IRuntime *runtime, const wchar_t *path, const char *expectedSha256,
    char *error, std::size_t errorBytes) noexcept
{
    if (error && errorBytes)
        error[0] = '\0';
    try {
        dcts::require(runtime && path && expectedSha256, "Invalid protected TensorRT loader arguments");
        const auto package = dcts::readFile(path, dcts::MaxModelBytes + sizeof(dcts::Envelope));
        auto header = dcts::parseEnvelope(package);
        dcts::SecureBytes secret(32), plan(std::size_t(header.plaintext_bytes));
        for (std::size_t i = 0; i < secret.bytes.size(); ++i)
            secret.bytes[i] = DctsKeyPart0[i] ^ DctsKeyPart1[i];
        dcts::crypt(false, secret.bytes.data(), header, package.data() + sizeof(header),
                    plan.bytes.size(), plan.bytes.data());
        secret.wipe();
        const auto digest = dcts::sha256(plan.bytes.data(), plan.bytes.size());
        dcts::require(dcts::equal(digest.data(), header.plaintext_sha256, digest.size())
            && dcts::hex(digest.data(), digest.size()) == expectedSha256, "Decrypted TensorRT engine fingerprint mismatch");
        auto *engine = runtime->deserializeCudaEngine(plan.bytes.data(), plan.bytes.size());
        plan.wipe();
        dcts::require(engine != nullptr, "Cannot deserialize the protected Depth Pro 2 engine for this GPU");
        return engine;
    } catch (const std::exception &failure) {
        if (error && errorBytes) {
            const auto length = std::min(errorBytes - 1, std::strlen(failure.what()));
            std::memcpy(error, failure.what(), length);
            error[length] = '\0';
        }
    } catch (...) {
        // Keep exceptions inside the loader DLL.
    }
    return nullptr;
}
