// Structs ending in a resizable array (frame objects, Appendix C.2) as
// values: returned from calls (built where the caller wants them), copied,
// assigned whole (the tail rebuilt, the new value read before the old is
// replaced), nested as another frame object's tail, as globals.
struct Bag { tag: i64, items: i64[>..] }
struct Outer { id: i64, bag: Bag }

var gbag: Bag = Bag { tag: 9, items: [90, 91] };
var gout: Outer = Outer { id: 1, bag: Bag { tag: 2, items: [] } };

fn make(n: i64) -> Bag {
    var b = Bag { tag: n, items: [] };
    for i in n { b.items.push(i * 10); }
    return b;
}
fn choose(c: bool) -> Bag { if c { make(2) } else { Bag { tag: 0, items: [7; 3] } } }
fn reset(b: Bag&, n: i64) { b = Bag { tag: n + 100, items: [n, n] }; }

fn main() {
    var a = make(3);
    print(a, " ", a.items.len);
    var b = copy(a);
    b.items.push(99);
    print(a, " ", b);
    a = make(2);
    print(a);
    let k = a.tag * 10;
    a = Bag { tag: k, items: [k, 5] };
    print(a);
    reset(a, 4);
    print(a, " ", choose(true), " ", choose(false));
    var o = Outer { id: 5, bag: make(1) };
    o.bag.items.push(42);
    print(o, " ", o.bag.items.len);
    gout.bag.items.push(3);
    gbag.items.push(92);
    print(gout, " ", gbag);
    for i in 3 {
        var t = make(i);
        t = make(i + 1);
        print(t.items.len);
    }
}
