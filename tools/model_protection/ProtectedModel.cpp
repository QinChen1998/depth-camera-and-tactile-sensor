#define DCTS_MODEL_PROTECTION_BUILD
#include "ModelProtectionApi.h"
#include "ProtectionCommon.h"
#include "model_key_material.h"

extern "C" __declspec(dllexport) int __cdecl DctsCreateProtectedSession(
    const OrtApi *api, const OrtEnv *environment, const OrtSessionOptions *options,
    const wchar_t *ciphertext_path, const char *expected_sha256, OrtSession **session,
    DctsModelLoadMetrics *metrics, char *error, size_t error_bytes) {
    if (session) *session = nullptr;
    if (error && error_bytes) error[0] = '\0';
    try {
        dcts::require(api && environment && options && ciphertext_path && expected_sha256 && session && metrics,
                      "Invalid protected model arguments");
        dcts::require(metrics->struct_bytes == sizeof(*metrics), "Protected loader ABI mismatch");
        *metrics = {}; metrics->struct_bytes = sizeof(*metrics); metrics->abi_version = 1;
        dcts::require(std::strlen(expected_sha256) == 64, "Expected model SHA256 invalid");
        for (size_t i = 0; i < 64; ++i)
            dcts::require((expected_sha256[i] >= '0' && expected_sha256[i] <= '9') ||
                          (expected_sha256[i] >= 'a' && expected_sha256[i] <= 'f'), "Expected model SHA256 invalid");
        const auto total = dcts::Clock::now();
        auto start = dcts::Clock::now();
        auto package = dcts::readFile(ciphertext_path, dcts::MaxModelBytes + sizeof(dcts::Envelope));
        auto header = dcts::parseEnvelope(package);
        metrics->plaintext_bytes = header.plaintext_bytes;
        metrics->read_ms = dcts::elapsed(start);
        dcts::SecureBytes secret(32), plaintext(size_t(header.plaintext_bytes));
        for (size_t i = 0; i < secret.bytes.size(); ++i) secret.bytes[i] = DctsKeyPart0[i] ^ DctsKeyPart1[i];
        start = dcts::Clock::now();
        dcts::crypt(false, secret.bytes.data(), header, package.data() + sizeof(header), plaintext.bytes.size(), plaintext.bytes.data());
        secret.wipe();
        metrics->decrypt_ms = dcts::elapsed(start);
        start = dcts::Clock::now();
        const auto digest = dcts::sha256(plaintext.bytes.data(), plaintext.bytes.size());
        dcts::require(dcts::equal(digest.data(), header.plaintext_sha256, digest.size()) &&
                      dcts::hex(digest.data(), digest.size()) == expected_sha256, "Decrypted model fingerprint mismatch");
        metrics->digest_ms = dcts::elapsed(start);
        start = dcts::Clock::now();
        // All parameters and CUDA provider options come from the unchanged host path.
        // ONNX model bytes remain alive until ORT has created its session.
        auto *status = api->CreateSessionFromArray(environment, plaintext.bytes.data(), plaintext.bytes.size(), options, session);
        metrics->session_create_ms = dcts::elapsed(start);
        if (status) {
            const std::string message = api->GetErrorMessage(status);
            api->ReleaseStatus(status);
            throw std::runtime_error(message);
        }
        start = dcts::Clock::now();
        plaintext.wipe();
        metrics->plaintext_clear_ms = dcts::elapsed(start);
        metrics->total_ms = dcts::elapsed(total);
        return 0;
    } catch (const std::exception &failure) {
        if (api && session && *session) { api->ReleaseSession(*session); *session = nullptr; }
        if (error && error_bytes) {
            const auto length = std::min(error_bytes - 1, std::strlen(failure.what()));
            std::memcpy(error, failure.what(), length); error[length] = '\0';
        }
        return 1;
    } catch (...) {
        if (api && session && *session) { api->ReleaseSession(*session); *session = nullptr; }
        if (error && error_bytes) {
            constexpr char message[] = "Unknown protected model failure";
            const auto length = std::min(error_bytes - 1, sizeof(message) - 1);
            std::memcpy(error, message, length); error[length] = '\0';
        }
        return 2;
    }
}
