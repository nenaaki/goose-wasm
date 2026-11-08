// Export fns with workers and queues (spec §7.11, §11.2): a host calls
// exports that spawn workers, wait for them and pass values over a typed
// queue, and a worker main started stays running between the host's calls.

struct Sq { i: i64, v: i64 }
struct Ping { n: i64 }
struct Pong { n: i64 }

var started = 0;

thread_fn square(i: i64) { qput(Sq { i: i, v: i * i }); }

thread_fn check(n: i64) { assert(n >= 0); }

// Answers each Ping with a Pong until a negative one.
thread_fn echo() {
    var going = true;
    while going {
        let p = qget<Ping>();
        going = p.n >= 0;
        if going { qput(Pong { n: p.n * 10 }); }
    }
}

var echoer: i64 = 0;

fn main() {
    echoer = thread_spawn(echo);
    started = 1;
}

// The sum of i * i for i below n, each square from a worker of its own.
export fn sum_squares(n: i64) -> i64 {
    var ids: i64[>..] = [];
    for i in n { ids.push(thread_spawn(square, i)); }
    for id in ids { thread_wait(id); }
    var t = 0;
    for i in n { t += qget<Sq>().v; }
    return t;
}

// Whether a contained worker checking n returned: false when it aborted.
export fn contained(n: i64) -> bool {
    return thread_wait(thread_spawn_contained(check, n));
}

// The echo worker main started answers n.
export fn ask(n: i64) -> i64 {
    qput(Ping { n: n });
    return qget<Pong>().n;
}

// Ends the echo worker and waits for it.
export fn stop() -> i64 {
    qput(Ping { n: -1 });
    thread_wait(echoer);
    return started;
}
