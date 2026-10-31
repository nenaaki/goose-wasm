// T[] of fixed-size elements held on its own (§3.3): a variable from a
// literal, a string, a slice or a call; indexed, iterated, sliced, compared,
// printed; passed and returned; its length checked against its field.
fn total(xs: i64[:]) -> i64 { var t = 0; for x in xs { t += x; } t }
fn sum(xs: i64[]) -> i64 { var t = 0; for x in xs { t += x; } t }
fn make(n: i64) -> i64[] {
    var out: i64[>..] = [];
    for i in n { out.push(i * 10); }
    return out;
}
fn bytes(n: i64) -> u8[] {
    var out: u8[>..] = [];
    for i in n { out.push(1); }
    return out;
}

fn main() {
    let a: i64[] = [1, 2, 3];
    var b: i64[] = [4, 5];
    b[0] = 40;
    let s: u8[] = "item_1234";
    print(a, " ", b, " ", s, " ", a.len, " ", s[2], " ", a[1..], " ", total(a), " ", sum(copy(a)));
    let m = make(4);
    print(m, " ", total(m), " ", total(make(2)), " ", m == [0, 10, 20, 30]);
    let w: u16[] = [3, 5, 65535];
    for x, i in w { print(i, ": ", x); }
    let c: i64[] = m[1..3];
    print(c, " ", s == "item_1234", " ", s[..4] == "item");
    let ok: u8[u8] = bytes(255);
    print(ok.len);
    let bad: u8[u8] = bytes(256);
    print(bad.len);
}
