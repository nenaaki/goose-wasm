// Several results of other kinds (§7.3): variable-size values built at the
// top of a stack the caller names, references to resizable arrays as their
// header and stack, references bound by their pointees' names (copies, or
// the pointee itself with `.=`), forwarded with `return f()`, from a
// function returning a reference to a resizable array, and used for their
// first value alone.
root struct Ctx { args: i64[>..<], other: i64[>..<] }
struct P { a: i64, b: i64 }
enum Shape { Circle { r: i64 }, Label { text: u8[] } }

fn texts(n: i64) -> u8[], i64, u8[] { return str("n=", n), n * 2, "tail"; }
fn shapes(k: i64) -> Shape.., varint[] {
    if k > 0 { return Shape.Label { text: str(k) }, [k, -k]; }
    return Shape.Circle { r: 1 }, [];
}
fn fwd(n: i64) -> u8[], i64, u8[] { return texts(n + 1); }
fn both(c: Ctx&) -> i64[>..<]&, i64[>..<]& { return c.args, c.other; }
fn pick(c: Ctx&, first: bool) -> i64[>..<]& {
    if first { return c.args; }
    return c.other;
}
fn refs(p: P&, x: i64&) -> P&, i64& { return p, x; }
fn pair(k: i64) -> P, i64 { return P { a: k, b: -k }, k * 3; }

fn main() {
    let a, n, b = texts(4);
    print(a, " ", n, " ", b);
    let c, m, d = fwd(7);
    print(c, " ", m, " ", d);
    for k in 2 {
        let s, xs = shapes(k);
        print(s, " ", xs);
    }
    var ctx = Ctx { args: [1], other: [] };
    let x, y = both(ctx);
    x.push(2);
    y.push(3);
    pick(ctx, false).push(4);
    var cp = copy(pick(ctx, true));
    cp.push(99);
    print(ctx.args, " ", ctx.other, " ", cp);
    var p = P { a: 1, b: 2 };
    var v = 5;
    let pc, vc = refs(p, v);
    let pr, vr .= refs(p, v);
    p.a = 10;
    v = 6;
    pr.b = 20;
    print(pc, " ", vc, " ", pr, " ", vr, " ", p);
    var arr: P[>..] = [];
    arr.push(pair(3));
    print(arr, " ", pair(4).b);
}
