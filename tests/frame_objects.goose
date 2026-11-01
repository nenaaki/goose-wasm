// Frame objects (structs with a resizable tail, spec Appendix C.2) and fat
// references held in struct fields: fixed fields beside the tail, pushes to
// the tail directly and through references to the object and to a holder.
struct Pile { n: u8, w: i32, items: i64[>..] }
struct Holder { k: i64, items: i64[>..]& }

fn add(p: Pile&, v: i64) {
    p.items.push(v);
    p.n++;
}

fn sum(p: Pile&) -> i64 {
    var s: i64 = 0;
    for x in p.items { s += x; }
    s + p.w
}

fn fill(h: Holder&, n: i64) {
    for i in 0..n { h.items.push(i * h.k); }
}

fn main() {
    var p = Pile { n: 1, w: 1000, items: [10, 20] };
    p.items.push(30);
    add(p, 40);
    print(p.n, " ", p.items.len, " ", p.items[3], " ", sum(p));
    var xs: i64[>..] = [];
    var h = Holder { k: 7, items: xs };
    fill(h, 4);
    h.items.push(99);
    print(xs.len, " ", xs[3], " ", h.items[4]);
    for round in 0..3 {
        var q = Pile { n: 0, w: round as i32, items: [] };
        add(q, round * 2);
        print(sum(q));
    }
}
