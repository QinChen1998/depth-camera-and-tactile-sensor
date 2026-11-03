#pragma once
#include "Key.h"
#include <cstdint>

// OpenSSL 1.1 EVP ABI exported by the board's existing libcrypto.
extern "C" {
struct evp_cipher_ctx_st;
struct evp_cipher_st;
struct engine_st;
evp_cipher_ctx_st* EVP_CIPHER_CTX_new();
void EVP_CIPHER_CTX_free(evp_cipher_ctx_st*);
const evp_cipher_st* EVP_aes_256_gcm();
int EVP_DecryptInit_ex(evp_cipher_ctx_st*, const evp_cipher_st*, engine_st*, const unsigned char*, const unsigned char*);
int EVP_DecryptUpdate(evp_cipher_ctx_st*, unsigned char*, int*, const unsigned char*, int);
int EVP_DecryptFinal_ex(evp_cipher_ctx_st*, unsigned char*, int*);
int EVP_CIPHER_CTX_ctrl(evp_cipher_ctx_st*, int, int, void*);
unsigned char* SHA256(const unsigned char*, size_t, unsigned char*);
void OPENSSL_cleanse(void*, size_t);
}

struct SecureBytes {
    std::vector<unsigned char> bytes;
    explicit SecureBytes(size_t size) : bytes(size) {}
    ~SecureBytes() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

#pragma pack(push, 1)
struct ProtectedEnvelope {
    unsigned char magic[8];
    uint32_t version, headerSize;
    uint64_t modelSize;
    unsigned char nonce[12], tag[16], hash[32], reserved[12];
};
#pragma pack(pop)
static_assert(sizeof(ProtectedEnvelope) == 96);
constexpr size_t ProtectedModelSize = 575927;

void initProtectedModel(rknn_context& context, const std::string& path) {
    ProtectedEnvelope header{};
    std::vector<unsigned char> ciphertext(ProtectedModelSize);
    {
        std::ifstream model(path, std::ios::binary);
        require(bool(model.read(reinterpret_cast<char*>(&header), sizeof(header))), "Envelope header missing");
        require(bool(model.read(reinterpret_cast<char*>(ciphertext.data()), ciphertext.size()))
                    && model.peek() == EOF, "Envelope size mismatch");
    }
    const unsigned char magic[8] = {'D','C','T','S','M','0','1',0};
    require(std::memcmp(header.magic, magic, sizeof(magic)) == 0 && header.version == 1
            && header.headerSize == sizeof(header) && header.modelSize == ProtectedModelSize
            && std::all_of(std::begin(header.reserved), std::end(header.reserved), [](auto b) { return b == 0; }),
            "Unsupported envelope");
    auto aad = header;
    std::memset(aad.tag, 0, sizeof(aad.tag));

    SecureBytes plaintext(ProtectedModelSize);
    {
        SecureBytes key(32);
        for (size_t i = 0; i < key.bytes.size(); ++i)
            key.bytes[i] = DctsKeyPart0[i] ^ DctsKeyPart1[i];
        std::unique_ptr<evp_cipher_ctx_st, decltype(&EVP_CIPHER_CTX_free)>
            cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        int aadLength = 0, written = 0, tail = 0;
        unsigned char finalBlock[16]{};
        constexpr int SetGcmTag = 0x11;
        require(cipher && EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), header.nonce) == 1
                && EVP_DecryptUpdate(cipher.get(), nullptr, &aadLength, reinterpret_cast<const unsigned char*>(&aad), sizeof(aad)) == 1
                && EVP_DecryptUpdate(cipher.get(), plaintext.bytes.data(), &written, ciphertext.data(), ciphertext.size()) == 1
                && EVP_CIPHER_CTX_ctrl(cipher.get(), SetGcmTag, sizeof(header.tag), header.tag) == 1
                && EVP_DecryptFinal_ex(cipher.get(), finalBlock, &tail) == 1
                && written == int(ProtectedModelSize) && tail == 0, "AES-GCM authentication failed");
    }
    unsigned char digest[32]{};
    require(SHA256(plaintext.bytes.data(), plaintext.bytes.size(), digest)
            && std::memcmp(digest, header.hash, sizeof(digest)) == 0, "Plaintext digest mismatch");
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    require(hex == LiteSrBoardContract::NearModelSha, "Unexpected plaintext model identity");

    check(rknn_init(&context, plaintext.bytes.data(), plaintext.bytes.size(), 0, nullptr), "rknn_init protected model");
}
