# Memory model: data stacks on Wasm multi-memory

Status: adopted (2026-10-31).

## Upstream model

Upstream Goose (`upstream/goose/docs/goose_spec.md` §1.2, §1.3, §3.8, §10.3)
places every value in one flat 64-bit address space that the OS provides:

- Each data stack is its own reservation (2 GB by default, `--stack-reserve`),
  committed on touch, with a 1 MB `PROT_NONE` guard gap after it. A program
  uses a handful to a few dozen stacks (4 to 26 in `samples/`, 31 in
  `test/goose_in_goose`); `GS_MAX_STACKS` is 1024 per thread program.
- Growth is a pointer bump with no capacity check: the guard gap catches an
  overflow, and the fault handler reports it (`gs_stack_overflow`).
- A reference `T&` is one machine address. Reads and writes through it never
  ask which stack it points into; the address alone decides.
- Which stack a value is pushed onto is static (§10.3), though the C backend
  passes it as a hidden runtime argument (`gs_sp`, `gs_rref.stk`).

## Decision

Each data stack is its own Wasm memory (multi-memory, Wasm 3.0).

- **Reference representation.** An `i64`: the upper 32 bits are the memory
  index, the lower 32 bits the address within that memory. A reference keeps
  upstream's 8-byte size and layout. `.==` compares the whole `i64`; null is
  0, so address 0 of memory 0 is never handed out.
- **Access through a reference** dispatches on the memory index with
  `br_table` to a `load`/`store` that names the memory as its immediate. An
  index outside the table traps (`unreachable`).
- **Copies** (`memory.copy`) dispatch on the (destination, source) pair.
- **Push and growth** target a stack known at compile time, so they name the
  memory directly and need no dispatch. Each memory grows on its own with
  `memory.grow`, close to upstream's reserve-then-commit regions.
- **Overflow isolation.** A stack cannot spill into another: an access past a
  memory's size traps, as upstream's guard gap faults. Capacity checks before
  growth still produce upstream's diagnostic.
- **WASI** (preview1) reads and writes only the memory exported as `memory`.
  Memory 0 holds a transfer area; I/O copies between it and a data stack.

Accesses whose memory index the compiler can prove may skip the dispatch, and
a loop over one reference may dispatch once before the loop. Both are later
optimizations.

## Why not one linear memory

One linear memory needs no dispatch, but stacks become adjacent with no guard
between them: a missed capacity check corrupts the neighbouring stack
silently, where upstream would fault. Only the last region can grow. Safety
and per-stack growth were judged worth the dispatch cost.

## Measurements

`experiments/multimem` (2026-10-31; Ryzen AI 7 PRO 360, Node 24.14.1 / V8,
wasmtime 50.0.1). ns per operation; `walk` changes memory at every node (the
worst case), `walk1` follows nodes that all sit in one memory, as a pool's do.

| V8 | walk | walk1 | scan | 4 KiB copy |
|---|---|---|---|---|
| single memory | 6.60 | 5.73 | 0.50 | 44.4 |
| br_table, 4 memories | 7.09 | 5.67 | 0.72 | 38.0 |
| br_table, 32 memories | 14.84 | 7.56 | 1.34 | 43.9 |
| call_indirect, 32 memories | 27.11 | 12.44 | 2.73 | 61.3 |

wasmtime shows the same ordering (`br_table` well ahead of `call_indirect`),
with more run-to-run noise. Not yet measured: the engines' limit on memories
per module. Do not probe it with thousands of memories: each 32-bit memory
reserves 4 GB or more of address space, and such a run was followed by a
Windows bugcheck (see `experiments/multimem/limits.mjs`).
