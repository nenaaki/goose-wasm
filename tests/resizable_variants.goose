// A resizable ADT's variant, itself resizable, as a function's result
// (§3.5), received by its ADT and forwarded with another result, to the
// caller and from further in; a variable-mode ADT result received in fixed
// mode; a dispatched call's several results returned on.
enum Value { V { tag: i32, items: i64[>..] }, W { n: i64 }, X { s: u8[] } }
enum Shape { Circle { r: i64 }, Square { s: i64 } }
enum Pick { First { k: i64 }, Second { k: i64 } }

fn show(v: Value..&) { print(v); }

fn run(n: i64) -> i64[>..] {
    var out: i64[>..] = [];
    for i in n { out.push(i * i); }
    return out;
}
recursive fn items(n: i64) -> Value.V { return Value.V { tag: n as i32, items: run(n) }; }
recursive fn some(k: i64) -> Value.. {
    if k == 0 { return items(4); }
    if k == 1 { return Value.W { n: 7 }; }
    return Value.X { s: "xs" };
}
recursive fn pair(k: i64) -> Value.V, i64 { return items(k + 1), k * 10; }
fn both(k: i64) -> Value.., i64 { return pair(k); }
recursive fn to_far(k: i64, n: i64) {
    if n > 0 { to_far(k, n - 1); }
    return pair(k + 2) from far;
}
fn far(k: i64) -> Value.., i64 { to_far(k, 1); return Value.W { n: 0 }, 0; }
recursive fn shape(r: i64) -> Shape.. { if r > 9 { Shape.Square { s: r } } else { Shape.Circle { r: r } } }
fn area(s: Shape) -> i64 { match s { Circle c => c.r * 3, Square q => q.s * q.s } }

fn pick(c: Pick.First) -> i32, i32 { return c.k as i32, -1; }
fn pick(c: Pick.Second) -> i32, i32 { return -2, c.k as i32; }
fn picked(c: Pick) -> i64, i64 { return pick(c); }

fn main() {
    var a: Value.. = items(3);
    show(a);
    a = items(5);
    show(a);
    for k in 3 {
        let s: Value.. = some(k);
        show(s);
        let t, n = both(k);
        show(t);
        let u, m = far(k);
        show(u);
        print(n, " ", m);
    }
    let f: Shape = shape(4);
    let g: Shape = shape(12);
    print(area(f), " ", area(g));
    let p, q = picked(Pick.First { k: 5 });
    let r, s = picked(Pick.Second { k: 6 });
    print(p, " ", q, " ", r, " ", s);
}
