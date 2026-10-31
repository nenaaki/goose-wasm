// Limited arrays T[..k] of static capacity (§4.2): { len, e[k] } in place,
// literals and runs copied in, the builtins that change the length, the
// capacity checked as they grow, iteration, indexing and rendering.
struct P { x: i32, y: i32 }
struct Rec { tag: u8, names: u8[..8], pts: P[..3] }

var gl: i64[..4] = [7, 8];

fn sum(xs: i64[..6]&) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    s
}

fn fill(n: i64) -> i64[..6] {
    var r: i64[..6] = [];
    for i in n { r.push(i * i); }
    r
}

fn main() {
    var a: i64[..6] = [1, 2, 3];
    a.push(4);
    print(a, " ", a.len, " ", a.cap, " ", sum(a));
    print(a.pop(), " ", a);
    a.append([10, 20]);
    print(a, " ", a[3]);
    a.pop_n(2);
    a.resize(5, 9);
    print(a);
    a.resize(1);
    a.clear();
    print(a, " ", a.len);
    let f = fill(4);
    print(f, " ", sum(f));
    var s: u8[..16] = "hi";
    s.append(" there");
    s.push(33);
    print(s, " ", s.len);
    var r = Rec { tag: 1, names: "bob", pts: [P { x: 1, y: 2 }] };
    r.pts.push(P { x: 3, y: 4 });
    r.names.append("!");
    print(r);
    var copy = r;
    copy.pts.clear();
    print(r.pts.len, " ", copy.pts.len);
    gl.push(9);
    for &x in gl { x += 1; }
    print(gl);
    var full: u8[..2] = [1, 2];
    full.push(3);
    print("unreachable");
}
