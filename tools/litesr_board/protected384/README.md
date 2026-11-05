# Rectangular LiteSR runtime

Independent encrypted 384x256 residual-core runtime. Native H32/W48 D/I, work H64/W96; original weights, normalization, signed residual, valid/range rules and matched-frame identity retained. Build with a separately generated private Key.h and matching ARM libcrypto.so.1.1. No keys or plaintext models belong in payload. Legacy protected/ runtime and ciphertext remain unchanged.
