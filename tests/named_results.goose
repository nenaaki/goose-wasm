// A named result (§7.3): the top-level local every return hands back is
// built where the result goes, both in a called function and in its inlined
// form; a return of another value builds that one there instead, and an
// early return of the local hands back what it holds then; a recursive
// function's too.
fn make(n: i64) -> i64[..] {
    var result: i64[..] = [..n + 2];
    for i in n { result.push(i * 10); }
    return result;
}
fn early(n: i64) -> i64[..] {
    var result: i64[..] = [..8];
    for i in 8 {
        if i == n { return result; }
        result.push(i);
    }
    return [..1];
}
fn rebound(n: i64) -> u8[] {
    let out: u8[] = if n > 1 { "rebound" } else { "start" };
    if n > 2 { return "other"; }
    out
}
recursive fn deep(n: i64) -> u8[] {
    if n > 0 { return deep(n - 1); }
    let s: u8[] = "deep";
    return s;
}
fn main() {
    var out: i64[>..] = [99];
    for k in 0..4 { out.append(if k == 0 { make(0) } else { make(k) }); }
    print(out);
    for n in 0..10 {
        let e = early(n);
        print(e.len, " ", e.cap, " ", e);
    }
    for n in 4 { print(rebound(n)); }
    print(deep(3));
}
