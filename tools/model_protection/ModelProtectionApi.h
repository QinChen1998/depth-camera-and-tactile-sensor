#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../../dependency_sources/onnxruntime/onnxruntime_c_api.h"

// C ABI: the DLL returns an ORT session, never a plaintext model buffer.
struct DctsModelLoadMetrics {
    uint32_t struct_bytes;
    uint32_t abi_version;
    uint64_t plaintext_bytes;
    double read_ms;
    double decrypt_ms;
    double digest_ms;
    double session_create_ms;
    double plaintext_clear_ms;
    double total_ms;
};

using DctsCreateProtectedSessionFn = int (__cdecl *)(
    const OrtApi *, const OrtEnv *, const OrtSessionOptions *, const wchar_t *,
    const char *, OrtSession **, DctsModelLoadMetrics *, char *, size_t);

#ifdef DCTS_MODEL_PROTECTION_BUILD
extern "C" __declspec(dllexport) int __cdecl DctsCreateProtectedSession(
    const OrtApi *api, const OrtEnv *environment, const OrtSessionOptions *options,
    const wchar_t *ciphertext_path, const char *expected_sha256, OrtSession **session,
    DctsModelLoadMetrics *metrics, char *error, size_t error_bytes);
#endif
