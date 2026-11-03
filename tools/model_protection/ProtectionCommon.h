#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dcts {
using Clock = std::chrono::steady_clock;
inline double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
inline void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
inline void checked(NTSTATUS status, const char *message) { require(status >= 0, message); }

struct SecureBytes {
    std::vector<unsigned char> bytes;
    explicit SecureBytes(size_t length = 0) : bytes(length) {}
    SecureBytes(const SecureBytes &) = delete;
    SecureBytes &operator=(const SecureBytes &) = delete;
    void wipe() noexcept {
        if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
        bytes.clear();
    }
    ~SecureBytes() { wipe(); }
};
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit File(HANDLE value) : handle(value) { require(handle != INVALID_HANDLE_VALUE, "File open failed"); }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File &) = delete;
};
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(const wchar_t *name) { checked(BCryptOpenAlgorithmProvider(&handle, name, nullptr, 0), "Crypto provider unavailable"); }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Key {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~Key() { if (handle) BCryptDestroyKey(handle); }
};
struct Hash {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~Hash() { if (handle) BCryptDestroyHash(handle); }
};

constexpr uint64_t MaxModelBytes = 512ull * 1024 * 1024;
#pragma pack(push, 1)
struct Envelope {
    unsigned char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint64_t plaintext_bytes;
    unsigned char nonce[12];
    unsigned char tag[16];
    unsigned char plaintext_sha256[32];
    unsigned char reserved[12];
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 96, "Envelope ABI changed");
constexpr unsigned char Magic[8] = {'D', 'C', 'T', 'S', 'M', '0', '1', 0};

inline std::vector<unsigned char> readFile(const wchar_t *path, uint64_t limit) {
    File file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER length{};
    require(GetFileSizeEx(file.handle, &length) && length.QuadPart > 0 && uint64_t(length.QuadPart) <= limit,
            "Model file size invalid");
    std::vector<unsigned char> result(size_t(length.QuadPart));
    size_t offset = 0;
    while (offset < result.size()) {
        DWORD count = 0;
        const auto chunk = DWORD(std::min<size_t>(4 * 1024 * 1024, result.size() - offset));
        require(ReadFile(file.handle, result.data() + offset, chunk, &count, nullptr) && count == chunk,
                "Model read failed");
        offset += count;
    }
    return result;
}
inline void writeNewFile(const wchar_t *path, const unsigned char *data, size_t length) {
    // CREATE_NEW prevents replacing the input, an existing model or an earlier result.
    File file(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    size_t offset = 0;
    while (offset < length) {
        DWORD count = 0;
        const auto chunk = DWORD(std::min<size_t>(4 * 1024 * 1024, length - offset));
        require(WriteFile(file.handle, data + offset, chunk, &count, nullptr) && count == chunk,
                "New output write failed");
        offset += count;
    }
    require(FlushFileBuffers(file.handle) != FALSE, "New output flush failed");
}
inline ULONG objectBytes(BCRYPT_ALG_HANDLE algorithm) {
    ULONG size = 0, count = 0;
    checked(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                             sizeof(size), &count, 0), "Crypto object length unavailable");
    return size;
}
inline std::array<unsigned char, 32> sha256(const unsigned char *data, size_t size) {
    Algorithm algorithm(BCRYPT_SHA256_ALGORITHM);
    SecureBytes object(objectBytes(algorithm.handle));
    Hash hash;
    checked(BCryptCreateHash(algorithm.handle, &hash.handle, object.bytes.data(), ULONG(object.bytes.size()),
                            nullptr, 0, 0), "SHA256 creation failed");
    require(size <= ULONG_MAX, "SHA256 input too large");
    checked(BCryptHashData(hash.handle, const_cast<PUCHAR>(data), ULONG(size), 0), "SHA256 update failed");
    std::array<unsigned char, 32> result{};
    checked(BCryptFinishHash(hash.handle, result.data(), ULONG(result.size()), 0), "SHA256 finish failed");
    return result;
}
inline std::string hex(const unsigned char *data, size_t length) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value(length * 2, '0');
    for (size_t i = 0; i < length; ++i) { value[2*i] = digits[data[i] >> 4]; value[2*i+1] = digits[data[i] & 15]; }
    return value;
}
inline bool equal(const unsigned char *left, const unsigned char *right, size_t size) {
    unsigned char difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}
inline void random(unsigned char *bytes, size_t length) {
    checked(BCryptGenRandom(nullptr, bytes, ULONG(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Secure random generation failed");
}
inline void crypt(bool encrypt, const unsigned char *secret, Envelope &header,
                  const unsigned char *input, size_t length, unsigned char *output) {
    Algorithm algorithm(BCRYPT_AES_ALGORITHM);
    checked(BCryptSetProperty(algorithm.handle, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0), "AES-GCM mode unavailable");
    SecureBytes object(objectBytes(algorithm.handle));
    Key key;
    checked(BCryptGenerateSymmetricKey(algorithm.handle, &key.handle, object.bytes.data(), ULONG(object.bytes.size()),
                                      const_cast<PUCHAR>(secret), 32, 0), "AES256 key creation failed");
    Envelope authenticated = header;
    SecureZeroMemory(authenticated.tag, sizeof(authenticated.tag));
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = header.nonce; info.cbNonce = sizeof(header.nonce);
    info.pbTag = header.tag; info.cbTag = sizeof(header.tag);
    info.pbAuthData = reinterpret_cast<PUCHAR>(&authenticated); info.cbAuthData = sizeof(authenticated);
    ULONG written = 0;
    require(length <= ULONG_MAX, "AES-GCM input too large");
    const auto status = encrypt
        ? BCryptEncrypt(key.handle, const_cast<PUCHAR>(input), ULONG(length), &info, nullptr, 0, output, ULONG(length), &written, 0)
        : BCryptDecrypt(key.handle, const_cast<PUCHAR>(input), ULONG(length), &info, nullptr, 0, output, ULONG(length), &written, 0);
    checked(status, encrypt ? "Model encryption failed" : "Model AES-GCM authentication failed");
    require(written == length, "AES-GCM output length mismatch");
}
inline Envelope parseEnvelope(const std::vector<unsigned char> &file) {
    require(file.size() >= sizeof(Envelope), "Truncated model envelope");
    Envelope header{};
    std::memcpy(&header, file.data(), sizeof(header));
    require(equal(header.magic, Magic, sizeof(Magic)) && header.version == 1 && header.header_bytes == sizeof(Envelope),
            "Unsupported model envelope");
    require(header.plaintext_bytes > 0 && header.plaintext_bytes <= MaxModelBytes &&
            file.size() - sizeof(Envelope) == header.plaintext_bytes, "Encrypted model length mismatch");
    for (auto byte : header.reserved) require(byte == 0, "Unsupported model envelope flags");
    return header;
}
}
