# Value representation on Wasm

Status: proposed (2026-10-31). Builds on [memory-model.md](memory-model.md).

Every Goose value keeps upstream's byte layout. Only where a value lives and
how an address is spelled change. Upstream's layouts are those of its C
backend on 64-bit little-endian hosts (spec Appendix C, implementation.md
§6.1), and Wasm is little-endian, so a struct, an array or a serialized
image has the same bytes here as natively.

References below: `spec` is `upstream/goose/docs/goose_spec.md`, `impl` is
`upstream/goose/docs/implementation.md`.

## 1. Addresses

| Upstream (C) | Here |
|---|---|
| A pointer: 8 bytes, 0 is null | An `i64` handle, (memory index << 32) \| address, 0 is null |
| One flat address space | Memory 0, and one memory per data stack |

- **Memory 0** holds everything that is not on a data stack: static data
  (string literals, `const` globals), globals, the shadow stack (below), the
  runtime's fixed area and the WASI transfer area. It plays upstream's
  handle id 0 (upstream `docs/design/wasm.md` §4). Its address 0 is never
  handed out, so the handle 0 is null.
- **Memories 1..N** are the data stacks: the thread program's indexed stacks,
  then the globals' dedicated stacks (impl §6.2, §6.3).
- A handle is 8 bytes, as a pointer is, so a reference field, a slice and
  every layout that holds them keep their size. Arithmetic on handles
  (`base + i * size`, field offsets, differences within one stack, a pool's
  range test) works on the whole `i64` and never carries into the index:
  no region reaches 4 GB.
- Loads and stores through a handle dispatch on its index with `br_table`
  (memory-model.md). A load or store whose memory the backend knows names
  it directly.

## 2. Where values live

| Kind (spec) | Size class | Upstream | Here |
|---|---|---|---|
| Integers, floats, bool (§3.1) | fixed | C locals, registers | Wasm locals; memory 0 when its address is taken |
| Fixed struct, `T[k]`, `T[..k]`, fixed-mode ADT (§3.2-3.5) | fixed | C locals (`#pragma pack(1)`) | the shadow stack in memory 0 |
| Fixed values over 4096 bytes (§10.3) | fixed | a data stack | a data stack, by the same rule |
| Variable values: `T[]`, `T[..]`, varint structs, variable-mode ADTs (§3.3, §3.6) | variable | data stack, `uint8_t *` | data stack, handle |
| `T[>..]`, `T[>..<]` (§5) | resizable | header `gs_rhdr` in the frame, elements at the top of a data stack | header on the shadow stack, elements at the top of a data stack |
| Globals (impl §6.3) | any | `gs_globals_t` members | memory 0, at static offsets |
| String literals (§3.7) | fixed | `static const` | static data in memory 0 |

A scalar local goes to memory only when something takes its address: a
`&x`, including the `&` the checker inserts where a by-reference parameter
takes a fixed value (spec §4.1, `Unary::synth`), a `.=` binding, or `for &x`.
The backend finds these by scanning a body before emitting it.

**The shadow stack** is a region of memory 0 with a stack pointer in a Wasm
global. A function claims its frame on entry and releases it on every exit.
Wasm locals have no address, so the shadow stack does what C's native stack
does for address-taken and aggregate locals, as clang's does for C on Wasm.
Running out of it is upstream's "native call stack overflow" runtime error,
checked at frame entry. A recursion deep enough to exhaust the engine's own
stack traps in the engine instead, with no Goose message; upstream reports
that case too (impl §7), so this is a known difference.

## 3. Representations

All sizes are upstream's (`codegen_types.h` `PadAlign`, `CT`; spec
Appendix C).

| Value | Bytes | Fields |
|---|---|---|
| Plain `T&`, `T?` | 8 | handle; null 0 |
| `T&` to a resizable | 16 | `hdr` handle (the header), `stk` (the stack's index as an `i64`, where upstream holds a `gs_stack *`) |
| Slice `T[:]` | 16 | `data` handle, `len` `i64` |
| Resizable header `gs_rhdr` | 16 | `base` handle, `len` `i64` |
| Self-relative `T&<uN>` | N bytes | signed offset from the field's own address |
| Pool-relative `T&<uN in pool>` | N bytes | `(target - base) + 1`, 0 for null |
| Fixed struct | sum of fields | packed, no implicit padding; `pad`/bare `pad` as upstream; an empty struct is 1 byte |
| ADT, fixed mode | tag (u8 up to 256 variants, else u16) + largest payload | as upstream |
| varint | 1-10 bytes | LEB128, zigzag for signed |

Unaligned accesses are allowed and correct in Wasm; every load and store
uses alignment 0, as packed layouts need.

Relative references stay offsets within one stack, so they move with nothing
and need no change. Taken from a handle, the base keeps the target's memory
index; decoding adds the offset to the whole `i64` (memory-model.md).

## 4. Data stacks

- **Which stack.** Spec §10.3 assigns stack indices statically per
  specialization context; upstream instead passes a hidden base `gs_sp` at
  run time (impl §6.2), which the spec allows. This backend does as upstream
  does (changed 2026-10-31 from emitting each function once per base): every
  function takes the base as its first parameter, allocates indices relative
  to it as upstream's `AllocStk` does, and calls with the base plus the
  indices in use (`SpTop`), none across a call into its own recursive cycle.
  It keeps code generation shaped like upstream's and emits each function
  once. The cost is a `br_table` in the helpers that grow a stack; per-base
  specialization remains a possible optimization.
- **Top and limit.** A table in memory 0 holds each stack's top (an address
  in its memory) and its memory's size, indexed by stack, so reading or
  moving a top needs no dispatch.
- **Count.** N is the most indices any path of calls from `main` holds at
  once, computed after every function is emitted; the module then declares
  N stack memories, each starting empty.
- **Growth checks** go at every place upstream's `wasm.md` §2 lists. A push
  that does not fit grows the memory with `memory.grow`, by at least
  doubling, up to the configured reserve; past it, it reports upstream's
  data stack overflow message, with this platform's limit in place of the
  reservation.
- **Scope exit** restores each local's base as the stack's top, as upstream's
  watermarks do (impl §6.2). Memory is never shrunk: Wasm cannot give pages
  back.

## 5. Functions

One Wasm function per specialization, whose first parameter is its stack
base. The calling convention follows upstream's (impl §6.4, spec C.3) with
handles for pointers:

| Parameter or result | Upstream (C) | Here |
|---|---|---|
| Scalar | by value | Wasm parameter or result |
| Fixed aggregate | by value up to 4096 bytes, else a pointer to a copy | a handle to a copy the caller makes on its shadow stack |
| `T&` | pointer | `i64` handle |
| Slice | `data`, `len` | two `i64` parameters |
| Resizable `T&` | `hdr`, `stk` | `i64` handle, `i32` stack index |
| Several results | out parameters | Wasm multi-value for scalars; fixed aggregates through a destination handle |
| Nonfixed result | built on the destination stack (`gs_dst`, spec §7.3) | built on the destination stack, passed as upstream passes it |

`return ... from` (spec §7.9) and inlined bodies are branches out of
enclosing blocks where the target is in the same function. Across functions
it is a propagation flag, as upstream's channel (impl §6.6).

## 6. Not decided here

- **Floats.** Values are `f32`/`f64` locals; `print` must render them as
  upstream's runtime does, which needs its formatting algorithm.
- **Threads.** Each worker needs its own set of stack memories while a module
  declares a fixed number; one instance per worker is the likely shape
  (memory-model.md, Known weaknesses).
- **`extern fn`** and the `ffi` module: host imports, with handles converted
  to host addresses at the boundary.
- **The debug build** (`GS_DEBUG`): overflow and `as` range checks.
- **The shadow stack's and data stacks' sizes**, and how `--stack-reserve`
  and `GS_STACK_RESERVE` map onto them.
