# wasi-threads probes

What `docs/design/threads.md` relies on, checked on wasmtime 46.0.1 (the
last version with wasi-threads) on 2026-11-08. Run with:

```sh
wasm-tools parse X.wat -o X.wasm
wasmtime run -S threads=y -W threads=y,shared-memory=y,multi-memory=y,exceptions=y,max-wasm-stack=268435456 X.wasm
```

- `wasi-threads-layout.wat`: an imported shared memory is memory 0; each
  thread's instance gets fresh defined memories and Wasm globals; main can
  block in `memory.atomic.wait32`; and WASI's `fd_write` reads its iovec and
  bytes from the shared memory, not from the memory exported as `memory`
  (the threads print garbled text, which shows it).
- `wasi-threads-unwind.wat`: a worker recursing a million deep gets the
  engine stack `max-wasm-stack` allows, and an exception thrown at the
  bottom is caught in its `wasi_thread_start`.
