# lib/

Standard library files goose-wasm uses in place of upstream's.

Some of upstream's standard library declares `extern fn`s that upstream's C
runtime implements. A WebAssembly module has no C runtime to link, so for
each such file goose-wasm keeps a version here in which those functions are
written in Goose, ported from upstream's C, and compiled by the Wasm backend
like the rest of the library. Everything else in the file is upstream's.

The files are compiled into the `goose-wasm` executable (CMakeLists.txt),
and the driver serves each one when the loader reads the file of the same
name from a standard library directory (`LibOverrides` in src/driver.cpp).
Upstream's tree is not modified.

Each file's header names the upstream files, functions and commit it derives
from, the original work and its license, and what was changed.

| File | Replaces | Ported from |
|---|---|---|
| slice_pool_internal.goose | stdlib/slice_pool_internal.goose | src/runtime_inline.h: gs_slice_pool_place, gs_slice_pool_free, gs_slice_pool_regrow, gs_spc_* |
