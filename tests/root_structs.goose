// Root structs (§3.4): each field its own storage root, a resizable one on
// a stack of its own; field access on the owner and through references to
// it; the stdlib's slot_pool, which is one.
import slot_pool;

struct P { x: i64, y: i64 }

root struct State {
    items: i64[>..] = [],
    count: i64 = 0,
    pts: P[>..<] = [],
    origin: P = P { x: 1, y: 2 },
}

fn add(s: State&, v: i64) {
    s.items.push(v);
    s.count += 1;
    s.pts.push(P { x: v, y: s.count });
}

fn total(s: State&) -> i64 {
    var t = 0;
    for x in s.items { t += x; }
    t + s.origin.x
}

fn main() {
    var s = State { .. };
    add(s, 10);
    add(s, 20);
    s.items.push(5);
    print(s.items, " ", s.count, " ", s.pts, " ", s.origin);
    print(total(s));
    var t = State { items: [1, 2], count: 7, pts: [], origin: P { x: 0, y: 0 } };
    add(t, 3);
    s.pts.pop();
    print(t.items, " ", t.count, " ", t.pts.len, " ", s.pts.len, " ", total(t));
    var pool = slot_pool<i64> {};
    let a = pool.alloc_index(100);
    let b = pool.alloc_index(200);
    pool.free(a);
    let c = pool.alloc_index(300);
    print(a, " ", b, " ", c, " ", pool.items);
}
