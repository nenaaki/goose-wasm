// os from workers: each writes a file of its own and reports through a
// queue; main reads them back. With threads, WASI's calls go through each
// thread's area in the shared memory.
import os;

thread_fn writer(k: i64) {
    let name = str("gs_thread_os_", k, ".txt");
    var text: u8[>..] = [];
    for i in k + 1 { text.append("x"); }
    qput(write_file(name, text));
    sleep_ms(1);
}

fn main() {
    var ids: i64[>..] = [];
    for k in 4 { ids.push(thread_spawn(writer, k)); }
    var ok = true;
    for k in 4 { ok = ok && qget<bool>(); }
    for id in ids { thread_wait(id); }
    var total = 0;
    for k in 4 {
        let name = str("gs_thread_os_", k, ".txt");
        var b: u8[>..] = [];
        read_file(name, b);
        total += b.len;
        delete_file(name);
    }
    print(ok, " ", total, " ", clock_ns() > 0);
}
