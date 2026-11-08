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

As of 2026-11-08, of the 749 fixtures `scripts/difftest.py` runs (upstream's
test suite and this repository's `tests/`), with no wrong output, wrong exit
or invalid module at either level:

| Level | Pass | Unsupported |
|---|---|---|
| `-O2` | 748 | 1 |
| `-O0` | 748 | 1 |

Translated: scalars and fixed aggregates, large ones on data stacks as
upstream keeps them; strings, slices, fixed, limited and resizable arrays;
variable-size values (`T[]`, `varint`, variable structs and variable-mode
ADTs) as packed images, a returned named local built where the result goes
(§7.3); structs ending in a resizable array (frame objects, with or without a
variable-size prefix) and ADTs with such a variant, as values, results,
parameters and in whole assignment; fixed and varint-width relative
references and `in pool` references; root structs; plain, fat and optional
references; several results of every kind; long-distance returns
(`return … from`); defer; match and case-function dispatch; elementwise
math; print, str and format with user overloads; `to_bytes` / `from_bytes`
with upstream's image verifier; `embed_file`; math's libm functions
(`sin`, `pow`, …, at both widths), ported from musl's libm (see
[lib/](lib/README.md); `scripts/libm_check.py` checks the port against musl
bit for bit); `reusable` and `reusable[]`
pools and pool references, std's `slice_pool` (the span tree both use ported
from upstream's C runtime, see [lib/](lib/README.md)); the runtime defines
`GS_DEBUG`, `GS_STACK_RESERVE` (and `--stack-reserve`), `GS_STACK_GAP`,
`GS_STACK_BUDGET`, `GS_MAX_STACKS` and `GS_STACK_STATS`; workers and typed
queues (`thread_spawn`, `thread_spawn_contained`, `thread_wait`, `qput`,
`qget`, `qpoll`, `qlimit`, `hardware_threads`) on Wasm threads and
wasi-threads ([docs/design/threads.md](docs/design/threads.md)); std's `os`
(files, directories, standard streams, arguments, environment, clocks,
`sleep_ms`, `random_seed`) ported from upstream's C runtime to Goose over
WASI preview 1, behaving as wasi-libc does (see [lib/](lib/README.md)), and
`trim_memory`; `extern fn`s bound to upstream's os runtime symbols, to C's
`sqrt`, `fabs` and `fmod`, and to an `export fn`'s symbol.

The one fixture left unsupported needs more data stacks than engines accept
memories (`optimizer/bce_place_flood.goose`), a limit of this backend's as
`GS_MAX_STACKS` is upstream's (below).

Not translated: an `extern fn` bound to any other C function (upstream
links the platform's C library; a module has none), dynamic externs
(`extern` blocks naming shared libraries), and `export fn` as an export of
the module: an export is callable from the program itself, as upstream's
wrapper is.

### Running the modules

A module is a WASI preview 1 command. It uses Wasm 3.0's multi-memory (one
memory per data stack), so a host must enable it. A host must also give the
engine a deep stack: modules report runaway recursion themselves, with
upstream's "native call stack overflow", after a million activations, which
needs on the order of 256 MiB of engine stack. A program using `os`'s files
sees the directories the host preopens, `.` as its working directory. With
wasmtime:

```sh
wasmtime run -W multi-memory=y -W max-wasm-stack=268435456 --dir . file.wasm
```

A program that spawns workers or uses queues is a wasi-threads module. It
imports a shared memory and needs the threads proposal, and exception
handling if it spawns contained workers. wasmtime removed wasi-threads in
version 47; these modules run on wasmtime 46:

```sh
wasmtime run -S threads=y -W threads=y,shared-memory=y,exceptions=y,multi-memory=y -W max-wasm-stack=268435456 --dir . file.wasm
```

## Constraints

Where a module behaves differently from upstream's native build, or needs
something of its host, beyond what is not translated yet (above):

- **Host features.** Multi-memory, and an engine stack of about 256 MiB
  (above). Engines reserve 4 GB or more of address space per 32-bit memory,
  and a module declares one memory per data stack, so a program with many
  stacks reserves much address space, though it commits only what it uses
  ([docs/design/memory-model.md](docs/design/memory-model.md)).
- **Number of data stacks.** Engines accept about 100 memories in a module
  (wasmparser and V8), one of them the runtime's own, so a program may need
  at most 99 data stacks at once, its globals' included (98 with workers or
  queues, whose shared memory is one more; a worker's stacks are in the
  same memories). A program needing more is reported as unsupported at
  compile time, as upstream reports one past `GS_MAX_STACKS` (1024 by
  default).
- **Size of a data stack.** A stack holds at most its reservation:
  `GS_STACK_RESERVE`, 2 GB by default as upstream's, of which whole 4 KB
  pages are usable as upstream's runtime makes them. Past it a program
  aborts with upstream's data stack overflow message. A 32-bit memory holds
  at most 4 GB, so a larger reservation acts as 4 GB, and the message says
  so. A stack's memory never shrinks: WebAssembly cannot give pages back.
- **Native stack.** Upstream's native call stack is an 8 MB shadow stack
  in memory 0 here, plus the engine's own stack; recursion past a million
  activations, or past the shadow stack, aborts with upstream's "native
  call stack overflow". As upstream does, fixed values over 4096 bytes live
  on data stacks except in recursive functions, whose frames hold them.
- **Runtime defines.** Given as `-D NAME=VALUE` (or `--stack-reserve SIZE`)
  when compiling, as upstream takes them: `GS_DEBUG`, `GS_STACK_RESERVE`,
  `GS_MAX_STACKS` and `GS_STACK_STATS` act as upstream's; `GS_STACK_GAP` and
  `GS_STACK_BUDGET` are accepted and only appear in `GS_STACK_STATS`'s
  report (a memory needs no guard gap, and the budget sizes worker threads,
  which are not translated yet). Any other define is reported as
  unsupported.
- **Files and the environment.** A module sees the directories its host
  preopens: a relative path names a file under the one preopened as `.`,
  the working directory, as wasi-libc resolves it (with wasmtime,
  `--dir .`). It sees the environment variables and arguments the host
  gives it. `resource_dir()` is never known: WASI has no executable to
  locate. `write_file_atomic` cannot give the new file its predecessor's
  permissions, nor sync the directory a rename lands in. `trim_memory()`
  gives nothing back (above) and returns true: no request it makes can
  fail.
- **Threads.** Workers need a wasi-threads host (wasmtime 46, not later
  wasmtimes; above). Each worker is an instance of the module, with memories
  of its own, so each reserves the address space of the module's data
  stacks ([docs/design/threads.md](docs/design/threads.md)).
  `hardware_threads()` is 1: WASI cannot tell how many processors there are,
  and wasi-libc's `sysconf` answers 1 too.
- **Floating point.** math's libm functions are musl's, where upstream calls
  the platform's C library, so a result may differ from a native build's in
  the last bit (each libm's own accuracy). `mul_add` rounds twice, as
  upstream's does on builds without FMA (the spec leaves it to the
  implementation, §7.12). WebAssembly leaves the sign and payload of a NaN
  produced by arithmetic unspecified.

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
