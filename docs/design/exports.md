# Export fns called from a host

Status: adopted (2026-11-08). Builds on [memory-model.md](memory-model.md).

References: `spec` is `upstream/goose/docs/goose_spec.md` (§7.10, §7.11),
and upstream's wrappers are `CodeGen::EmitExports` and `EmitProgramInit` in
`upstream/goose/src/codegen_emit.h`.

## Upstream model

An `export fn` is an ordinary Goose function for which upstream emits a C
wrapper under its C symbol. Built with `--header out.h`, the C is for a C
host: it has no `main`, a program may leave `fn main()` out, and the header
declares `goose_init(argc, argv)` and the exports. The host calls
`goose_init` once. It initializes the runtime and the globals, marks the
program initialized, and runs `fn main()` if there is one; a second call
does nothing. Each wrapper aborts with `goose_init must be called before
exported functions` if the program is not initialized, then calls the Goose
function. A wrapper is not thread-safe: the host calls the exports from the
thread that called `goose_init`.

What crosses (spec §7.11, §7.10): scalars; flat fixed-size values (structs,
fixed arrays, fixed-mode ADTs) by value, as their packed C types; `T&` as a
pointer; `T[:]` as `{ data, len }`; `T[>..]&` (not of `u8`) as the
resizable's header and stack. A result is a scalar or a flat fixed-size
value by value, or a `const u8[:]`, which comes back as a pointer, with its
length written through an extra trailing `int64_t *` (`exported slice
length pointer is null` if that is null).

## Decision

`goose-wasm --header out.h` builds the module for a host, as upstream builds
its C for one: a **WASI reactor**, with no `_start`. The checker runs in
upstream's library mode, so `fn main()` is optional and an export is
required. Without `--header` the module is a command as before, and an
export is callable only from the program itself.

The module exports:

| Export | What |
|---|---|
| `goose_init()` | upstream's `goose_init`: the runtime, the globals' initializers, then `fn main()` if there is one; once. It takes no arguments: the program's arguments are WASI's (`os::arg`), which the host gives the instance. |
| each export fn | under its C symbol, with the signature below |
| `memory` | memory 0, which every address below is in |
| `gs_alloc(size) -> addr` | a block of `size` bytes in memory 0, 0 when there is no room |
| `gs_free(addr)` | gives a block back; 0 is nothing |

`gs_alloc` and `gs_free` are new: a C host passes pointers to its own
memory, but a Wasm host can only pass addresses in the module's, and needs
somewhere to put what it passes. They carry upstream's reserved `gs_`
prefix, so no export fn can take their names (spec §7.11). The heap is the
size-class allocator workers use in the shared memory (`heap.h`), here in
memory 0 past the shadow stack, growing memory 0.

### Signatures

The wrapper's Wasm signature follows wasm32's C ABI where that ABI passes
a value indirectly, and passes the rest as Wasm values:

- **Scalars** as themselves: `i32` for `bool` and integers up to 32 bits,
  `i64`, `f32`, `f64`. A narrow integer is brought into its type's range,
  as C's conversion to the parameter type would leave it, and a `bool` is
  1 when nonzero.
- **`T&`** as the `i32` address of its target.
- **`T[:]`** as two parameters: the `i32` address of its elements and their
  `i64` count.
- **A fixed value** as the `i32` address of its bytes, laid out as
  upstream's packed C type (C.2). The wrapper copies them into its frame,
  so the host's bytes are never changed, as by-value passing leaves them.
- **A fixed result** through a leading `i32` parameter: the address the
  wrapper copies it to, as wasm32's C ABI returns a struct.
- **A `const u8[:]` result** as the `i32` address of its bytes, with a
  trailing `i32` parameter, the address of the `int64_t` that receives
  their count (upstream's `out_len`).

`--header` writes these signatures as C declarations, with `gs_addr` (a
`uint32_t`) for an address and each export's Goose signature in a comment.
They are a description of the module's interface for a host's own
bindings, not declarations a C program can link against, which upstream's
header is.

### Addresses

Every address a host passes or receives is in memory 0. A reference is an
`i64` (memory index << 32 | address, see memory-model.md), so one into
memory 0 is the address itself: the wrapper widens what the host passes and
the Goose function reads and writes the host's bytes in place.

A returned slice may be on a data stack, in a memory of its own that the
host cannot address. Then the wrapper copies its bytes to a block of the
heap and returns that; the block lives until the same export is called
again, which frees it. A slice in memory 0 (a string literal, a global
fixed array) is returned where it is, as upstream returns it.

### What is not translated

- **`T[>..]&` parameters**: upstream passes a resizable as its header and
  its `gs_stack`, which a C host sets up with `goose.h`; a Wasm host has no
  way to make a data stack. Reported as unsupported.
- **Workers and queues** in a module built for a host: a wasi-threads
  module exports its shared memory as `memory` (WASI reads and writes it),
  and its instances are started by the host's wasi-threads, which reactors
  are not. Reported as unsupported.
- **`GS_STACK_STATS`** prints nothing, as upstream's library build prints
  nothing: upstream reports as its own `main()` ends, which a host replaces.

## Testing

`scripts/hosttest.py` runs `tests/host/`: each test is a Goose program and
two hosts making the same calls, a C host linked with upstream's
`--standalone --header` build and a JS host running the module under
Node's WASI as a reactor. Both must print the same and exit alike.

## Rejected

- **Exports in a command module.** A WASI command's exports are not to be
  called after `_start` returns (wasi-libc runs its destructors there), and
  upstream's own exports need the host to start the program, which is
  `goose_init`, not `main`.
- **Running `goose_init` from `_initialize`.** Engines call a reactor's
  `_initialize` when they instantiate it, so `fn main()` would run before
  the host chose to start the program. Upstream leaves that to the host.
- **Copying every returned slice.** A slice in memory 0 is returned in
  place, with upstream's lifetime; only one the host could not otherwise
  read is copied.
