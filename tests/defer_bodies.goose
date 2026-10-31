// defer (§6.6) runs its body at every exit of its block, latest first,
// innermost block first, reading variables as they are then; a loop body's
// at each iteration. Also: an aggregate loop value, a string literal into
// u8[k], copy of a scalar, and exit.
struct P { x: i64, y: i64 }
fn f(n: i64) -> i64 {
    var a = 1;
    defer print("f done a=", a);
    defer { a += 100; print("second ", a); }
    if n > 0 {
        defer print("inner ", n);
        return n * 2;
    }
    a = 5;
    0
}
fn main() {
    print(f(3));
    print(f(0));
    for i in 3 {
        defer print("iter ", i);
        if i == 1 { continue; }
        print("body ", i);
    }
    let p = loop { break P { x: 1, y: 2 }; };
    var w: u8[3] = "abc";
    let k = 7;
    print(p, " ", w, " ", copy(k));
    exit(0);
    print("unreachable");
}
