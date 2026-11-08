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

math.goose is the exception in what it ports: upstream's libm functions are
the platform C library's, so math.goose ports musl's libm instead (MIT; the
notices it carries are in its header and in NOTICE). `scripts/libm_check.py`
builds musl's C for those functions and checks the port's results against
it bit for bit, and regenerates the data tables math.goose holds.

| File | Replaces | Ported from |
|---|---|---|
| slice_pool_internal.goose | stdlib/slice_pool_internal.goose | src/runtime_inline.h: gs_slice_pool_place, gs_slice_pool_free, gs_slice_pool_regrow, gs_spc_* |
| math.goose | stdlib/math.goose | musl 1.2.5 src/math (its libm functions, MIT; see NOTICE); upstream's declarations and helpers otherwise |
| os.goose | stdlib/os.goose | src/runtime/runtime_os.h: gs_os_read_file through gs_os_random_u64 (the POSIX branch), over WASI preview 1 primitives the backend implements (gs_wasi_*) |
