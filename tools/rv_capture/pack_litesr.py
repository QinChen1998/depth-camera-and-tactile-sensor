#!/usr/bin/env python3
"""Prepare the pinned near-depth model for the private RV1103 runtime."""
import argparse
from hashlib import sha256
from pathlib import Path
import secrets
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path, help='New private build directory')
    args = parser.parse_args()

    model = args.source.read_bytes()
    digest = sha256(model).digest()
    if len(model) != 575927 or digest.hex() != 'a916354dce57bd4cf4559cde91270cbd50bb2b14302ed277e8653e02b525268b':
        parser.error('The input is not the pinned near-depth LiteSR 256 RKNN model')
    key, mask, nonce = secrets.token_bytes(32), secrets.token_bytes(32), secrets.token_bytes(12)
    header = struct.Struct('<8sIIQ12s16s32s12s')
    fields = (b'DCTSM01\0', 1, header.size, len(model), nonce)
    aad = header.pack(*fields, bytes(16), digest, bytes(12))
    sealed = AESGCM(key).encrypt(nonce, model, aad)
    if sha256(AESGCM(key).decrypt(nonce, sealed, aad)).digest() != digest:
        raise RuntimeError('Encrypted model did not round-trip')

    args.output.mkdir(mode=0o700)
    (args.output / 'litesr-near-256.dctsm').write_bytes(header.pack(*fields, sealed[-16:], digest, bytes(12)) + sealed[:-16])
    parts = (mask, bytes(left ^ right for left, right in zip(key, mask)))
    with (args.output / 'model_key_material.h').open('x', encoding='ascii') as output:
        output.write('#pragma once\n')
        for index, part in enumerate(parts):
            output.write(f'constexpr unsigned char DctsKeyPart{index}[] = {{')
            output.write(', '.join(f'0x{byte:02x}' for byte in part) + '};\n')


if __name__ == '__main__':
    main()
