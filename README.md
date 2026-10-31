# goose-wasm

A WebAssembly backend for the [Goose programming language](https://github.com/aardappel/goose).

This is an independent, unofficial project. It is not affiliated with or endorsed
by the Goose project or its author.

## Upstream

This project follows the reference Goose implementation:

- Repository: <https://github.com/aardappel/goose>
- Author: Wouter van Oortmerssen
- License: Apache License 2.0
- Included as a git submodule at [`upstream/goose`](upstream/goose), pinned to a
  specific commit. Its contents are unmodified and remain under their own licenses:
  see [`upstream/goose/LICENSE`](upstream/goose/LICENSE) and the license files in
  `upstream/goose/third_party/`.

The language semantics are those of the upstream specification
([`docs/goose_spec.md`](upstream/goose/docs/goose_spec.md)) and compiler. Where
this backend differs, the difference is a bug unless documented.

Clone with the submodule:

```sh
git clone --recurse-submodules https://github.com/nenaaki/goose-wasm.git
```

## Building

Needs CMake, Ninja and a clang (llvm-mingw on Windows; MSVC is not used).

```sh
cmake -S . -B build/mingw -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/mingw
build/mingw/goose-wasm file.goose -o file.wasm
```

`python scripts/difftest.py` runs upstream's test fixtures through
goose-wasm and wasmtime, validates every module with `wasm-tools validate`,
and compares the output with upstream's expected output. Fixtures using what
the backend does not translate yet are counted as `unsupported`.

The compiler uses upstream's front end unmodified; see
[docs/design/architecture.md](docs/design/architecture.md). Upstream code
copied into this repository lives only under `src/upstream_copy/`, between
marker lines; `python scripts/upstream_copy.py check` verifies it is still
identical to upstream.

## License

Licensed under the Apache License, Version 2.0, the same license as upstream
Goose. See [LICENSE](LICENSE).

## Third-party notices

See [NOTICE](NOTICE).
