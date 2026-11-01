// Variable-mode ADTs with a variant ending in a resizable array or a frame
// object (§3.5): built from literals, constants, calls and branches; their
// tags matched and other payloads bound by value; printed; replaced whole,
// as locals, struct tails, globals and through references.
struct Stack { n: i64, items: i64[>..<] }
enum Value { Num { n: i64 }, Text { s: u8[], k: varint }, List { tag: u8[], items: i64[>..] },
             Framed { k: i64, st: Stack }, Nothing }
struct Holder { id: i64, name: u8[], v: Value.. }

fn mk(k: i64) -> Value.. {
    if k % 3 == 0 { return Value.List { tag: "l", items: [k, k + 1] }; }
    if k % 3 == 1 { return Value.Text { s: "text", k: k * 1000 }; }
    return Value.Framed { k: k, st: Stack { n: k * 2, items: [k] } };
}

fn describe(v: Value..&) -> i64 {
    match v {
        Num x => x.n,
        Text t => t.s.len + t.k,
        List => -1,
        Framed => -2,
        Nothing => 0,
    }
}

fn swap(h: Holder&) {
    match h.v {
        Text t => {
            h.v = Value.List { tag: copy(t.s), items: [t.k] };
            print("was text ", t.s, " ", t.k);
        },
        _ => { h.v = Value.Nothing; },
    }
}

var g: Value.. = Value.List { tag: "global", items: [1] };

fn main() {
    var a: Value.. = Value.Num { n: 5 };
    print(a, " ", describe(a));
    a = Value.List { tag: "xs", items: [1, 2, 3] };
    print(a, " ", describe(a));
    a = Value.Framed { k: 9, st: Stack { n: 8, items: [7, 6] } };
    print(a, " ", describe(a));
    a = Value.Nothing;
    print(a);
    for k in 0..4 {
        var b = mk(k);
        print(b, " ", describe(b));
    }
    var c: Value.. = if describe(a) == 0 { mk(3) } else { Value.Num { n: 0 } };
    print(c);
    var h = Holder { id: 1, name: "holder", v: Value.Text { s: "abc", k: 300 } };
    print(h);
    swap(h);
    print(h, " ", h.id, " ", h.name);
    swap(h);
    print(h);
    h.v = mk(5);
    h.id = 2;
    print(h);
    print(g);
    g = mk(6);
    print(g, " ", describe(g));
}
