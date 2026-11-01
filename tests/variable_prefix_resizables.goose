// Structs ending in a resizable array after a variable-size field (Appendix
// C.2): their fields' image then the tail's elements on a data stack. Built
// from literals and results, read field by field, assigned whole through a
// reference, the tail grown and shrunk.
struct Rec { k: i64, name: u8[], n: varint, items: i64[>..<] }

fn mk(s: const u8[:], c: i64) -> Rec {
    var r = Rec { k: c, name: s, n: 300, items: [] };
    for i in 0..c { r.items.push(i * 10); }
    return r;
}

fn reset(r: Rec&) {
    r = Rec { k: 0, name: "ABCDEFGHIJ", n: 70000, items: [9] };
    r.items.push(10);
}

fn pick(b: bool) -> Rec {
    if b { mk("yes", 1) } else { Rec { k: -1, name: "no", n: 1, items: [5, 6] } }
}

fn main() {
    var a = Rec { k: 3, name: "ab", n: 300, items: [7] };
    a.k = 4;
    a.items.push(8);
    print(a, " ", a.k + a.n, " ", a.name.len);
    for x in a.items { print(x); }
    reset(a);
    print(a);
    a.items.pop();
    print(a.items.len, " ", a.name);
    a = mk("xyz", 3);
    print(a);
    let b = mk("q", 2);
    print(b, " ", b.items[1]);
    print(pick(true), " ", pick(false));
    var c = pick(false);
    c.items.push(c.k);
    print(c);
}
