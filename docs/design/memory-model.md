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
- **Memory 0** plays the role of upstream's handle id 0
  (`upstream/goose/docs/design/wasm.md` §4): static data, globals,
  address-taken locals (the shadow stack), resizable headers in frames, and
  the WASI transfer area. Data stacks, including the globals' dedicated
  stacks, are memories 1 and up.
- **WASI** (preview1) reads and writes only the memory exported as `memory`.
  Memory 0 holds a transfer area; I/O copies between it and a data stack.
- **Growth checks** go at every place upstream's `wasm.md` §2 lists: pushes,
  appends, `resize`, in-place construction (`str()`, `to_bytes()`, `print`,
  patched prefixes, frame objects, the `_er` twins, walkers) and the runtime
  helpers that write at a top with no bound (`gs_fmt_i64`, `gs_bld_append`,
  the slice pool's freelist).
- **Arithmetic on references** works on the whole `i64`, never on the low 32
  bits alone. A pool-relative reference stores `target - base + 1`; computed
  in 32 bits, a borrow could reach the memory index.

Accesses whose memory index the compiler can prove may skip the dispatch, and
a loop over one reference may dispatch once before the loop. Both are later
optimizations.

## Why not one linear memory

One linear memory needs no dispatch, but stacks become adjacent with no guard
between them: a missed capacity check corrupts the neighbouring stack
silently, where upstream would fault. Only the last region can grow. Safety
and per-stack growth were judged worth the dispatch cost.

## Relation to upstream's Wasm proposal

Upstream has an unimplemented proposal, `upstream/goose/docs/design/wasm.md`.
It weighs fixed partitions of one linear memory (§3) and stacks that relocate
on growth, addressed by handles (§4), and rules out multi-memory (§5) for two
reasons:

1. A load names its memory as an immediate, while the stack is chosen at run
   time (`gs_sp + k`, a fat reference's `stk`). The `br_table` dispatch
   answers this.
2. C cannot target several memories. This backend emits Wasm directly, so it
   does not apply.

Our reference encoding matches upstream's handle (stack id in the high bits,
offset in the low bits, id 0 for plain addresses), so layouts stay as on the
64-bit hosts the C backend targets. Where upstream decodes a handle with a
base table (`bases[h >> 32] + (uint32_t)h`, no branch), we dispatch with
`br_table`; the two are to be compared by measurement.

## Stability compared with heap or GC based designs

Goose is built on having no heap, no GC, and memory that never moves. This
design keeps those properties on Wasm better than the alternatives:

| | multi-memory + `memory.grow` | heap blocks (upstream §4) or GC |
|---|---|---|
| Pauses | none: nothing to collect | GC pauses, at the engine's discretion |
| References | stay valid: growth never moves a stack | a stack moves on growth; a decoded pointer held across it goes stale, which upstream names its main source of bugs |
| Cost of growth | `memory.grow`, no copy | the whole stack is copied into a block twice the size |
| Memory use | what is used | up to 2-3x the live data at peak (upstream's estimate), plus fragmentation |
| Overrun | traps; other stacks are untouched | a missed check corrupts the neighbouring block |
| Placement | static, as upstream's | depends on allocator or collector state |

Known weaknesses:

- **Address space.** Engines reserve 4 GB or more per 32-bit memory. A few
  dozen memories are fine on 64-bit desktops; hosts with little address
  space (mobile browsers) may refuse them.
- **No giving memory back.** Wasm cannot shrink or discard memory
  (`memory.discard` has not shipped), so upstream's `trim_memory()` gives
  nothing back (it returns true) and a stack keeps its peak size.
- **Performance varies with data placement.** `br_table` is cheap when
  consecutive accesses hit one memory and costs a mispredicted branch when
  the memory index changes: 7.6 vs 14.8 ns per node on V8 with 32 memories.
  A base table or dispatch hoisted out of loops may reduce this.
- **Engine-internal copies.** Where an engine has not reserved address space
  ahead, `memory.grow` may copy the memory internally. Offsets stay the same,
  so behaviour does not change, but that growth is slow.
- **Workers.** Their number is decided at run time without a fixed bound
  (spec §11.2), while a module declares a fixed number of memories. Each
  worker is therefore an instance of the module (wasi-threads), with
  memories of its own ([threads.md](threads.md)).
- **Memories per module.** The Wasm spec sets no limit, but engines do:
  wasmparser (wasmtime, wasm-tools) and V8 accept 100. A program needing
  more than 99 data stacks cannot have one memory each. Upstream's tests
  have one such program (`optimizer/bce_place_flood.goose`, 240 resizable
  globals), which is reported unsupported. Should real programs need more,
  stacks that never hold a reference target could share a memory, or the
  base table of upstream's `wasm.md` §4 could serve the overflow.

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
with more run-to-run noise.

### br_table against upstream's base table

`basetable` keeps every stack in one linear memory and decodes a reference
as `bases[index] + address` (upstream `wasm.md` §4), with no index check and
no isolation between stacks. Same machine, 2026-10-31; V8 figures are the
range over three runs, wasmtime one run.

| ns per operation | walk | walk1 | scan | 4 KiB copy |
|---|---|---|---|---|
| V8, base table, 4 stacks | 6.0-7.0 | 5.4-6.6 | 0.46-0.55 | 28-34 |
| V8, br_table, 4 stacks | 4.4-5.1 | 4.3-5.5 | 0.57-0.75 | 31-41 |
| V8, base table, 32 stacks | 16.6-18.8 | 5.3-5.6 | 0.46-0.50 | 70-78 |
| V8, br_table, 32 stacks | 20.4-25.6 | 7.9-9.7 | 1.5-1.9 | 42-64 |
| wasmtime, base table, 4 stacks | 9.33 | 9.63 | 1.83 | 28.0 |
| wasmtime, br_table, 4 stacks | 7.54 | 7.25 | 1.65 | 29.3 |
| wasmtime, base table, 32 stacks | 16.19 | 9.90 | 2.06 | 79.1 |
| wasmtime, br_table, 32 stacks | 15.98 | 6.56 | 2.16 | 69.7 |

Neither wins outright. With 4 stacks `br_table` is as fast or faster on both
engines. With 32, V8 favours the base table (walk1 about 0.6x, scan about
0.3x) and wasmtime favours `br_table` (walk1 6.6 against 9.9 ns). The base
table's 32-stack copies are slow on both engines, for reasons not yet
investigated. Since the base table here also gives up the index check and
the isolation, `br_table` stays. Codegen keeps reference decoding in one
place so that the strategy can be swapped per engine. Not yet measured: the engines' limit on memories
per module. Do not probe it with thousands of memories: each 32-bit memory
reserves 4 GB or more of address space, and such a run was followed by a
Windows bugcheck (see `experiments/multimem/limits.mjs`).
