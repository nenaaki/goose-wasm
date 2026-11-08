// runtime-define: GS_STACK_STATS=1
// Under GS_STACK_STATS each thread program reports its stacks as it ends,
// a worker's naming it, and main's the most regions a worker takes and the
// thread cap that leaves (upstream gs_stack_stats). Only stdout is
// compared; the reports go to stderr.
var log: u8[>..] = [];

thread_fn worker(var xs: i64[>..], k: i64) {
    xs.push(k);
    log.push('w');
    var tmp: u8[>..] = [];
    tmp.append("abc");
    print(xs, " ", log, " ", tmp);
}

fn main() {
    log.append("m");
    thread_wait(thread_spawn(worker, [1, 2], 3));
    thread_wait(thread_spawn(worker, [], 4));
    print(log);
}
