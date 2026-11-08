# Threads and queues on wasi-threads

Status: adopted (2026-11-08). Builds on [memory-model.md](memory-model.md).

References: `spec` is `upstream/goose/docs/goose_spec.md` (§11.2), `impl`
is `upstream/goose/docs/implementation.md` (§6.8, §7), and upstream's
runtime is `upstream/goose/src/runtime/runtime_threads.h`.

## Upstream model

Workers share no mutable memory (spec §11.2). A `thread_fn` and everything
it calls form a separate thread program with its own data stacks and its
own copy of every global it uses, taken from the spawning instance at
`thread_spawn`. Arguments and queue elements are flat values, copied in and
out. Upstream runs each worker on a native thread with a fresh block of
stacks and a fresh globals instance, and keeps workers and typed queues in
`malloc`'d records under a mutex and condition variables.

## Decision

Wasm threads (shared memory, atomics) with **wasi-threads**: each worker is
a new instance of the module, which the host starts on a thread of its own
and enters at the exported `wasi_thread_start`.

- **What is a thread's own.** Everything the module defines: memory 0
  (static data, globals, the shadow stack), the data stacks' memories, and
  Wasm globals. A worker's instance is laid out exactly as main's, so the
  memories are the thread program's stack block (upstream `gs_stks`) and
  memory 0 its globals instance (`gs_gl`), and no address needs translating
  between them. A module declares memories for the most any one thread
  program needs: the globals' dedicated stacks, then, for main, its indexed
  stacks, and for a worker, the stacks its entry thunk fills followed by its
  program's indexed stacks.
- **What is shared.** One imported shared memory (`env.memory`, also
  exported as `memory`), present only in a program that spawns workers or
  uses queues. It holds the runtime's state at fixed addresses, then a heap
  for spawn packets, queue nodes, abort messages and each thread's WASI
  area (`src/wasm/threads.h`). An imported memory takes Wasm index 0, so
  the program's own memories move one index up in the binary (`memshift`);
  references keep their encoding, memory index 0 meaning memory 0.
- **WASI.** wasi-threads hosts do WASI I/O through the shared memory, so a
  thread stages what it writes in its own area there.
- **Synchronization.** One lock over the shared state: Drepper's futex
  mutex on `memory.atomic.wait32` / `notify`. Each condition variable of
  upstream's is a sequence word, bumped and notified under the lock; a
  waiter reads it under the lock, unlocks and waits for it to change. A
  timed `qget` waits with a timeout on the monotonic clock
  (`clock_time_get`).
- **Heap.** Size classes of 16 << c bytes with a free list each, grown from
  the end with `memory.grow`, all under the lock. A request the shared
  memory cannot hold ends the program with upstream's message for what was
  being allocated.

### Spawning

`thread_spawn(worker, args...)` and the queue builtins become calls of
runtime functions the backend makes, with the Goose signature of what they
take and give (`LowerThreads`). Their arguments and results then follow the
calling conventions every call does, whatever the type, and their bodies
(`SynthBody`) deal only in places:

- the spawn packet holds each argument's image, then each thread global's
  copy, in the shared heap; the runtime records the worker and asks the
  host for a thread;
- the worker's thunk (`EmitThunk`) unpacks the arguments as the worker's
  parameters take them (scalars as values, fixed aggregates in its frame or
  on a stack, variable-size and resizable values on stacks of their own),
  fills its globals, and calls the worker with its indexed stacks after
  those;
- a global is copied verbatim: the bytes of its place in memory 0 and the
  used bytes of each stack it has to itself (a resizable's, a variable-size
  one's, a pool's freelist). Since the worker's instance is laid out alike,
  handles inside the copy, into its own stacks or static data, stay valid.

Like upstream's `BytesTemp`, a spawn builds its arguments' copies on one
temporary stack, released after the call, so that `--stacks` counts and
`GS_STACK_STATS` reports agree with upstream's.

### Images

What crosses threads, in a packet or a queue node: a scalar's or fixed
aggregate's bytes; a variable-size value's image; a resizable array's
count, its elements' byte size and its elements; a frame object, or a
resizable struct with a variable-size prefix, as its tail's count, its
prefix image's size, its place's bytes before the tail's header, the
prefix image and the elements. A missed `qpoll` gives the zero value,
writing the all-zero image of upstream's `MinBytes(t, true)`.

### Ending

A worker's return frees its packet and WASI area and publishes its state
(running, returned, aborted) in a byte per issued id, which `thread_wait`
reads. A contained worker's abort throws a Wasm exception (exception
handling, Wasm 3.0) that its `wasi_thread_start` catches; the message, the
text after `goose runtime error: `, waits in the shared heap for the first
`thread_wait` that asks for it. Only a program that spawns contained
workers uses exceptions. Any other abort, `exit`, a data stack overflow
(which names the worker, as upstream's `gs_overflow_text` does) and a native
stack overflow end the process, as the host ends it for a `proc_exit` or a
trap on any thread. When `main` returns, the host's process ends, workers
included.

### hardware_threads()

WASI cannot tell how many processors there are, and wasi-libc's `sysconf`
answers 1, which `hardware_threads()` follows (os functions behave as
wasi-libc's, decided 2026-11-07). The thread cap upstream computes from
`GS_STACK_BUDGET` is at least 1, so it never lowers that, but appears in
`GS_STACK_STATS`'s report as upstream's does.

## Hosts

wasmtime removed wasi-threads in version 47 (RFC
`wasmtime-remove-wasi-threads`, 2026-07), leaving it to WASIp3's cooperative
threads and, later, shared-everything-threads. Modules of programs with
threads therefore run on wasmtime 46 (`-S threads=y -W
threads=y,shared-memory=y,exceptions=y,multi-memory=y`), the project's
reference host for them (decided 2026-11-08); every other module still runs
on current wasmtime. Other wasi-threads hosts need multi-memory and, for
contained workers, exception handling.

The modules are unchanged for a second host, `scripts/wasihost.mjs` on
Node.js (added 2026-11-08), so that testing them does not hang on one old
wasmtime; `scripts/difftest.py --host node` runs every fixture on it.

- **Threads.** The program runs on a Worker, and `wasi.thread-spawn` asks
  the main thread for another, which instantiates the same compiled module
  with the same shared memory and calls `wasi_thread_start`. Spawns go
  through the main thread because a Worker's own Workers end with it, and a
  Goose worker outlives the thread that spawned it. Thread ids come from a
  counter in a `SharedArrayBuffer`, so `thread-spawn` returns at once.
- **WASI.** The host implements the preview1 functions the backend imports,
  over the memory the module exports (the shared one in a threaded module),
  with `.` preopened and no environment, as difftest's wasmtime runs have.
  Each thread has its own descriptor table, which os allows: it opens, uses
  and closes a descriptor within one call. Writes are synchronous, so
  output is complete when the process ends.
- **Ending.** `proc_exit` on any thread, or `_start` returning, asks the
  main thread to end the process and blocks the thread that asked; a trap
  on any thread ends it as abort(3) would. The main thread stops every
  Worker and lets the process end with the code, rather than calling
  `process.exit`, which on Windows trips a libuv assertion while V8 still
  compiles the module in the background.
- **Engine features.** Exception handling with exnref is behind
  `--experimental-wasm-exnref` in Node 24; the host turns it on itself.
- **Engine stack.** Each Worker gets 256 MB, as wasmtime is given. Windows
  commits a Worker's stack whole when it starts, so `WASIHOST_STACK_MB`
  sets a smaller one, and difftest runs threaded modules on Node one at a
  time.

Facts the design relies on, checked with wasmtime 46 (`experiments/threads`):
an imported memory is index 0; a thread instance gets fresh defined
memories and Wasm globals; WASI I/O goes through the shared memory;
`memory.atomic.wait32` works on the main thread; a deep recursion in a
worker gets the engine stack `max-wasm-stack` allows; an exception thrown in
a worker is caught in its `wasi_thread_start`.

## Alternatives not taken

- **A host-specific spawn import** in place of `wasi.thread-spawn`, which
  would let a host answer `hardware_threads()`: every host would then have
  to implement goose-wasm's own interface (considered 2026-11-08, when the
  Node.js host was added instead).
- **Cooperative threads in one instance** (stack switching): a worker's data
  stacks would need memories of their own, whose number a module fixes,
  while workers are unbounded (spec §11.2).
- **Waiting for shared-everything-threads**: years away, per wasmtime's RFC.
