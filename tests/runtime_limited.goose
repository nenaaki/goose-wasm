// T[..] (§3.3): a limited array whose capacity is chosen at run time,
// [..n] reserving n slots or an initializer's elements filling it; the
// builtins grow it up to that capacity, and a negative one is refused.
fn fill(a: i64[..]&, n: i64) { for i in n { a.push(i * i); } }

fn main() {
    let n = 3;
    var a: i64[..] = [..n + 2];
    fill(a, 4);
    print(a, " ", a.len, " ", a.cap);
    a.pop();
    a.append([7, 8]);
    print(a, " ", a.cap, " ", a[4]);
    var b: u8[..] = "hey";
    b.clear();
    b.push('x');
    print(b, " ", b.len, " ", b.cap, " ", b == "x");
    var c: i32[..] = [1, 2, 3];
    for x, i in c { print(i, "=", x); }
    c.resize(1);
    print(c, " ", c.cap);
    a.push(100);
    print("unreachable");
}
