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

## Status

As of 2026-11-08, of the 740 fixtures `scripts/difftest.py` runs (upstream's
test suite and this repository's `tests/`), with no wrong output, wrong exit
or invalid module at either level:

| Level | Pass | Unsupported |
|---|---|---|
| `-O2` | 675 | 65 |
| `-O0` | 675 | 65 |

Translated: scalars and fixed aggregates; strings, slices, fixed, limited and
resizable arrays; variable-size values (`T[]`, `varint`, variable structs and
variable-mode ADTs) as packed images; structs ending in a resizable array
(frame objects, with or without a variable-size prefix) and ADTs with such a
variant, as values, results, parameters and in whole assignment; fixed and
varint-width relative references and `in pool` references; root structs;
plain, fat and optional references; several results of every kind;
long-distance returns (`return … from`); defer; match and case-function
dispatch; elementwise math; print, str and format with user overloads;
`to_bytes` / `from_bytes` with upstream's image verifier; `embed_file`;
`reusable` and `reusable[]` pools and pool references, std's `slice_pool`
(the span tree both use ported from upstream's C runtime, see
[lib/](lib/README.md)); the debug build's checks (`-D GS_DEBUG=1`).

Not yet translated, by the backend's reason for the unsupported fixtures at
`-O2`:

| What | Fixtures |
|---|---|
| Threads and queues (`thread_spawn`, `thread_wait`, `qput`, …) | 34 |
| `extern fn`s: math (`sin`, `pow`, …), os (files, directories, arguments, clocks), `trim_memory`, `export fn` | 17 |
| Runtime defines other than `GS_DEBUG` (`GS_STACK_RESERVE`, `GS_MAX_STACKS`, …) | 11 |
| Values of a resizable ADT's variant type on their own (`fn f() -> Value.V`) | 1 |
| A frame larger than the shadow stack (upstream keeps fixed values over 4096 bytes on data stacks) | 1 |
| More data stacks than engines accept memories | 1 |

`extern fn`s are implemented by porting upstream's C runtime, where upstream
implements them itself; math by porting musl's libm, os functions over WASI
preview 1 as wasi-libc does. Threads are planned on Wasm threads and
wasi-threads.

### Running the modules

A module is a WASI preview 1 command. It uses Wasm 3.0's multi-memory (one
memory per data stack), so a host must enable it. A host must also give the
engine a deep stack: modules report runaway recursion themselves, with
upstream's "native call stack overflow", after a million activations, which
needs on the order of 256 MiB of engine stack. With wasmtime:

```sh
wasmtime run -W multi-memory=y -W max-wasm-stack=268435456 file.wasm
```

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
