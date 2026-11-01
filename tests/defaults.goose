// default<T>() (§4.2): zero bytes for a type with no declared field
// default, the declared defaults where there are, per element of an array;
// scalars, optionals, ADTs (their first variant), limited arrays; `..` in
// a literal takes the rest from the defaults.
struct P { x: i64, y: i32 }
struct D { a: i64 = 7, b: bool, c: P = P { x: 1, y: 2 } }
struct Cell { v: i64 }
enum Shape { Circle { r: i64 = 3 }, Dot }

fn main() {
    print(default<i64>(), " ", default<bool>(), " ", default<f64>());
    let p = default<P>();
    let d = default<D>();
    let ds = default<D[2]>();
    let ps = default<P[3]>();
    print(p, " ", d, " ", ds, " ", ps);
    let c: Cell? = default<Cell?>();
    print(c == null);
    print(default<Shape>());
    let l = default<i64[..4]>();
    print(l, " ", l.len);
    let e = D { b: true, .. };
    print(e);
}
