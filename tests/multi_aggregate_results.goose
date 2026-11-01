// Several results some of which are aggregates (§7.2): a slice, a struct
// or an array is built where the caller's variable is; the scalars come
// back as values.
struct P { x: i64, y: i64 }
fn cut(s: const u8[:], n: i64) -> const u8[:], const u8[:] { return s[..n], s[n..]; }
fn stats(xs: i64[:]) -> i64, P, bool {
    var lo = xs[0];
    var hi = xs[0];
    for x in xs { if x < lo { lo = x; } if x > hi { hi = x; } }
    return xs.len, P { x: lo, y: hi }, lo < 0;
}
fn pair(n: i64) -> i64[2], i64 { return [n, n * n], n + 1; }
fn main() {
    let head, tail = cut("hello world", 5);
    print(head, "|", tail);
    let n, range, neg = stats([3, -1, 7, 2]);
    print(n, " ", range, " ", neg);
    let a, b = pair(4);
    print(a, " ", b);
    var h: const u8[:] = "";
    var t: const u8[:] = "";
    h, t = cut("abcdef", 2);
    print(h, " ", t);
    stats([1]);
}
