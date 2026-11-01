// Resizable arrays and structs ending in one as several results of a call
// (each built where the caller says, on a stack it names), as parameters
// taken by value (a copy the caller makes, on a stack of its own), and
// through case-function dispatch (§7.3, §8.2).
enum Choice { One, Two }
struct Bag { tag: i64, items: i64[>..] }
struct Rec { name: u8[], items: i64[>..<] }

fn pair(k: i64) -> i64[>..], i64, Bag {
    var xs: i64[>..] = [k];
    xs.push(k + 1);
    return xs, k * 10, Bag { tag: k, items: [k, k, k] };
}
fn three() -> Rec, i64[>..<], i64 {
    return Rec { name: "rec", items: [1] }, [5, 6], 7;
}
fn total(var s: Bag) -> i64 {
    s.items.push(100);
    var t = s.tag;
    for x in s.items { t += x; }
    return t;
}
fn grow(var xs: i64[>..], k: i64) -> i64 {
    for i in 0..k { xs.push(i); }
    return xs.len;
}
fn named(var r: Rec) -> i64 { r.items.push(3); return r.items.len + r.name.len; }
fn pick(c: Choice.One, var b: Bag) -> Bag, i64 {
    b.items.push(1);
    return b, 1;
}
fn pick(c: Choice.Two, var b: Bag) -> Bag, i64 {
    b.items.pop();
    return Bag { tag: b.tag * 2, items: [b.items.len] }, 2;
}

fn main() {
    let a, n, b = pair(3);
    print(a, " ", n, " ", b);
    var x, m, y = pair(4);
    x.push(1);
    y.items.push(2);
    print(x, " ", m, " ", y);
    var r, zs, q = three();
    r.items.push(2);
    zs.pop();
    print(r, " ", zs, " ", q);

    var bag = Bag { tag: 1, items: [2, 3] };
    var xs: i64[>..] = [1];
    print(total(copy(bag)), " ", bag, " ", grow(copy(xs), 3), " ", xs);
    print(total(Bag { tag: 5, items: [] }), " ", grow([9, 9], 1));
    print(named(Rec { name: "zz", items: [] }), " ", r);

    check(Choice.One {}, bag);
    check(Choice.Two {}, bag);
}

fn check(c: Choice, bag: Bag&) {
    var p, k = pick(c, copy(bag));
    p.items.push(k);
    print(p, " ", k, " ", bag);
}
