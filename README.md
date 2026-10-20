# backbone-core

The C++20 engine shared by our drop-in replacement clients for the LayerTwo-family
sidechains on eCash (Thunder, BitNames, BitAssets, zSide, Truthcoin). It holds everything
that does not depend on one chain's rules: borsh/bincode codecs, BLAKE3/SHA, FROST
ristretto255, the rustreexo port, rust-bitcoin address/tx helpers, BIP-39 + bip32ish keys,
the ordered KV store, cusf.mainchain.v1 types, JSON-RPC server/client, logging, and (in
`bt_net`) QUIC, gRPC and the enforcer client. See `doc/DESIGN.md`.

Chain repositories (`backbone-thunder`, `backbone-bitnames`, ...) carry this repository as a
git submodule at `core/` and call `add_subdirectory(core)`. Fixes to the core land here once
and every chain picks them up by moving its submodule pointer.

## Build and test (standalone)

```
depends/fetch.sh
cmake -S depends -B depends/work/linux                 # OpenSSL 3.5.9 + ngtcp2 1.25.0
~/skydoge/tools/build.sh ~/skydoge/backbone-core depends/work/linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo   # system libsodium, sqlite3, nghttp2
~/skydoge/tools/build.sh ~/skydoge/backbone-core build
build/bt_core_tests
```

Inside a chain repository the same commands run with `core/depends` (the chain's
top-level CMake finds `core/depends/built/<host>` automatically). The mingw-w64
toolchain file is `cmake/mingw64.cmake`.
