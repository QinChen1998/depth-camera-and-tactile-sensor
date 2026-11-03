#include "ProtectionCommon.h"
#include <iostream>
#include <sstream>

int wmain(int argc, wchar_t **argv) {
    try {
        dcts::require(argc == 4, "Usage: PackModelCopy copied-model.onnx NEW-model.dctsm NEW-private-key-header.h");
        dcts::SecureBytes original;
        original.bytes = dcts::readFile(argv[1], dcts::MaxModelBytes);
        const auto digest = dcts::sha256(original.bytes.data(), original.bytes.size());
        dcts::SecureBytes secret(32), mask(32);
        dcts::random(secret.bytes.data(), secret.bytes.size());
        dcts::random(mask.bytes.data(), mask.bytes.size());
        dcts::Envelope header{};
        std::memcpy(header.magic, dcts::Magic, sizeof(header.magic));
        header.version = 1; header.header_bytes = sizeof(header); header.plaintext_bytes = original.bytes.size();
        dcts::random(header.nonce, sizeof(header.nonce));
        std::memcpy(header.plaintext_sha256, digest.data(), digest.size());
        std::vector<unsigned char> package(sizeof(header) + original.bytes.size());
        auto start = dcts::Clock::now();
        dcts::crypt(true, secret.bytes.data(), header, original.bytes.data(), original.bytes.size(), package.data() + sizeof(header));
        const double encrypt_ms = dcts::elapsed(start);
        std::memcpy(package.data(), &header, sizeof(header));
        dcts::SecureBytes verified(original.bytes.size());
        start = dcts::Clock::now();
        dcts::crypt(false, secret.bytes.data(), header, package.data() + sizeof(header), original.bytes.size(), verified.bytes.data());
        const double decrypt_ms = dcts::elapsed(start);
        dcts::require(dcts::equal(original.bytes.data(), verified.bytes.data(), original.bytes.size()), "Round-trip model bytes differ");
        dcts::require(dcts::sha256(verified.bytes.data(), verified.bytes.size()) == digest, "Round-trip model SHA256 differs");

        std::ostringstream generated;
        generated << "// PRIVATE BUILD INPUT. Do not distribute this generated key material.\n#pragma once\n";
        for (int part = 0; part < 2; ++part) {
            generated << "static const unsigned char DctsKeyPart" << part << "[32] = {";
            for (size_t i = 0; i < 32; ++i) {
                if (i) generated << ',';
                generated << unsigned(part == 0 ? (secret.bytes[i] ^ mask.bytes[i]) : mask.bytes[i]);
            }
            generated << "};\n";
        }
        const auto text = generated.str();
        dcts::writeNewFile(argv[2], package.data(), package.size());
        dcts::writeNewFile(argv[3], reinterpret_cast<const unsigned char *>(text.data()), text.size());
        std::cout << "{\"passed\":true,\"roundtrip_bitwise_identical\":true,\"plaintext_bytes\":" << original.bytes.size()
                  << ",\"package_bytes\":" << package.size() << ",\"plaintext_sha256\":\"" << dcts::hex(digest.data(), digest.size())
                  << "\",\"encrypt_ms\":" << encrypt_ms << ",\"decrypt_ms\":" << decrypt_ms << "}\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 2; }
}
