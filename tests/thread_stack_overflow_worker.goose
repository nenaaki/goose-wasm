// runtime-define: GS_STACK_RESERVE=65536
// A worker's data stack outgrowing its reservation ends the program with
// upstream's report, which names the worker (gs_overflow_text).

thread_fn grow(n: i64) {
    var xs: i64[>..] = [];
    for i in n { xs.push(i); }
    print(xs.len);
}

fn main() {
    thread_wait(thread_spawn(grow, 100));
    print("main goes on");
    thread_wait(thread_spawn(grow, 100000));   // Aborts.
    print("not reached");
}
