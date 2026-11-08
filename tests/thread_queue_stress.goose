// Many workers putting and taking at once (spec §11.2): every message is
// received exactly once, whatever the interleaving, so the totals are
// deterministic. Exercises the runtime's lock and its heap's reuse of
// queue nodes of several sizes.
struct Done { worker: i64, sum: i64 }

thread_fn producer(w: i64, n: i64) {
    for i in n {
        qput(w * 100000 + i);
        var word: u8[>..] = [];
        for k in i % 7 { word.push('a' + k as u8); }
        qput(word);
    }
}

thread_fn consumer(n: i64) {
    var sum = 0;
    var letters = 0;
    for i in n {
        sum += qget<i64>();
        letters += qget<u8[>..]>().len;
    }
    qput(Done { worker: n, sum: sum + letters });
}

fn main() {
    let workers = 4;
    let n = 3000;
    var ids: i64[>..] = [];
    for w in workers { ids.push(thread_spawn(producer, w, n)); }
    // Two consumers share the work; the queues interleave the producers.
    let c1 = thread_spawn(consumer, workers * n / 2);
    let c2 = thread_spawn(consumer, workers * n / 2);
    for id in ids { thread_wait(id); }
    let a = qget<Done>();
    let b = qget<Done>();
    print(thread_wait(c1) && thread_wait(c2), " ", a.sum + b.sum);
    let left, any = qpoll<i64>();
    print(any, " ", left);
}
