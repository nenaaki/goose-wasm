# Architecture: upstream front end, our Wasm backend

Status: adopted (2026-10-31).

## Shape

`goose-wasm` is one C++ translation unit, `src/driver.cpp`, as upstream's
compiler is (`upstream/goose/src/main.cpp`). It includes upstream's headers
unmodified, in upstream's order, and adds a backend that writes Wasm where
upstream writes C:

| Phase | Code | Source |
|---|---|---|
| Parse, resolve, typecheck, optimize, BCE | `ParseProgram`, `ResolveTypeNames`, `TypeCheckProgram`, `Optimizer`, `BCE::RunAll` | upstream, unmodified |
| Program loading (`ParseProgram` and helpers) | `src/upstream_copy/main_parse.h` | upstream `main.cpp`, copied verbatim |
| Wasm encoding | `src/wasm/binary.h`, `src/wasm/module.h` | ours |
| Code generation | `src/wasm/backend.h` | ours |

Also compiled from upstream unmodified: `process.cpp`, `progress.cpp`,
`version.cpp`, and `shaderc.c` with `third_party/cute_spirv` (zlib or public
domain).

## What is left out, and why

- **`jit.h` and TinyCC.** TinyCC is LGPL 2.1; this project neither links nor
  redistributes it. Nothing upstream includes before `jit.h` depends on it,
  and `build.h`, `packages.h` and `native.h` after it do not either.
- **`main.cpp`** includes `jit.h`, so it is not compiled. Its program loader
  (lines 61-488 at upstream 2ed1a63: `DirOf` through `ParseProgram`) is
  copied instead.
- **The language server and the formatter.**

## Copied upstream code

Every copy lives under `src/upstream_copy/`. In each file the upstream lines
sit unchanged between `BEGIN UPSTREAM COPY` and `END UPSTREAM COPY` markers;
the header above them names the upstream file, lines and commit, the
original work and its license, and what the file adds around the copy.
`python scripts/upstream_copy.py check` fails when a copy no longer matches
upstream, and `update` takes the block again after the submodule moves.

Generated headers: `goose_version.h` comes from upstream's own
`cmake/version.cmake`. `builtin_packages.h` is written by our
`CMakeLists.txt` in the form upstream's `cmake/package.cmake` writes, since
that function targets upstream's `goose` target and tree; it holds upstream's
`src/blessed.txt` and no compiled-in packages.

## Walking the program

Nodes carry a `NodeKind` (`ast.h`, 38 kinds) and public fields, so the
backend walks each live specialization's annotated body (`FnSpec::body`)
with a switch on `nkind`, without adding virtuals to upstream's `Node`.

## What the backend must reproduce

Much of upstream's semantics is decided inside its C codegen while it emits
text, not recorded on the tree (implementation.md §6):

- stack assignment (`AllocStk`, `SpTop`, §6.2) and scope watermarks;
- `defer` and exit restores;
- destinations and in-place construction (§6.5), result placement (§7.3);
- the calling convention (§6.4), walkers (§6.9), `print` rendering.

The Wasm backend reimplements these rules (about 15k lines of
`codegen*.h`), following implementation.md §6 and checked by running
upstream's tests through both compilers and comparing output. This is the
main cost of the project and its main risk to fidelity.
