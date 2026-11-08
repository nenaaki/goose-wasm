// export fns called from a host: scalars of every width, fixed values by
// value and as results, references, slices, and a returned const u8[:]
// from static data and from a global's stack. main runs inside goose_init,
// once however often the host calls it.

struct P { x: i32, y: i32 }
struct Box { lo: P, hi: P, tag: u8 }

var calls: i32 = 0;
var log: u8[>..] = [];

fn main() {
    calls = 100;
    print("main ran");
}

export "c_add" fn add(a: i32, b: i32) -> i32 {
    calls += 1;
    return a + b;
}

export fn narrow(a: i8, b: u8, c: i16, d: u16, e: bool) -> i32 {
    return (a as i32) * 1000000 + (b as i32) * 1000 + c + d + (if e { 7 } else { 0 });
}

export fn wide(a: i64, b: u64) -> u64 { return (a as! u64) + b; }

export fn halve(a: f64, b: f32) -> f64 { return a / 2.0 + b; }

export fn third(a: f32) -> f32 { return a / 3.0; }

export fn yes(n: i32) -> bool { return n > 0; }

export fn small(n: i32) -> i8 { return n as! i8; }

export fn swap(p: P) -> P { return P { p.y, p.x }; }

export fn span(b: Box) -> Box {
    return Box { P { b.lo.x - 1, b.lo.y - 1 }, P { b.hi.x + 1, b.hi.y + 1 }, b.tag + 1 };
}

export fn bump(p: P&) {
    p.x += 1;
    p.y *= 2;
}

export fn total(xs: i32[:]) -> i64 {
    var t: i64 = 0;
    for x in xs { t += x; }
    return t;
}

export fn count_byte(s: const u8[:], c: u8) -> i32 {
    var n: i32 = 0;
    for b in s { if b == c { n += 1; } }
    return n;
}

export fn hello() -> const u8[:] { return "Hello, world!"; }

export fn remember(s: const u8[:]) -> const u8[:] {
    log.append(s);
    log.append(";");
    return log;
}

export fn count() -> i32 { return calls; }
