// Reusable pools (§5.4, deprecated): alloc_index and alloc_ref take a freed
// slot first, else a fresh one; free checks the index; a pool reference
// parameter carries the freelist; a global pool has one of its own.
struct Node { v: i64, next: i64 }

reusable var gpool: Node[>..] = [];

fn take(p: Node[>..]&, v: i64) -> i64 { p.alloc_index(Node { v: v, next: -1 }) }
fn drop(p: Node[>..]&, i: i64) { p.free(i); }

fn main() {
    reusable var pool: i64[>..] = [];
    let a = pool.alloc_index(10);
    let b = pool.alloc_index(20);
    let c = pool.alloc_index(30);
    pool.free(b);
    pool.free(a);
    let d = pool.alloc_index(40);
    let r .= pool.alloc_ref(50);
    r += 5;
    let e = pool.alloc_index(60);
    print(a, " ", b, " ", c, " ", d, " ", e, " ", pool);
    let x = take(gpool, 1);
    let y = take(gpool, 2);
    drop(gpool, x);
    let z = take(gpool, 3);
    print(x, " ", y, " ", z, " ", gpool);
    pool.free(7);
}
