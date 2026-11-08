// reusable[] slice pools (spec §5.4): local and global pools, pool
// references as parameters, captured by a nested function and chosen
// between pools, elements with field defaults, growth in place and by a
// move, and runs placed among many free spans.

struct Cell { a: i64 = 7, b: i32 = -1 }

reusable[] var gpool: Cell[>..] = [];

fn grow(p: i64[>..]&, s: i64[:], n: i64) -> i64[:] {
    return p.realloc_slice(s, n);
}

fn churn(p: i64[>..]&, rounds: i64) -> i64 {
    var kept: (i64[:])[>..] = [];
    var sum = 0;
    for r in rounds {
        let s = p.alloc_slice(1 + r % 5);
        for j in s.len { s[j] = r; }
        kept.push(s);
        if r % 2 == 1 { p.free_slice(kept[r - 1]); }
        sum += p.index_of(s[0]);
    }
    return sum;
}

fn main() {
    reusable[] var pool: i64[>..] = [];
    let a = pool.alloc_slice(4);
    for i in a.len { a[i] = i + 1; }
    let b = pool.alloc_slice(2);
    print(pool.len, " ", pool.index_of(b[0]));
    // Grown in place where it ends the array.
    let b2 = grow(pool, b, 5);
    print(pool.len, " ", pool.index_of(b2[0]), " ", b2);
    // Not at the end, no free span after it: moved, keeping its elements.
    let a2 = grow(pool, a, 6);
    print(pool.len, " ", pool.index_of(a2[0]), " ", a2);
    // The freed front is reused first fit.
    let c = pool.alloc_slice(3);
    print(pool.index_of(c[0]), " ", c);
    // Shrinking frees the back.
    let a3 = pool.realloc_slice(a2, 2);
    let d = pool.alloc_slice(1);
    print(a3, " ", pool.index_of(d[0]), " ", pool.len);
    print(churn(pool, 40), " ", pool.len);

    // A nested function reaching the pool.
    fn take(n: i64) -> i64 { return pool.alloc_slice(n).len; }
    print(take(3), " ", pool.len);

    // A pool reference chosen between two pools.
    reusable[] var other: i64[>..] = [];
    let pick = pool.len > 100;
    let r: i64[>..]& = if pick { pool } else { other };
    let e = r.alloc_slice(3);
    r.free_slice(e[1..]);
    print(other.len, " ", r.alloc_slice(2).len, " ", other.len);

    // Field defaults fill every added element; a global pool.
    let g = gpool.alloc_slice(3);
    g[1].a = 1;
    gpool.free_slice(g[1..2]);
    let h = gpool.alloc_slice(1);
    print(gpool.index_of(h[0]), " ", h[0], " ", gpool.len);
    let h2 = gpool.realloc_slice(h, 2);
    print(gpool.index_of(h2[0]), " ", h2, " ", gpool.len);
    let lim: Cell[..4] = gpool.alloc_slice(2);
    print(lim.len, " ", lim.cap);

    // A length past any data stack aborts.
    let big = 1 << 40;
    print(pool.alloc_slice(big).len);
}
