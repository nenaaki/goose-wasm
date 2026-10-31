// Fixed structs: literals, field reads and writes, copies (assignment reads
// the old value), nesting, by-value parameters, results, `pad`.
struct P { x: i32, y: i32 }
struct R { a: P, b: P, tag: u8 }
struct Padded { a: u8, pad, b: i64, c: i16 }

fn swap(p: P) -> P { P { x: p.y, y: p.x } }

fn area(r: R) -> i64 {
    let w = r.b.x - r.a.x;
    let h = r.b.y - r.a.y;
    return w * h;
}

fn bump(p: P) -> i32 {
    var q = p;
    q.x += 100;
    q.x
}

fn pick(c: bool, a: P, b: P) -> P { if c { a } else { b } }

fn main() {
    var p = P { x: 1, y: 2 };
    p = P { x: p.y, y: p.x };
    print(p.x, " ", p.y);
    let s = swap(p);
    print(s.x, " ", s.y, " ", bump(s), " ", s.x);
    var r = R { a: P { x: 1, y: 1 }, b: s, tag: 7 };
    r.b.y *= 10;
    r.tag++;
    print(area(r), " ", r.tag, " ", r.b.y);
    let c = r.a;
    r.a.x = 50;
    print(c.x, " ", r.a.x, " ", pick(true, c, r.a).x, " ", pick(false, c, r.a).x);
    let d = Padded { a: 1, b: 2, c: 3 };
    print(d.a + d.b + d.c);
}
