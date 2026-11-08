// A worker an export spawns, not contained, aborting while the host's
// thread waits for it in the export: the abort ends the process (§11.2).

thread_fn fail(n: i64) {
    let xs = [1, 2, 3];
    print(xs[n]);
}

export fn run(n: i64) -> i64 {
    thread_wait(thread_spawn(fail, n));
    return n;
}
