// Resizable arrays as values: function results built on the destination
// stack (spec §7.3) through literals, branches, returned locals, calls and
// early returns; an un-annotated var binding by reference (§4.1) and
// assignment through it; append of literals,
// slices, strings and call results; discarded and temporary results.
fn squares(n: i64) -> i64[>..] {
    var out: i64[>..] = [];
    for i in 0..n { out.push(i * i); }
    return out;
}

fn pick(c: bool) -> i64[>..] {
    if c { [1, 2, 3] } else { squares(2) }
}

fn early(n: i64) -> i64[>..] {
    if n < 0 { return []; }
    var acc: i64[>..] = [n];
    acc.append(squares(n));
    acc
}

fn total(s: i64[:]) -> i64 {
    var t: i64 = 0;
    for x in s { t += x; }
    t
}

fn greet(name: u8[:]) -> u8[>..] {
    var s: u8[>..] = [];
    s.append("hello, ");
    s.append(name);
    s.push('!');
    s
}

fn main() {
    let a = squares(5);
    print(a.len, " ", a[4], " ", total(a));
    var b = pick(true);
    b.append(pick(false));
    print(b.len, " ", total(b));
    var c = a;
    c.push(100);
    print(a.len, " ", c.len, " ", total(c));
    c = early(3);
    print(c.len, " ", total(c), " ", early(-1).len);
    c.append([7, 8]);
    c.append(a[1..3]);
    print(c.len, " ", total(c));
    squares(1000);
    print(total(squares(4)), " ", greet("goose"));
    var fill = [9; 4];
    var many: i64[>..] = [5; 3];
    many.append(fill);
    print(many.len, " ", total(many));
}
